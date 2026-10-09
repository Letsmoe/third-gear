#include "WorldMeshBuilder.h"

#include "DynamicMesh/DynamicMeshAttributeSet.h"

using namespace UE::Geometry;

namespace
{
/** Face direction of a triangle by the engine's convention (unnormalised). */
FVector3f FaceDirection(const FVector3f& A, const FVector3f& B, const FVector3f& C)
{
	return FVector3f::CrossProduct(C - A, B - A);
}

/** Per-vertex normals: area-weighted sum of the face directions around each vertex. */
TArray<FVector3f> ComputeVertexNormals(const TArray<FVector3f>& Positions, const TArray<FIntVector3>& Triangles)
{
	TArray<FVector3f> Normals;
	Normals.SetNumZeroed(Positions.Num());
	for (const FIntVector3& Triangle : Triangles)
	{
		const FVector3f Face = FaceDirection(Positions[Triangle.X], Positions[Triangle.Y], Positions[Triangle.Z]);
		Normals[Triangle.X] += Face;
		Normals[Triangle.Y] += Face;
		Normals[Triangle.Z] += Face;
	}
	for (FVector3f& Normal : Normals)
	{
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
	}
	return Normals;
}

/** Per-vertex tangent frame from the UV layout: tangent along increasing U, bitangent along increasing V. */
struct FTangentFrames
{
	TArray<FVector3f> Tangents;
	TArray<FVector3f> Bitangents;
};

/** Tangents made orthogonal to the normals; bitangents are Cross(Normal, Tangent) flipped to follow the V direction. */
FTangentFrames ComputeTangentFrames(const TArray<FVector3f>& Positions, const TArray<FVector2f>& UVs,
	const TArray<FIntVector3>& Triangles, const TArray<FVector3f>& Normals)
{
	FTangentFrames Frames;
	Frames.Tangents.SetNumZeroed(Positions.Num());
	Frames.Bitangents.SetNumZeroed(Positions.Num());
	for (const FIntVector3& Triangle : Triangles)
	{
		const FVector3f Edge1 = Positions[Triangle.Y] - Positions[Triangle.X];
		const FVector3f Edge2 = Positions[Triangle.Z] - Positions[Triangle.X];
		const FVector2f DeltaUV1 = UVs[Triangle.Y] - UVs[Triangle.X];
		const FVector2f DeltaUV2 = UVs[Triangle.Z] - UVs[Triangle.X];
		const float Determinant = DeltaUV1.X * DeltaUV2.Y - DeltaUV2.X * DeltaUV1.Y;
		if (FMath::Abs(Determinant) < 1e-12f)
		{
			continue;
		}
		const FVector3f Tangent = (Edge1 * DeltaUV2.Y - Edge2 * DeltaUV1.Y) / Determinant;
		const FVector3f Bitangent = (Edge2 * DeltaUV1.X - Edge1 * DeltaUV2.X) / Determinant;
		for (const int32 Vertex : {Triangle.X, Triangle.Y, Triangle.Z})
		{
			Frames.Tangents[Vertex] += Tangent;
			Frames.Bitangents[Vertex] += Bitangent;
		}
	}
	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		const FVector3f& Normal = Normals[Index];
		FVector3f Tangent = Frames.Tangents[Index] - Normal * FVector3f::DotProduct(Normal, Frames.Tangents[Index]);
		if (!Tangent.Normalize())
		{
			const FVector3f Reference = FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector;
			Tangent = FVector3f::CrossProduct(Normal, Reference).GetSafeNormal();
		}
		FVector3f Bitangent = FVector3f::CrossProduct(Normal, Tangent);
		if (FVector3f::DotProduct(Bitangent, Frames.Bitangents[Index]) < 0.f)
		{
			Bitangent = -Bitangent;
		}
		Frames.Tangents[Index] = Tangent;
		Frames.Bitangents[Index] = Bitangent;
	}
	return Frames;
}
}

int32 FWorldMeshBuilder::AddVertex(const FVector3f& Position, const FVector2f& UV, const FColor& Color)
{
	Positions.Add(Position);
	UVs.Add(UV);
	Colors.Add(Color);
	return Positions.Num() - 1;
}

void FWorldMeshBuilder::AddTriangleRaw(int32 A, int32 B, int32 C, int32 Material)
{
	if (A == B || B == C || A == C)
	{
		return;
	}
	Triangles.Add(FIntVector3(A, B, C));
	TriangleMaterials.Add(Material);
}

void FWorldMeshBuilder::AddTriangle(int32 A, int32 B, int32 C, int32 Material, const FVector3f& FacingHint)
{
	const FVector3f Face = FaceDirection(Positions[A], Positions[B], Positions[C]);
	if (FVector3f::DotProduct(Face, FacingHint) < 0.f)
	{
		Swap(B, C);
	}
	AddTriangleRaw(A, B, C, Material);
}

void FWorldMeshBuilder::AddQuad(int32 A, int32 B, int32 C, int32 D, int32 Material, const FVector3f& FacingHint)
{
	AddTriangle(A, B, C, Material, FacingHint);
	AddTriangle(A, C, D, Material, FacingHint);
}

