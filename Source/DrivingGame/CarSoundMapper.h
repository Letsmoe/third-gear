#pragma once

#include "CoreMinimal.h"
#include "CarSimTypes.h"
#include "CarSoundDsp.h"

/** What the sound needs to know about the world around the car (not part of the physics telemetry). */
struct FCarSoundEnvironment
{
	/** Road surface under the wheels, as weights that add up to 1. */
	float SurfaceWeights[static_cast<int32>(ECarRoadSurface::Count)] = {1.f, 0.f, 0.f, 0.f};
	float Wetness = 0.f;
	/** Wind velocity in the car's frame, m/s: x forward, y right. */
	FVector2D WindLocalMps = FVector2D::ZeroVector;
	bool bIndicatorOn = false;
	float FullLoadTorqueNm = 250.f;
	float RevLimitRpm = 6400.f;
};

/** A one-shot the mapper wants played. */
struct FCarSoundEventRequest
{
	ECarSoundEvent Event;
	float Strength;
};

/**
 * Turns physics telemetry into the synthesiser's inputs and detects the moments that make one-shot sounds: gear
 * engagements, handbrake, stall, starter, kerb and pothole hits. Keeps the previous sample to find edges.
 * Plain C++ so the offline audio test uses exactly what the game uses.
 */
class FCarSoundMapper
{
public:
	/** Forget history, e.g. after the car was teleported. */
	void Reset();

	/** Builds the inputs for one telemetry sample; one-shots to play are appended to OutEvents. */
	FCarSoundInputs Update(const FCarTelemetry& Telemetry, const FCarSoundEnvironment& Environment, float DeltaSeconds,
		TArray<FCarSoundEventRequest>& OutEvents);

private:
	float ComputeSquealLevel(const FCarTelemetry& Telemetry, float& OutLongitudinalShare) const;
	void DetectBumps(const FCarTelemetry& Telemetry, float DeltaSeconds, TArray<FCarSoundEventRequest>& OutEvents);
	void DetectDiscreteEvents(const FCarTelemetry& Telemetry, TArray<FCarSoundEventRequest>& OutEvents);

	bool bHasPrevious = false;
	FCarTelemetry Previous;
	float WheelLoadBaselineN[CarNumWheels] = {};
	float BumpCooldownSeconds[2] = {};
};
