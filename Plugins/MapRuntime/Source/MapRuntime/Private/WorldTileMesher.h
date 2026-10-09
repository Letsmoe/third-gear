#pragma once

#include "CoreMinimal.h"
#include "WorldMeshBuilder.h"

struct FWorldTileData;
struct FWorldFurnitureInstances;

/** How much of a tile is built, by its distance from the viewer. */
enum class EWorldTileDetail : uint8
{
	/** Everything: 1 m terrain, kerbs, markings, shrubs, collision. */
	Near = 0,
	/** 4 m terrain, roads and paths without kerbs or markings, trees but no shrubs, no collision. */
	Middle = 1,
	/** 8 m terrain, main surfaces, buildings and trees. */
	Far = 2,
};

/** Shortest distance from a point to a rectangle, 0 inside. */
inline double DistanceToBox2D(const FBox2D& Box, const FVector2D& Point)
{
	return FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(Point));
}

/** What the mesher needs to know about the game's assets; filled on the game thread, read by workers. */
struct FWorldMeshingContext
{
	/** Natural crown diameter and height of each plant model in metres, by model name. */
	TMap<FString, FVector2f> PlantModelSizes;
};

/** Instances of one plant model in a tile. */
struct FWorldPlantInstances
{
	FString Model;
	TArray<FTransform> Transforms;
};

/** Everything built for one tile at one detail level, in cm relative to the tile corner. */
struct FWorldTileMeshes
{
	EWorldTileDetail Detail = EWorldTileDetail::Near;
	/** Material slot names; mesh material IDs index into this. */
	TArray<FString> MaterialNames;
	/** Terrain, road surfaces, paths, water and kerbs. */
	FWorldMeshBuilder Ground;
	/** Painted lines on the roads. */
	FWorldMeshBuilder Markings;
	/** Walls and roofs. */
	FWorldMeshBuilder Buildings;
	/**
	 * The builders as dynamic meshes, converted at the end of BuildWorldTileMeshes. The ground is split into
	 * ChunksPerSide² square chunks (row-major from the tile corner), so spawning and collision cooking can be
	 * spread over frames and collision limited to the chunks near the car.
	 */
	TArray<UE::Geometry::FDynamicMesh3> GroundChunks;
	int32 ChunksPerSide = 1;
	/** Chunk edge lengths in cm. */
	FVector2f ChunkSizeCm = FVector2f::ZeroVector;
	UE::Geometry::FDynamicMesh3 MarkingsMesh;
	UE::Geometry::FDynamicMesh3 BuildingsMesh;
	TArray<FWorldPlantInstances> Plants;
	/** Trunk collision cylinders: base point (cm, tile-relative) and diameter (cm). */
	TArray<FVector> TrunkBases;
	TArray<float> TrunkDiameters;
	/** Lamps, signal poles and signs (near detail only), or null. */
	TSharedPtr<FWorldFurnitureInstances> Furniture;
};

/**
 * Builds all meshes and instance transforms of a tile, including the dynamic meshes. Pure function of its inputs;
 * meant for worker threads.
 */
FWorldTileMeshes BuildWorldTileMeshes(const FWorldTileData& Tile, EWorldTileDetail Detail, const FWorldMeshingContext& Context);
