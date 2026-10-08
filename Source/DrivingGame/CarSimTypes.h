#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/**
 * Plain data shared between the game thread (pawn, input, tests) and the physics-thread car simulation.
 * All values in SI units (m, s, kg, N, Nm, rad) unless the name says otherwise.
 */

/** Physics parameters, copied from UCarSettings when the vehicle's physics state is created. */
struct FCarSimParams
{
	// --- Chassis / aero ---
	float MassKg = 1300.f;
	float DragAreaM2 = 0.67f;          // Cd * frontal area
	float AirDensity = 1.2f;

	// --- Tyres (simplified Pacejka "magic formula", combined slip via normalised slip vector) ---
	float RollingRadiusM = 0.31f;
	float TirePeakMu = 1.1f;           // longitudinal peak friction, dry asphalt
	float TireLateralMuScale = 0.9f;   // lateral peak = TirePeakMu * this (friction ellipse)
	float TireLoadSensitivity = 0.1f;  // mu drops by this fraction per +100 % load
	float TirePeakSlipRatio = 0.12f;
	float TirePeakSlipAngleDeg = 8.f;
	float TireShapeC = 1.3f;           // falloff after the peak: sliding grip = sin(C * pi / 2) of peak
	float TireLowSpeedMps = 1.0f;      // slip denominator floor (keeps low-speed slip well-behaved)
	float RollingResistance = 0.011f;
	float PneumaticTrailM = 0.035f;
	float CasterTrailM = 0.02f;
	float WheelInertiaFront = 1.2f;    // wheel + tyre + brake disc + half shaft, kg m^2
	float WheelInertiaRear = 1.0f;

	// --- Brakes ---
	float MaxBrakeTorqueFront = 2200.f; // per wheel at full pedal
	float MaxBrakeTorqueRear = 1000.f;
	float BrakePedalExponent = 1.3f;
	float HandbrakeTorque = 1500.f;     // per rear wheel
	bool bABS = true;
	float AbsSlipRatio = 0.14f;

	// --- Engine ---
	TArray<FVector2f> TorqueCurve;      // (rpm, full-load net torque Nm)
	float IdleRpm = 800.f;
	float RevLimitRpm = 6400.f;
	float StallRpm = 350.f;
	float EngineInertia = 0.15f;        // crank + dual-mass flywheel + clutch, kg m^2
	float FrictionTorqueBase = 18.f;    // closed-throttle drag (friction + pumping), Nm
	float FrictionTorquePerKrpm = 6.f;
	float ThrottleExponent = 1.4f;      // pedal -> torque demand (progressive, eases smooth starts)
	float NaturallyAspiratedFraction = 0.55f; // share of full-load torque available without boost
	float TurboLagSeconds = 0.35f;
	float IdleMaxTorque = 60.f;         // idle/anti-stall controller authority
	float StarterTorque = 90.f;
	float StarterFireRpm = 250.f;

	// --- Clutch ---
	float ClutchMaxTorque = 380.f;
	float ClutchEngagedBelow = 0.25f;   // pedal position (0 = released) below which the clutch is fully engaged
	float ClutchReleasedAbove = 0.8f;   // pedal position above which it transmits nothing
	float ClutchCurveExponent = 2.f;

	// --- Gearbox ---
	TArray<float> GearRatios;           // forward gears 1..N
	float ReverseRatio = 3.6f;
	float FinalDrive = 3.647f;
	float DrivetrainEfficiency = 0.92f;
	float SyncRpmTolerance = 300.f;     // engaging a gear without the clutch works only if engine speed matches

	// --- Steering ---
	float SteeringRatio = 13.6f;        // steering wheel angle / mean road wheel angle
	float MaxRoadWheelAngleDeg = 36.f;
	float ComplianceSteerDegPerKN = 0.2f; // front wheels steer out of the corner under lateral load (bushings) -> understeer
};

/** Driver controls, written by the game thread every frame. */
struct FCarDriverInput
{
	float SteeringWheelDeg = 0.f;  // + = clockwise (right)
	float Throttle = 0.f;          // pedals 0..1
	float Brake = 0.f;
	float Clutch = 0.f;            // 1 = pedal fully pressed (disengaged)
	bool bHandbrake = false;
	int32 SelectedGear = 0;        // -1 = reverse, 0 = neutral, 1..N
	bool bStarter = false;         // key held in the start position
	bool bIgnitionOn = true;       // false switches the engine off
};

constexpr int32 CarNumWheels = 4; // FL, FR, RL, RR (front-wheel drive)

/** Simulation output, written by the physics thread every step. */
struct FCarTelemetry
{
	double SimTime = 0.0;          // accumulated physics time, s
	FVector PositionM = FVector::ZeroVector;
	float YawDeg = 0.f;
	FVector LocalVelocityMps = FVector::ZeroVector; // x forward, y right, z up
	FVector LocalAccelMps2 = FVector::ZeroVector;   // low-pass filtered
	float YawRateDegPerSec = 0.f;
	float SpeedKmh = 0.f;          // signed forward speed

	float EngineRpm = 0.f;
	bool bEngineRunning = false;
	bool bCranking = false;
	int32 StallCount = 0;
	int32 EngagedGear = 0;
	int32 SelectedGear = 0;
	bool bGrinding = false;
	float ClutchCapacityNm = 0.f;
	float ClutchTorqueNm = 0.f;
	float ClutchSlipRpm = 0.f;
	float EngineTorqueNm = 0.f;    // net combustion torque minus friction
	float Boost = 0.f;
	float Throttle = 0.f;
	float Brake = 0.f;
	float Clutch = 0.f;
	float RoadWheelAngleDeg = 0.f;
	float SteeringRackTorqueNm = 0.f; // kingpin torque of both front wheels / steering ratio = torque at the steering wheel without assist, + = clockwise
	bool bAbsActive = false;
	bool bHandbrake = false;
	FVector AppliedForceN = FVector::ZeroVector; // sum of tyre + drag forces applied to the body, local frame (diagnostics)

	float WheelRpm[CarNumWheels] = {};
	float SlipRatio[CarNumWheels] = {};
	float SlipAngleDeg[CarNumWheels] = {};
	float LoadN[CarNumWheels] = {};
	float ForceLongN[CarNumWheels] = {};
	float ForceLatN[CarNumWheels] = {};
	bool bContact[CarNumWheels] = {};
};

/** Lock-protected exchange between game and physics thread (both sides only copy small structs under the lock). */
struct FCarSharedState
{
	FCriticalSection Lock;
	FCarDriverInput Input;
	FCarTelemetry Telemetry;
	/** Incremented by the game thread to reset the drivetrain (e.g. after a teleport); the physics thread compares. */
	int32 ResetCounter = 0;
	bool bResetEngineRunning = true;
};
