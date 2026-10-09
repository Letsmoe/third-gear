#pragma once

#include "CoreMinimal.h"
#include "CarSimTypes.h"

/** Road surface classes that differ in tyre grip. */
enum class ECarGripSurface : uint8
{
	Asphalt,
	Cobble,
	Pavers,
	/** Lawn, soil, gravel and unpaved tracks. */
	Rough,
};

/** The weather as the tyres feel it. */
struct FCarSurfaceConditions
{
	/** How wet the road is, 0 (dry) to 1 (standing water). */
	float Wetness = 0.f;
	/** How much of the ground is covered in snow, 0 to 1. */
	float SnowCover = 0.f;
	float TemperatureCelsius = 15.f;
};

/**
 * Tyre grip on wet, snowy and icy ground. Plain functions, no engine state, so the game thread can call them
 * a few times per second per wheel and tests can call them directly. Dry asphalt gives exactly the neutral
 * FCarWheelSurface, so dry driving is unchanged.
 *
 * Friction values are relative to dry asphalt and follow published figures for winter tyres: about 0.8 wet,
 * 0.3 on packed snow and 0.1 on ice. On snow and ice the grip peak moves to higher slip and flattens.
 */
namespace CarSurfaceGrip
{
	/** Surface class from the ground material name (FindWorldSurfaceName); NAME_None counts as asphalt. */
	DRIVINGGAME_API ECarGripSurface Classify(const FName& GroundMaterialName);

	/**
	 * Grip, slip curve shape and snow drag for one wheel. Variation is a position-based value in -1..1 that makes
	 * snow depth and grip differ between the wheels of a car (ruts, tracks).
	 */
	DRIVINGGAME_API FCarWheelSurface Evaluate(ECarGripSurface Surface, const FCarSurfaceConditions& Conditions, float Variation);

	/** Fixed conditions for the drive test: "dry", "wet", "flood", "snow" or "ice". False if the name is unknown. */
	DRIVINGGAME_API bool TestPreset(const FString& Name, FCarSurfaceConditions& OutConditions);

	/** Smooth 0..1 ramp from Edge0 to Edge1 (Edge0 may be above Edge1 for a falling ramp). */
	DRIVINGGAME_API float Ramp(float Edge0, float Edge1, float Value);
}
