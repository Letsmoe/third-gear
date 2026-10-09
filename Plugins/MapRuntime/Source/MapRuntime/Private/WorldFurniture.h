#pragma once

#include "CoreMinimal.h"

struct FWorldTileData;

/** A lamp or signal lens that gets a real light when the car is near. Tile-relative position in cm. */
struct FFurnitureLight
{
	FVector LocationCm = FVector::ZeroVector;
	/** Direction the light shines in (world, unit). */
	FVector Direction = FVector::DownVector;
	/** Signal heads: approach id, else INDEX_NONE for a street lamp. */
	int32 ApproachId = INDEX_NONE;
	/** Signal heads: vertical offsets of the red, amber and green lens from LocationCm. */
	float LensOffsetsCm[3] = {};
};

/** Instance transforms (cm, relative to the tile corner) of the street furniture of one tile. Built on worker threads. */
struct FWorldFurnitureInstances
{
	TArray<FTransform> Lamps;
	TArray<FTransform> SignalPoles;
	TArray<FTransform> SignalHeads;
	/** Approach id per entry of SignalHeads. */
	TArray<int32> HeadApproaches;
	/** Sign plates by graphic name (Zeichen_274-30). */
	TMap<FString, TArray<FTransform>> SignPlates;
	TArray<FTransform> SignClamps;
	/** Sign poles by visible height in cm. */
	TMap<int32, TArray<FTransform>> SignPoles;
	TArray<FFurnitureLight> Lights;

	bool IsEmpty() const { return Lamps.IsEmpty() && SignalPoles.IsEmpty() && SignPlates.IsEmpty(); }
};

/** Turns the tile's POIS records into instance transforms. */
void BuildWorldFurniture(const FWorldTileData& Tile, FWorldFurnitureInstances& Out);

/** Pole heights (cm) the sign pole meshes come in. */
const TArray<int32>& GetSignPoleHeightsCm();

/** Asset names of the furniture meshes under /Game/World/Furniture/Meshes. */
namespace FurnitureAssets
{
extern const TCHAR* const Lamp;
extern const TCHAR* const SignalPole;
extern const TCHAR* const SignalHead;
extern const TCHAR* const SignPlate;
extern const TCHAR* const SignClamp;
}

/** Path of the texture of a sign graphic, e.g. Zeichen_274.1-20 to /Game/World/Furniture/Signs/T_Zeichen_274_1-20. */
FString SignTexturePath(const FString& GraphicName);
