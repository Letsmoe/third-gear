#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"

struct FWorldTileData;
struct FWorldTileMeshes;

/**
 * The snow layer of one tile: a mesh of the snow surface at full depth (about 15 cm on lawns), in ChunksPerSide²
 * chunks like the ground. The vertices sit at the full-depth surface so the normals describe the rounded shape; the
 * material lowers them to the ground by (1 - SnowCover) times the depth stored in the vertex colour.
 *
 * Vertex colour: R = depth in 0.5 cm steps, G = road weight, B = trodden weight (footways), A = distance from the
 * nearest road or footway edge in 1/51 m steps (the material draws wheel tracks and the worn footway from it).
 * UV is the world position in metres, like the ground.
 */
struct FWorldSnowMeshes
{
	TArray<UE::Geometry::FDynamicMesh3> Chunks;
	int32 ChunksPerSide = 4;
	FVector2f ChunkSizeCm = FVector2f::ZeroVector;
};

/**
 * Builds the snow layer from the tile's data and its already built ground and building meshes: the ground heights
 * are rasterised into a 1 m grid, the snow surface is that grid plus a class dependent depth, smoothed so kerbs and
 * edges round off, with ploughed ridges along roads and drifts on the lee side of walls, then meshed as two triangles per cell. Roofs get their own cap with a rounded rim. Pure function; runs on worker threads.
 */
void BuildWorldSnow(const FWorldTileData& Tile, const FWorldTileMeshes& Meshes, FWorldSnowMeshes& Out);
