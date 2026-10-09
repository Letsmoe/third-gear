#include "CarSoundMapper.h"

namespace
{
constexpr float PeakSlipRatio = 0.12f;
constexpr float PeakSlipAngleDeg = 8.f;
constexpr float SquealStartSlip = 1.05f; // multiples of the peak slip at which a tyre starts to sing
constexpr float SquealFullSlip = 1.9f;
constexpr float BumpThresholdN = 650.f;  // wheel load change from its slow average that counts as a hit
constexpr float BumpFullStrengthN = 3500.f;
constexpr float BumpCooldownLength = 0.12f;
constexpr float BumpBaselineSeconds = 0.2f;
}

void FCarSoundMapper::Reset()
{
	bHasPrevious = false;
	BumpCooldownSeconds[0] = BumpCooldownSeconds[1] = 0.f;
}

float FCarSoundMapper::ComputeSquealLevel(const FCarTelemetry& Telemetry, float& OutLongitudinalShare) const
{
	float Strongest = 0.f;
	OutLongitudinalShare = 0.f;
	if (FMath::Abs(Telemetry.LocalVelocityMps.X) < 1.5f)
	{
		return 0.f;
	}
	for (int32 Wheel = 0; Wheel < CarNumWheels; ++Wheel)
	{
		if (!Telemetry.bContact[Wheel] || Telemetry.LoadN[Wheel] < 300.f)
		{
			continue;
		}
		const float Longitudinal = Telemetry.SlipRatio[Wheel] / PeakSlipRatio;
		const float Lateral = Telemetry.SlipAngleDeg[Wheel] / PeakSlipAngleDeg;
		const float Slip = FMath::Sqrt(Longitudinal * Longitudinal + Lateral * Lateral);
		const float Ramp = FMath::Clamp((Slip - SquealStartSlip) / (SquealFullSlip - SquealStartSlip), 0.f, 1.f);
		const float Level = Ramp * Ramp * (3.f - 2.f * Ramp) * FMath::Clamp(Telemetry.LoadN[Wheel] / 2500.f, 0.f, 1.f);
		if (Level > Strongest)
		{
			Strongest = Level;
			OutLongitudinalShare = Longitudinal * Longitudinal / FMath::Max(Longitudinal * Longitudinal + Lateral * Lateral, 1e-4f);
		}
	}
	return Strongest;
}

void FCarSoundMapper::DetectBumps(const FCarTelemetry& Telemetry, float DeltaSeconds, TArray<FCarSoundEventRequest>& OutEvents)
{
	const float Speed = FMath::Abs(Telemetry.LocalVelocityMps.X);
	for (float& Cooldown : BumpCooldownSeconds)
	{
		Cooldown = FMath::Max(0.f, Cooldown - DeltaSeconds);
	}
	const float Follow = FMath::Min(1.f, DeltaSeconds / BumpBaselineSeconds);
	for (int32 Wheel = 0; Wheel < CarNumWheels; ++Wheel)
	{
		const float Load = Telemetry.bContact[Wheel] ? Telemetry.LoadN[Wheel] : 0.f;
		const float Change = Load - WheelLoadBaselineN[Wheel];
		WheelLoadBaselineN[Wheel] += Change * Follow;
		const int32 Axle = Wheel / 2;
		if (Speed < 1.5f || FMath::Abs(Change) < BumpThresholdN || BumpCooldownSeconds[Axle] > 0.f)
		{
			continue;
		}
		BumpCooldownSeconds[Axle] = BumpCooldownLength;
		const float Strength = FMath::Clamp((FMath::Abs(Change) - BumpThresholdN) / BumpFullStrengthN, 0.15f, 1.f)
			* FMath::Clamp(Speed / 8.f, 0.35f, 1.f);
		OutEvents.Add({ECarSoundEvent::SuspensionThump, Strength});
	}
}

void FCarSoundMapper::DetectDiscreteEvents(const FCarTelemetry& Telemetry, TArray<FCarSoundEventRequest>& OutEvents)
{
	if (Telemetry.EngagedGear != Previous.EngagedGear)
	{
		const bool bEngaging = Telemetry.EngagedGear != 0;
		OutEvents.Add({ECarSoundEvent::GearClunk, bEngaging ? 0.8f : 0.4f});
	}
	if (Telemetry.bHandbrake != Previous.bHandbrake)
	{
		OutEvents.Add({Telemetry.bHandbrake ? ECarSoundEvent::HandbrakePull : ECarSoundEvent::HandbrakeRelease, 1.f});
	}
	if (Telemetry.bCranking && !Previous.bCranking)
	{
		OutEvents.Add({ECarSoundEvent::StarterSolenoid, 1.f});
	}
	if (Previous.bEngineRunning && !Telemetry.bEngineRunning)
	{
		const bool bStalled = Telemetry.StallCount != Previous.StallCount;
		OutEvents.Add({ECarSoundEvent::StallShudder, bStalled ? 1.f : 0.35f});
	}
}

FCarSoundInputs FCarSoundMapper::Update(const FCarTelemetry& Telemetry, const FCarSoundEnvironment& Environment, float DeltaSeconds,
	TArray<FCarSoundEventRequest>& OutEvents)
{
	if (bHasPrevious)
	{
		DetectDiscreteEvents(Telemetry, OutEvents);
		DetectBumps(Telemetry, DeltaSeconds, OutEvents);
	}
	else
	{
		for (int32 Wheel = 0; Wheel < CarNumWheels; ++Wheel)
		{
			WheelLoadBaselineN[Wheel] = Telemetry.LoadN[Wheel];
		}
	}
	Previous = Telemetry;
	bHasPrevious = true;

	FCarSoundInputs Inputs;
	Inputs.EngineRpm = Telemetry.EngineRpm;
	Inputs.Throttle = Telemetry.Throttle;
	Inputs.Load = Telemetry.EngineTorqueNm / FMath::Max(Environment.FullLoadTorqueNm, 1.f);
	Inputs.Boost = Telemetry.Boost;
	Inputs.Clutch = Telemetry.Clutch;
	Inputs.bEngineRunning = Telemetry.bEngineRunning;
	Inputs.bCranking = Telemetry.bCranking;
	Inputs.bRevLimiter = Telemetry.bEngineRunning && Telemetry.Throttle > 0.5f && Telemetry.EngineRpm > Environment.RevLimitRpm - 120.f;
	Inputs.bGrinding = Telemetry.bGrinding;
	Inputs.EngagedGear = Telemetry.EngagedGear;

	Inputs.SpeedMps = FMath::Abs(Telemetry.LocalVelocityMps.X);
	const FVector2D AirVelocity = Environment.WindLocalMps - FVector2D(Telemetry.LocalVelocityMps.X, Telemetry.LocalVelocityMps.Y);
	Inputs.AirSpeedMps = static_cast<float>(AirVelocity.Size());
	for (int32 Surface = 0; Surface < static_cast<int32>(ECarRoadSurface::Count); ++Surface)
	{
		Inputs.SurfaceWeights[Surface] = Environment.SurfaceWeights[Surface];
	}
	Inputs.Wetness = Environment.Wetness;
	Inputs.SquealLevel = ComputeSquealLevel(Telemetry, Inputs.SquealPitch);
	Inputs.bIndicatorOn = Environment.bIndicatorOn;
	return Inputs;
}
