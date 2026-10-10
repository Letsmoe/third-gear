#pragma once

#include "CoreMinimal.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
struct FWorldStructure;
struct FWorldTileData;

/**
 * The building kit's structure pieces placed by the STRU section (WorldBuilder's Structures.h): the meshes from
 * Scripts/import_structures.py under /Game/World/Structures, with their material variants (a petrol brand, a poster)
 * swapped in per placement. Used by the region's tall structures (AWorldStructuresActor) and by the tiles.
 */

/** Instances of one piece in one variant, in cm relative to the file's corner. */
struct FWorldStructureGroup
{
	/** <family>_<piece>, e.g. pylons_Donau_Anchor. */
	FString Piece;
	/** Material variant, or empty. */
	FString Variant;
	/** Conductors carry their sag in custom data 0 and never collide. */
	bool bWire = false;
	TArray<FTransform> Transforms;
	TArray<float> Sags;
};

/** One spinning rotor: where it sits (cm, relative to the file's corner), the tilted shaft, speed and start angle. */
struct FWorldStructureRotor
{
	FString Piece;
	FVector Location = FVector::ZeroVector;
	FQuat Shaft = FQuat::Identity;
	float Rpm = 0.f;
	float StartDegrees = 0.f;
};

/** The structures of one file, grouped for instancing. Built on any thread. */
struct FWorldStructureInstances
{
	TArray<FWorldStructureGroup> Groups;
	TArray<FWorldStructureRotor> Rotors;

	bool IsEmpty() const { return Groups.IsEmpty() && Rotors.IsEmpty(); }
};

/** Groups a file's STRU records by piece and variant. */
void BuildStructureInstances(const FWorldTileData& Data, FWorldStructureInstances& Out);

/** The static mesh of a piece, loaded on first use and kept; null (and a warning once) when it isn't imported. */
UStaticMesh* FindStructureMeshCached(const FString& Piece);

/**
 * Adds one group as an instanced component attached to Parent; it blocks the car unless it is a conductor or
 * bCollision is false. Returns null when the mesh is missing. Game thread.
 */
UInstancedStaticMeshComponent* AddStructureGroup(AActor& Owner, USceneComponent& Parent, const FWorldStructureGroup& Group,
	bool bCollision);

class AWorldTileActor;

/**
 * Adds the next few groups of a tile's structures to its actor, with collision, so the spawn stays inside the frame
 * budget. Step counts up from 0 on each call; returns true after the last group. Game thread.
 */
bool AddStructureGroupsStep(AWorldTileActor& Actor, const FWorldStructureInstances& Structures, int32 Step);

/** Replaces the slots a variant has its own material for (MI_Struct_<Slot>_<Variant>) on a component's mesh. */
void ApplyStructureVariant(class UMeshComponent& Component, UStaticMesh& Mesh, const FString& Variant);
