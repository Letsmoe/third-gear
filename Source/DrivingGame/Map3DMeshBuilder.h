#pragma once

#include "CoreMinimal.h"

/**
 * The colours of the 3D map, as slots of a small palette texture. A vertex carries its slot in the U of its UV, so the
 * day and night palettes switch by rewriting 32 pixels instead of rebuilding any mesh.
 */
enum class EMap3DColor : uint8
{
	Ground,
	Green,
	Water,
	Footway,
	Road,
	Marking,
	RoofResidential,
	RoofCommercial,
	RoofIndustrial,
	WallResidential,
	WallCommercial,
	WallIndustrial,
	Trunk,
	Crown,
	Route,
	SignalHousing,
	CarBody,
	CarGlass,
	PinRed,
	PinWhite,
	SignalRed,
	SignalYellow,
	SignalGreen,
	Count,
};

constexpr int32 Map3DPaletteSize = 32;

/** One vertex of a 3D map mesh: a position in centimetres from the mesh's origin, and its colour slot and brightness. */
struct FMap3DVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	/** U of the palette texture that holds the vertex's colour. */
	float PaletteU = 0.f;
	/** Brightness factor, baked from the direction the face looks. */
	float Shade = 1.f;
};

/** A finished mesh of the 3D map, ready to become a UMap3DMeshComponent. */
struct FMap3DMeshData
{
	TArray<FMap3DVertex> Vertices;
	TArray<uint32> Indices;
	/** Bounds of the vertices, centimetres from the mesh's origin. */
	FBox Bounds = FBox(ForceInit);
};

/**
 * Collects the triangles of a 3D map mesh in metres and turns them into FMap3DMeshData in centimetres. Faces with their
 * own shade get their own vertices; flat layers share them. Safe on worker threads.
 */
class FMap3DMeshBuilder
{
public:
	/** Appends a vertex and returns its index. */
	int32 AddVertex(const FVector3f& Position, EMap3DColor Color, float Shade);

	/** Appends a triangle of existing vertices, wound to face along Facing; degenerate triangles are dropped. */
	void AddTriangle(int32 A, int32 B, int32 C, const FVector3f& Facing);

	/** Adds a triangle with its own vertices. */
	void AddTriangle(const FVector3f& A, const FVector3f& B, const FVector3f& C, EMap3DColor Color, float Shade, const FVector3f& Facing);

	/** Adds a quad given in order around its corners, with its own vertices. */
	void AddQuad(const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D, EMap3DColor Color, float Shade, const FVector3f& Facing);

	/** Adds a quad on the ground at a height, facing up. */
	void AddFlatQuad(const FVector2f& A, const FVector2f& B, const FVector2f& C, const FVector2f& D, float Height, EMap3DColor Color);

	/** Adds a polygon given as vertices and triangles of vertex indices, flat at a height and facing up. */
	void AddFlatTriangles(const TArray<FVector2f>& Vertices, const TArray<FIntVector3>& Triangles, float Height, EMap3DColor Color);

	/** Adds a box turned by YawRadians around its base centre; Shade is the brightness of the top. */
	void AddBox(const FVector3f& BaseCentre, const FVector3f& HalfSize, float YawRadians, EMap3DColor Color, float Shade);

	/** Adds a low-poly sphere (an icosahedron stretched to the radii) with per-face shading. */
	void AddBlob(const FVector3f& Centre, const FVector3f& Radii, EMap3DColor Color, float Shade);

	/** Adds a flat ribbon along a polyline of the ground, with the given width and height above the ground. */
	void AddRibbon(const TArray<FVector2f>& Points, float Width, float Height, EMap3DColor Color);

	int32 NumTriangles() const { return Indices.Num() / 3; }
	bool IsEmpty() const { return Indices.IsEmpty(); }

	/** The mesh in centimetres. */
	TSharedPtr<FMap3DMeshData> Finish() const;

private:
	TArray<FVector3f> Positions;
	TArray<FVector2f> PaletteAndShade;
	TArray<uint32> Indices;
};

namespace Map3D
{
/** Brightness of a surface by the direction it faces: the sun stands in the north-west, sky light fills the rest. */
float ShadeForNormal(const FVector3f& Normal);
}
