#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/** Road surfaces the tyre noise distinguishes. */
enum class ECarRoadSurface : uint8
{
	Asphalt,
	Cobble,
	Pavers,
	Rough, // grass, gravel, anything unpaved
	Count
};

/** One-shot sounds the game thread can trigger. */
enum class ECarSoundEvent : uint8
{
	GearClunk,
	HandbrakePull,
	HandbrakeRelease,
	SuspensionThump,
	StallShudder,
	StarterSolenoid,
	BlowOff,
	IndicatorOn,
	IndicatorOff,
};

/** Everything the synthesiser needs to know about the car, written by the game thread every frame. */
struct FCarSoundInputs
{
	// --- Engine ---
	float EngineRpm = 0.f;
	float Throttle = 0.f;           // pedal, 0..1
	float Load = 0.f;               // net engine torque / full-load torque, negative on overrun
	float Boost = 0.f;              // turbo boost, 0..1
	float Clutch = 0.f;             // pedal, 1 = pressed
	bool bEngineRunning = true;
	bool bCranking = false;
	bool bRevLimiter = false;
	bool bGrinding = false;         // gear grind
	int32 EngagedGear = 0;

	// --- Road ---
	float SpeedMps = 0.f;
	float AirSpeedMps = 0.f;        // speed of the air relative to the car
	float SurfaceWeights[static_cast<int32>(ECarRoadSurface::Count)] = {1.f, 0.f, 0.f, 0.f};
	float Wetness = 0.f;            // 0 dry .. 1 standing water
	float SquealLevel = 0.f;        // 0..1, from the tyres' slip
	float SquealPitch = 0.f;        // 0..1, 1 = longitudinal (locking) rather than cornering

	// --- Details ---
	bool bIndicatorOn = false;

	/** Closed car with all windows up: 1. Reduces the brightness of everything outside the cabin. */
	float CabinClosed = 1.f;
};

/**
 * Procedural sound of a 1.4 litre four-cylinder turbo petrol car heard from the driver's seat: engine (firing orders
 * with load dependent timbre, intake, turbo, rev limiter, starter, stall), tyre roar on asphalt, cobbles and pavers,
 * squeal, wind, and the small mechanical sounds (gear clunk, handbrake, indicator relay, kerb thumps).
 *
 * Plain C++ with no engine dependency beyond TArray, so the audio thread can run it and the offline test can render
 * the same code to a WAV file. The game thread sets the inputs and posts events; the audio thread calls Render.
 */
class FCarSoundDsp
{
public:
	explicit FCarSoundDsp(int32 InSampleRate);
	~FCarSoundDsp();

	/** Latest car state. Safe to call from any thread. */
	void SetInputs(const FCarSoundInputs& NewInputs);

	/** Triggers a one-shot sound; Strength 0..1. Safe to call from any thread. */
	void PostEvent(ECarSoundEvent Event, float Strength = 1.f);

	/** Writes NumFrames interleaved stereo frames. */
	void Render(float* OutStereo, int32 NumFrames);

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
