#pragma once

#include "CoreMinimal.h"

/**
 * One world data tile (.tgtile) as plain data: what Tools/osmimport/build_world.py writes and the streamer turns
 * into meshes. The file format is documented in Tools/osmimport/osmimport/worldtile.py; keep both in sync.
 *
 * Positions are metres relative to the tile corner (Origin, world metres, x east / y south); heights are absolute
 * metres above sea level.
 */

/** How a surface polygon gets its height (worldtile.py HEIGHT_*). */
enum class EWorldSurfaceHeight : uint8
{
	Road = 0,     // road height grid + Params[0]
	Terrain = 1,  // terrain grid + Params[0]
	Constant = 2, // Params[0]
	Ramp = 3,     // Params[0] at (Params[2], Params[3]) to Params[1] at (Params[4], Params[5]), in world metres
};

enum class EWorldRoofShape : uint8
{
	Flat = 0,
	Gabled = 1,
};

/** Outline plus holes; rings are open (no repeated end point). */
struct FWorldPolygon
{
	TArray<TArray<FVector2f>> Rings;
};

struct FWorldSurface
{
	int32 Material = 0;
	EWorldSurfaceHeight HeightMode = EWorldSurfaceHeight::Road;
	float Params[6] = {};
	FWorldPolygon Polygon;
};

struct FWorldMarking
{
	int32 Material = 0;
	uint8 Style = 0;
	float Width = 0.f;
	/** Dash length and gap in metres; both 0 for a solid line. */
	float DashOn = 0.f;
	float DashOff = 0.f;
	/** Distance along the original line where this piece starts, so dashes continue across tiles. */
	float Phase = 0.f;
	TArray<FVector2f> Points;
};

struct FWorldBuilding
{
	uint64 OsmId = 0;
	int32 Facade = 0;
	int32 Roof = 0;
	EWorldRoofShape RoofShape = EWorldRoofShape::Flat;
	/** Per-building brightness (R) and window variation (G) for the facade material, 0..255. */
	uint8 Tint = 0;
	uint8 Variation = 0;
	float BaseZ = 0.f;
	float EaveHeight = 0.f;
	/** Gabled roofs: the footprint's minimum rotated rectangle. */
	FVector2f RoofRectangle[4];
	FWorldPolygon Footprint;
};

struct FWorldPlant
{
	int32 Model = 0;
	/** x, y relative to the tile corner; z absolute. */
	FVector3f Position = FVector3f::ZeroVector;
	float YawDegrees = 0.f;
	float CrownDiameter = 0.f;
	float Height = 0.f;
	float TrunkDiameter = 0.f;
};

/** Terrain and road heights and land cover on the tile's vertex grid (row 0 at the tile's y origin). */
struct FWorldTileGrid
{
	int32 NumX = 0;
	int32 NumY = 0;
	float CellSize = 1.f;
	float BaseZ = 0.f;
	TArray<uint16> TerrainCm;
	TArray<uint16> RoadCm;
	/** Blend weights meadow, field, forest per vertex, 0..255. */
	TArray<uint8> Cover;

	/** Terrain value of a grid point without ground: horizon tiles leave the region's area to its own tiles. */
	static constexpr uint16 HoleValue = 0xFFFF;

	/** Terrain height at grid vertex (clamped to the grid). */
	float TerrainAtVertex(int32 X, int32 Y) const;

	/** True when the grid vertex is a hole; every cell touching it is left out. */
	bool IsHole(int32 X, int32 Y) const;

	/** Bilinear terrain height at a tile-local position, clamped to the tile. */
	float TerrainAt(float LocalX, float LocalY) const;

	/** Bilinear road surface height at a tile-local position, clamped to the tile. */
	float RoadAt(float LocalX, float LocalY) const;

	/** Land cover weights at grid vertex. */
	FColor CoverAtVertex(int32 X, int32 Y) const;

private:
	float SampleBilinear(const TArray<uint16>& Heights, float LocalX, float LocalY) const;
};

struct MAPRUNTIME_API FWorldTileData
{
	/** Tile corner in world metres. */
	FVector2D Origin = FVector2D::ZeroVector;
	FVector2f Size = FVector2f::ZeroVector;
	/** Material and model names that records refer to by index. */
	TArray<FString> Names;
	FWorldTileGrid Grid;
	TArray<FWorldSurface> Surfaces;
	TArray<FWorldMarking> Markings;
	TArray<FWorldBuilding> Buildings;
	TArray<FWorldPlant> Plants;

	/** Reads a .tgtile file. Returns false and fills Error when the file is missing or malformed. */
	static bool Load(const FString& Path, FWorldTileData& Out, FString& Error);

	/** Height of a surface vertex at a tile-local position, by the surface's height rule. */
	float SurfaceHeightAt(const FWorldSurface& Surface, float LocalX, float LocalY) const;
};
