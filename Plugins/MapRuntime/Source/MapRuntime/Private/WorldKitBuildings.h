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

/** A flat roof surface the caller still has to add, at Z metres absolute with the given material slot. */
struct FKitFlatRoof
{
	bool bNeeded = false;
	float Z = 0.f;
	int32 Material = 0;
};

/** True when the building has a typing record, a class with a kit style, and a footprint the kit walls fit. */
bool IsKitBuilding(const FWorldBuilding& Building);

/**
 * Appends the instances of one building's walls and decorations to Instances and its generated roof to RoofBuilder
 * (cm, relative to the tile corner). FindSlot turns a material name (Roof_Clay, Facade_BrickSooty ...) into a slot of
 * the tile's material list. A flat roof is returned for the caller to add.
 */
FKitFlatRoof BuildKitBuilding(const FWorldTileData& Tile, const FWorldBuilding& Building, const TFunction<int32(const FString&)>& FindSlot,
	FWorldKitInstances& Instances, FWorldMeshBuilder& RoofBuilder);

/** Content path of the static mesh of a kit piece key; the part after a | is the replacement material. */
FString KitMeshPath(const FName& PieceName);

class AWorldTileActor;

/**
 * Spawns the instance components of one tile's kit pieces, a few piece types per call so a tile's spawn stays inside
 * the frame budget. Step counts up from 0 on each call; returns true after the last piece type. Game thread.
 */
bool AddKitInstancesStep(AWorldTileActor& Actor, const FWorldKitInstances& Kit, int32 Step);
