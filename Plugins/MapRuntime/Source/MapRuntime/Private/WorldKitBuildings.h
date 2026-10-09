#pragma once

#include "CoreMinimal.h"

class FWorldMeshBuilder;
struct FWorldBuilding;
struct FWorldTileData;

/**
 * Buildings assembled from the building kit (Tools/buildingkit): wall bays with window reveals, sills, corners,
 * plinth, string courses, cornice, gutters, dormers and chimneys are instances of kit static meshes, the roof
 * surfaces are generated (so hips, gables and the eaves fit any footprint). The kit pieces and their placement rules
 * are described in Tools/buildingkit/README.md.
 */

/** Instance transforms (cm, relative to the tile corner) per kit piece, e.g. "brick_Wall_Window". */
struct FWorldKitInstances
{
	TMap<FName, TArray<FTransform>> Pieces;
	int32 NumBuildings = 0;
	int32 NumInstances = 0;

	bool IsEmpty() const { return NumInstances == 0; }
};

/** Material slot indices of the tile's material list that the generated kit roofs and gables use. */
struct FKitRoofMaterials
{
	int32 RoofTiles = 0;
	int32 RoofFlat = 0;
	/** Gable end triangles, in the building's facade material. */
	int32 Gable = 0;
};

/** True when the building has a typing record, a class with a kit style, and a footprint the kit walls fit. */
bool IsKitBuilding(const FWorldBuilding& Building);

/**
 * Appends the instances of one building's walls and decorations to Instances and its generated roof to RoofBuilder
 * (cm, relative to the tile corner). Returns true when the roof is flat and the caller should add a flat roof surface
 * at FlatRoofZ (metres, absolute).
 */
bool BuildKitBuilding(const FWorldTileData& Tile, const FWorldBuilding& Building, const FKitRoofMaterials& Materials,
	FWorldKitInstances& Instances, FWorldMeshBuilder& RoofBuilder, float& FlatRoofZ);

/** Content path of the static mesh of a kit piece name. */
FString KitMeshPath(const FName& PieceName);

class AWorldTileActor;

/**
 * Spawns the instance components of one tile's kit pieces, a few piece types per call so a tile's spawn stays inside
 * the frame budget. Step counts up from 0 on each call; returns true after the last piece type. Game thread.
 */
bool AddKitInstancesStep(AWorldTileActor& Actor, const FWorldKitInstances& Kit, int32 Step);
