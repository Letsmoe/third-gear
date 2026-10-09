#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

/**
 * Collects triangles with UVs, vertex colours and a material slot, in centimetres relative to the tile corner,
 * and turns them into an FDynamicMesh3 with normals and tangents. Runs on worker threads.
 *
 * Winding follows the engine (VectorUtil::Normal): a triangle (A, B, C) faces along Cross(C - A, B - A).
 * The Add functions take the direction a triangle should face and fix the winding themselves.
 */
class FWorldMeshBuilder
{
public:
	/** Appends a vertex and returns its index. Position in cm, UV in metres (materials tile by metre). */
	int32 AddVertex(const FVector3f& Position, const FVector2f& UV, const FColor& Color = FColor::White);

	/** Appends a triangle in Material, wound to face along FacingHint. */
	void AddTriangle(int32 A, int32 B, int32 C, int32 Material, const FVector3f& FacingHint);

	/** Appends a triangle in Material exactly as wound. */
	void AddTriangleRaw(int32 A, int32 B, int32 C, int32 Material);

	/** Appends a quad (A, B, C, D in order around it) facing along FacingHint. */
	void AddQuad(int32 A, int32 B, int32 C, int32 D, int32 Material, const FVector3f& FacingHint);

	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTriangles() const { return Triangles.Num(); }
	bool IsEmpty() const { return Triangles.IsEmpty(); }
	const FVector3f& GetPosition(int32 Vertex) const { return Positions[Vertex]; }

	/**
	 * Builds the dynamic mesh: smooth per-vertex normals from the triangles around each vertex, tangents from the
	 * UVs, colours and material IDs. Vertices that would make an edge non-manifold are split.
	 */
	UE::Geometry::FDynamicMesh3 ToDynamicMesh() const;

private:
	TArray<FVector3f> Positions;
	TArray<FVector2f> UVs;
	TArray<FColor> Colors;
	TArray<FIntVector3> Triangles;
	TArray<int32> TriangleMaterials;
};
