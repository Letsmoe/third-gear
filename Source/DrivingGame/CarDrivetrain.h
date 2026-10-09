#pragma once

#include "CoreMinimal.h"
#include "CarSimTypes.h"

/**
 * Engine + clutch + manual gearbox + open differential + wheel spin + tyre forces, independent of Unreal physics.
 * Runs on the physics thread once per physics step and sub-steps internally (wheel spin is stiff).
 *
 * Per sub-step:
 *  1. explicit torques: combustion (+ turbo lag, idle controller, rev limiter), starter; tyre reaction torque on each
 *     wheel, integrated semi-implicitly (linearised tyre stiffness) so it is stable at any speed;
 *  2. a small sequential-impulse solver for the couplings, each with a torque limit:
 *     clutch (engine <-> gearbox/differential, limit = clutch capacity from the pedal => progressive slip, lock-up,
 *     stalling all emerge naturally), engine friction, brakes + rolling resistance (friction that can hold a wheel at 0).
 */
class FCarDrivetrain
{
public:
	/** Contact state of one wheel for the current physics step, in the wheel's own frame. */
	struct FWheelContact
	{
		bool bContact = false;
		float Vx = 0.f;     // contact point velocity along the wheel heading, m/s
		float Vy = 0.f;     // ... to the wheel's right, m/s
		float LoadN = 0.f;  // normal force
		float Grip = 1.f;   // surface grip factor (1 = dry asphalt)
		float PeakSlipScale = 1.f;       // grip peak at more slip on snow and ice
		float ShapeCScale = 1.f;         // slip curve falloff after the peak
		float ExtraRollingResistance = 0.f; // loose snow drag, coefficient added to the tyre's own
	};

	/** Tyre forces averaged over the step, wheel frame. */
	struct FWheelForces
	{
		float Fx = 0.f;     // forward
		float Fy = 0.f;     // to the wheel's right
		float Mz = 0.f;     // aligning moment about the steering axis, + = turns the wheel clockwise (seen from above)
	};

	void Init(const FCarSimParams& InParams);
	void Reset(bool bEngineRunning);

	/** @param MassPerWheel used to keep low-speed tyre forces from overshooting within one physics step. */
	void Step(float Dt, const FCarDriverInput& Input, const FWheelContact (&Contacts)[CarNumWheels], float MassPerWheel,
	          FWheelForces (&OutForces)[CarNumWheels]);

	void FillTelemetry(FCarTelemetry& Out) const;

	float GetWheelOmega(int32 Wheel) const { return WheelOmega[Wheel]; }
	float GetEngineRpm() const;

	/** Full-load torque curve, Nm. */
	float FullLoadTorque(float Rpm) const;
	/** Friction + pumping drag of the engine at closed throttle, Nm (positive). */
	float FrictionTorque(float Rpm) const;
	/** Torque the clutch can transmit at the given pedal position (1 = pressed). */
	float ClutchCapacity(float Pedal) const;

private:
	struct FTireResult
	{
		float Fx = 0.f, Fy = 0.f, Mz = 0.f;
		float dFxdOmega = 0.f; // linearised stiffness for the implicit wheel update, N per rad/s
		float SlipRatio = 0.f, SlipAngle = 0.f;
	};
	FTireResult EvaluateTire(int32 Wheel, const FWheelContact& Contact, float Omega, float MassPerWheel, float Dt) const;

	float GearRatio(int32 Gear) const; // includes final drive, signed (reverse < 0), 0 in neutral
	void UpdateGearbox(const FCarDriverInput& Input);
	void UpdateIgnition(const FCarDriverInput& Input, float Dt);
	float CombustionTorque(const FCarDriverInput& Input, float H);

	FCarSimParams P;
	float WheelInertia[CarNumWheels] = {};
	bool bDriven[CarNumWheels] = {true, true, false, false};
	bool bFront[CarNumWheels] = {true, true, false, false};

	// State
	float EngineOmega = 0.f;
	float WheelOmega[CarNumWheels] = {};
	float AbsFactor[CarNumWheels] = {1.f, 1.f, 1.f, 1.f};
	float Boost = 0.f;
	float IdleIntegral = 0.f;
	bool bRunning = false;
	bool bCranking = false;
	bool bRevCut = false;
	bool bReachedRunningSpeed = false;
	bool bPrevStarter = false;
	float CrankTime = 0.f;
	int32 StallCount = 0;
	int32 EngagedGear = 0;
	int32 SelectedGear = 0;
	bool bGrinding = false;

	// Last step, for telemetry
	float LastClutchCapacity = 0.f;
	float LastClutchTorque = 0.f;
	float LastEngineTorque = 0.f;
	FCarDriverInput LastInput;
	FTireResult LastTire[CarNumWheels];
	float LastLoad[CarNumWheels] = {};
	bool LastContact[CarNumWheels] = {};
	bool bLastAbsActive = false;
};