namespace
{
/** Writes triangles of a builder into one dynamic mesh, copying each builder vertex in on first use. */
class FDynamicMeshWriter
{
public:
	FDynamicMeshWriter(const TArray<FVector3f>& InPositions, const TArray<FVector2f>& InUVs, const TArray<FColor>& InColors,
		const TArray<FVector3f>& InNormals, const FTangentFrames& InFrames)
		: Positions(InPositions), UVs(InUVs), Colors(InColors), Normals(InNormals), Frames(InFrames)
	{
		Mesh.EnableAttributes();
		FDynamicMeshAttributeSet& Attributes = *Mesh.Attributes();
		Attributes.EnablePrimaryColors();
		Attributes.EnableMaterialID();
		Attributes.EnableTangents();
		VertexMap.Init(INDEX_NONE, Positions.Num());
	}

	/** Adds a triangle of builder vertices; vertices that would make an edge non-manifold are split. */
	void AddTriangle(const FIntVector3& Source, int32 Material)
	{
		FIndex3i Triangle(VertexFor(Source.X), VertexFor(Source.Y), VertexFor(Source.Z));
		int32 TriangleID = Mesh.AppendTriangle(Triangle);
		if (TriangleID == FDynamicMesh3::NonManifoldID)
		{
			Triangle = FIndex3i(AppendCopy(Source.X), AppendCopy(Source.Y), AppendCopy(Source.Z));
			TriangleID = Mesh.AppendTriangle(Triangle);
		}
		if (TriangleID < 0)
		{
			return;
		}
		FDynamicMeshAttributeSet& Attributes = *Mesh.Attributes();
		Attributes.PrimaryUV()->SetTriangle(TriangleID, Triangle);
		Attributes.PrimaryNormals()->SetTriangle(TriangleID, Triangle);
		Attributes.PrimaryTangents()->SetTriangle(TriangleID, Triangle);
		Attributes.PrimaryBiTangents()->SetTriangle(TriangleID, Triangle);
		Attributes.PrimaryColors()->SetTriangle(TriangleID, Triangle);
		Attributes.GetMaterialID()->SetValue(TriangleID, Material);
	}

	FDynamicMesh3 Mesh;

private:
	/** This mesh's copy of a builder vertex, created on first use. Element IDs equal vertex IDs. */
	int32 VertexFor(int32 Source)
	{
		if (VertexMap[Source] == INDEX_NONE)
		{
			VertexMap[Source] = AppendCopy(Source);
		}
		return VertexMap[Source];
	}

	int32 AppendCopy(int32 Source)
	{
		FDynamicMeshAttributeSet& Attributes = *Mesh.Attributes();
		const int32 Vertex = Mesh.AppendVertex(FVector3d(Positions[Source]));
		Attributes.PrimaryUV()->AppendElement(UVs[Source]);
		Attributes.PrimaryNormals()->AppendElement(Normals[Source]);
		Attributes.PrimaryTangents()->AppendElement(Frames.Tangents[Source]);
		Attributes.PrimaryBiTangents()->AppendElement(Frames.Bitangents[Source]);
		const FColor& Color = Colors[Source];
		Attributes.PrimaryColors()->AppendElement(FVector4f(Color.R / 255.f, Color.G / 255.f, Color.B / 255.f, Color.A / 255.f));
		return Vertex;
	}

	const TArray<FVector3f>& Positions;
	const TArray<FVector2f>& UVs;
	const TArray<FColor>& Colors;
	const TArray<FVector3f>& Normals;
	const FTangentFrames& Frames;
	/** Builder vertex to this mesh's vertex, or INDEX_NONE. */
	TArray<int32> VertexMap;
};
}

FDynamicMesh3 FWorldMeshBuilder::ToDynamicMesh() const
{
	TArray<FDynamicMesh3> Meshes = ToDynamicMeshes(1, [](const FVector3f&) { return 0; });
	return MoveTemp(Meshes[0]);
}

TArray<FDynamicMesh3> FWorldMeshBuilder::ToDynamicMeshes(int32 NumChunks, TFunctionRef<int32(const FVector3f&)> ChunkOf) const
{
	// Normals and tangents come from the whole mesh, so chunk borders don't show as lighting seams.
	const TArray<FVector3f> Normals = ComputeVertexNormals(Positions, Triangles);
	const FTangentFrames Frames = ComputeTangentFrames(Positions, UVs, Triangles, Normals);
	// Reserved up front: TArray moves elements bitwise when it grows, which would break the pointer each mesh's
	// attribute set keeps to its mesh.
	TArray<FDynamicMeshWriter> Writers;
	Writers.Reserve(NumChunks);
	for (int32 Chunk = 0; Chunk < NumChunks; ++Chunk)
	{
		Writers.Emplace(Positions, UVs, Colors, Normals, Frames);
	}
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		const FIntVector3& Triangle = Triangles[Index];
		const FVector3f Centroid = (Positions[Triangle.X] + Positions[Triangle.Y] + Positions[Triangle.Z]) / 3.f;
		const int32 Chunk = FMath::Clamp(ChunkOf(Centroid), 0, NumChunks - 1);
		Writers[Chunk].AddTriangle(Triangle, TriangleMaterials[Index]);
	}
	TArray<FDynamicMesh3> Meshes;
	Meshes.Reserve(NumChunks);
	for (FDynamicMeshWriter& Writer : Writers)
	{
		Meshes.Add(MoveTemp(Writer.Mesh));
	}
	return Meshes;
}
