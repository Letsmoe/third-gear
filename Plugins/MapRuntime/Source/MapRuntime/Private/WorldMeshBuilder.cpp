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

FDynamicMesh3 FWorldMeshBuilder::ToDynamicMesh() const
{
	const TArray<FVector3f> Normals = ComputeVertexNormals(Positions, Triangles);
	const FTangentFrames Frames = ComputeTangentFrames(Positions, UVs, Triangles, Normals);

	FDynamicMesh3 Mesh;
	Mesh.EnableAttributes();
	FDynamicMeshAttributeSet& Attributes = *Mesh.Attributes();
	Attributes.EnablePrimaryColors();
	Attributes.EnableMaterialID();
	Attributes.EnableTangents();
	FDynamicMeshUVOverlay& UVOverlay = *Attributes.PrimaryUV();
	FDynamicMeshNormalOverlay& NormalOverlay = *Attributes.PrimaryNormals();
	FDynamicMeshNormalOverlay& TangentOverlay = *Attributes.PrimaryTangents();
	FDynamicMeshNormalOverlay& BitangentOverlay = *Attributes.PrimaryBiTangents();
	FDynamicMeshColorOverlay& ColorOverlay = *Attributes.PrimaryColors();
	FDynamicMeshMaterialAttribute& MaterialIDs = *Attributes.GetMaterialID();

	// Mesh vertex and overlay element per builder vertex; splitting a vertex appends a copy of both.
	const auto AppendCopy = [&](int32 Source) -> int32
	{
		const int32 Vertex = Mesh.AppendVertex(FVector3d(Positions[Source]));
		UVOverlay.AppendElement(UVs[Source]);
		NormalOverlay.AppendElement(Normals[Source]);
		TangentOverlay.AppendElement(Frames.Tangents[Source]);
		BitangentOverlay.AppendElement(Frames.Bitangents[Source]);
		ColorOverlay.AppendElement(FVector4f(Colors[Source].R / 255.f, Colors[Source].G / 255.f, Colors[Source].B / 255.f, Colors[Source].A / 255.f));
		return Vertex;
	};
	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		AppendCopy(Index); // element IDs equal vertex IDs for these
	}
	for (int32 Index = 0; Index < Triangles.Num(); ++Index)
	{
		FIndex3i Triangle(Triangles[Index].X, Triangles[Index].Y, Triangles[Index].Z);
		int32 TriangleID = Mesh.AppendTriangle(Triangle);
		if (TriangleID == FDynamicMesh3::NonManifoldID)
		{
			Triangle = FIndex3i(AppendCopy(Triangle.A), AppendCopy(Triangle.B), AppendCopy(Triangle.C));
			TriangleID = Mesh.AppendTriangle(Triangle);
		}
		if (TriangleID < 0)
		{
			continue;
		}
		UVOverlay.SetTriangle(TriangleID, Triangle);
		NormalOverlay.SetTriangle(TriangleID, Triangle);
		TangentOverlay.SetTriangle(TriangleID, Triangle);
		BitangentOverlay.SetTriangle(TriangleID, Triangle);
		ColorOverlay.SetTriangle(TriangleID, Triangle);
		MaterialIDs.SetValue(TriangleID, TriangleMaterials[Index]);
	}
	return Mesh;
}
