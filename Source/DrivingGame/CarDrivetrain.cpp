#include "CarDrivetrain.h"

namespace
{
constexpr float RpmToRadPerSec = 2.f * UE_PI / 60.f;
constexpr float RadPerSecToRpm = 60.f / (2.f * UE_PI);
constexpr float Gravity = 9.81f;
/** Target length of a drivetrain sub-step. Wheel spin against the tyre is stiff; 0.25 ms keeps it accurate. */
constexpr float SubStepSeconds = 0.00025f;
constexpr int32 SolverIterations = 10;
/** Engine speed above which the engine is considered started (stall detection only arms after this). */
constexpr float RunningRpm = 600.f;
/** A stalled engine with ignition on restarts if the wheels spin it above this (bump start). */
constexpr float BumpStartRpm = 500.f;
/** Idle controller gains: Nm per rpm of error, Nm per (rpm * s). */
constexpr float IdleKp = 0.15f;
constexpr float IdleKi = 0.4f;
/** ABS: rate at which the brake pressure factor is released / re-applied, 1/s. */
constexpr float AbsReleaseRate = 12.f;
constexpr float AbsApplyRate = 4.f;
constexpr float AbsMinSpeed = 2.f;
}

void FCarDrivetrain::Init(const FCarSimParams& InParams)
{
	P = InParams;
	if (P.TorqueCurve.IsEmpty())
	{
		P.TorqueCurve = {{0.f, 100.f}, {7000.f, 100.f}};
	}
	if (P.GearRatios.IsEmpty())
	{
		P.GearRatios = {1.f};
	}
	for (int32 i = 0; i < CarNumWheels; ++i)
	{
		WheelInertia[i] = FMath::Max(0.05f, bFront[i] ? P.WheelInertiaFront : P.WheelInertiaRear);
	}
	P.EngineInertia = FMath::Max(0.02f, P.EngineInertia);
	Reset(false);
}

void FCarDrivetrain::Reset(bool bEngineRunning)
{
	bRunning = bEngineRunning;
	bReachedRunningSpeed = bEngineRunning;
	bCranking = false;
	bRevCut = false;
	EngineOmega = bEngineRunning ? P.IdleRpm * RpmToRadPerSec : 0.f;
	for (int32 i = 0; i < CarNumWheels; ++i)
	{
		WheelOmega[i] = 0.f;
		AbsFactor[i] = 1.f;
	}
	Boost = 0.f;
	IdleIntegral = 0.f;
	EngagedGear = 0;
	SelectedGear = 0;
	bGrinding = false;
}

float FCarDrivetrain::GetEngineRpm() const
{
	return EngineOmega * RadPerSecToRpm;
}

float FCarDrivetrain::FullLoadTorque(float Rpm) const
{
	const TArray<FVector2f>& Curve = P.TorqueCurve;
	if (Rpm <= Curve[0].X)
	{
		return Curve[0].Y;
	}
	for (int32 i = 1; i < Curve.Num(); ++i)
	{
		if (Rpm <= Curve[i].X)
		{
			const float Alpha = (Rpm - Curve[i - 1].X) / FMath::Max(1.f, Curve[i].X - Curve[i - 1].X);
			return FMath::Lerp(Curve[i - 1].Y, Curve[i].Y, Alpha);
		}
	}
	return Curve.Last().Y;
}

float FCarDrivetrain::FrictionTorque(float Rpm) const
{
	return P.FrictionTorqueBase + P.FrictionTorquePerKrpm * FMath::Abs(Rpm) * 0.001f;
}

float FCarDrivetrain::ClutchCapacity(float Pedal) const
{
	const float Range = FMath::Max(0.01f, P.ClutchReleasedAbove - P.ClutchEngagedBelow);
	const float Engagement = FMath::Clamp((P.ClutchReleasedAbove - Pedal) / Range, 0.f, 1.f);
	return P.ClutchMaxTorque * FMath::Pow(Engagement, P.ClutchCurveExponent);
}

float FCarDrivetrain::GearRatio(int32 Gear) const
{
	if (Gear > 0 && Gear <= P.GearRatios.Num())
	{
		return P.GearRatios[Gear - 1] * P.FinalDrive;
	}
	if (Gear == -1)
	{
		return -P.ReverseRatio * P.FinalDrive;
	}
	return 0.f;
}

void FCarDrivetrain::UpdateGearbox(const FCarDriverInput& Input)
{
	SelectedGear = FMath::Clamp(Input.SelectedGear, -1, P.GearRatios.Num());
	if (SelectedGear == EngagedGear)
	{
		bGrinding = false;
		return;
	}
	if (SelectedGear == 0)
	{
		EngagedGear = 0;
		bGrinding = false;
		return;
	}

	// The synchronisers can only match the gearbox input shaft to the output if it is free (clutch pressed),
	// or if the driver has matched engine speed to road speed (clutchless shift). Reverse has no synchro.
	const float Carrier = 0.5f * (WheelOmega[0] + WheelOmega[1]);
	const float TargetRatio = GearRatio(SelectedGear);
	const bool bClutchOpen = ClutchCapacity(Input.Clutch) < 2.f;
	const bool bSpeedMatched = FMath::Abs(EngineOmega - TargetRatio * Carrier) < P.SyncRpmTolerance * RpmToRadPerSec;
	bool bCanEngage = bClutchOpen || bSpeedMatched;
	if (SelectedGear == -1)
	{
		bCanEngage = bClutchOpen && FMath::Abs(Carrier * P.RollingRadiusM) < 0.5f;
	}

	if (bCanEngage)
	{
		EngagedGear = SelectedGear;
		bGrinding = false;
	}
	else
	{
		EngagedGear = 0; // lever is out of the old gear, new one refuses: grinding, nothing transmitted
		bGrinding = true;
	}
}

void FCarDrivetrain::UpdateIgnition(const FCarDriverInput& Input, float Dt)
{
	const float Rpm = GetEngineRpm();
	if (!Input.bIgnitionOn)
	{
		bRunning = false;
		bCranking = false;
		return;
	}

	// Starter, with the usual clutch interlock: only in neutral or with the clutch pressed.
	const bool bInterlockOk = EngagedGear == 0 || ClutchCapacity(Input.Clutch) < 2.f;
	bCranking = Input.bStarter && !bRunning && bInterlockOk;
	CrankTime = bCranking ? CrankTime + Dt : 0.f;
	if (bCranking && Rpm >= P.StarterFireRpm && CrankTime > 0.15f)
	{
		bRunning = true;
		bReachedRunningSpeed = false;
		IdleIntegral = 0.f;
	}

	// Bump start: ignition on, engine turned by the wheels.
	if (!bRunning && !bCranking && Rpm > BumpStartRpm)
	{
		bRunning = true;
		bReachedRunningSpeed = true;
	}

	if (bRunning)
	{
		if (Rpm > RunningRpm)
		{
			bReachedRunningSpeed = true;
		}
		else if (bReachedRunningSpeed && Rpm < P.StallRpm)
		{
			bRunning = false;
			++StallCount;
		}
	}
}

float FCarDrivetrain::CombustionTorque(const FCarDriverInput& Input, float H)
{
	const float Rpm = GetEngineRpm();
	if (!bRunning)
	{
		Boost += (0.f - Boost) * FMath::Min(1.f, H / P.TurboLagSeconds);
		return 0.f;
	}

	const float Friction = FrictionTorque(Rpm);
	const float Gross = FullLoadTorque(Rpm) + Friction; // combustion torque at full load before friction

	// Driver demand (drive-by-wire: pedal maps to a fraction of the available torque).
	const float Demand = FMath::Pow(FMath::Clamp(Input.Throttle, 0.f, 1.f), P.ThrottleExponent) * Gross;

	// Idle / anti-stall controller: feed-forward of the friction plus PI on the speed error.
	const float Error = P.IdleRpm - Rpm;
	float Idle = Friction + IdleKp * Error + IdleIntegral;
	if (Idle >= Demand)
	{
		IdleIntegral = FMath::Clamp(IdleIntegral + IdleKi * Error * H, -Friction, P.IdleMaxTorque);
	}
	Idle = FMath::Clamp(Idle, 0.f, P.IdleMaxTorque);
	float Torque = FMath::Max(Demand, Idle);

	// Turbo: only the naturally aspirated share is available instantly; boost builds with a lag (slower at low rpm).
	const float NA = FMath::Clamp(P.NaturallyAspiratedFraction, 0.05f, 1.f);
	const float Available = Gross * (NA + (1.f - NA) * Boost);
	const float BoostTarget = NA < 1.f ? FMath::Clamp((Torque / FMath::Max(1.f, Gross) - NA) / (1.f - NA), 0.f, 1.f) : 0.f;
	const float LagScale = FMath::GetMappedRangeValueClamped(FVector2f(1000.f, 2000.f), FVector2f(3.f, 1.f), Rpm);
	Boost += (BoostTarget - Boost) * FMath::Min(1.f, H / FMath::Max(0.01f, P.TurboLagSeconds * LagScale));
	Torque = FMath::Min(Torque, Available);

	// Rev limiter: fuel cut with hysteresis.
	if (Rpm > P.RevLimitRpm)
	{
		bRevCut = true;
	}
	else if (Rpm < P.RevLimitRpm - 150.f)
	{
		bRevCut = false;
	}
	return bRevCut ? 0.f : Torque;
}

FCarDrivetrain::FTireResult FCarDrivetrain::EvaluateTire(int32 Wheel, const FWheelContact& Contact, float Omega, float MassPerWheel, float Dt) const
{
	FTireResult Out;
	if (!Contact.bContact || Contact.LoadN <= 0.f)
	{
		return Out;
	}
	const float R = P.RollingRadiusM;
	const float VRef = FMath::Max(FMath::Abs(Contact.Vx), P.TireLowSpeedMps);
	const float SlipVelocity = Omega * R - Contact.Vx;
	const float Kappa = SlipVelocity / VRef;
	const float TanAlpha = Contact.Vy / VRef;
	Out.SlipRatio = Kappa;
	Out.SlipAngle = FMath::Atan(TanAlpha);

	// Grip with load sensitivity (heavily loaded tyres have a lower friction coefficient).
	const float NominalLoad = P.MassKg * Gravity / CarNumWheels;
	const float LoadFactor = FMath::Clamp(1.f - P.TireLoadSensitivity * (Contact.LoadN / NominalLoad - 1.f), 0.5f, 1.2f);
	const float MuFz = P.TirePeakMu * Contact.Grip * LoadFactor * Contact.LoadN;
	const float LateralScale = FMath::Clamp(P.TireLateralMuScale, 0.5f, 1.5f); // friction ellipse

	// Normalised combined slip: s = 1 is the peak in any direction (friction circle).
	const float PeakSlipScale = FMath::Max(0.1f, Contact.PeakSlipScale);
	const float KappaPeak = FMath::Max(0.01f, P.TirePeakSlipRatio * PeakSlipScale);
	const float TanAlphaPeak = FMath::Tan(FMath::DegreesToRadians(FMath::Max(1.f, P.TirePeakSlipAngleDeg))) * PeakSlipScale;
	const float Sx = Kappa / KappaPeak;
	const float Sy = TanAlpha / TanAlphaPeak;
	const float S = FMath::Sqrt(Sx * Sx + Sy * Sy);
	const float C = FMath::Clamp(P.TireShapeC * Contact.ShapeCScale, 1.01f, 1.9f);
	const float B = FMath::Tan(UE_HALF_PI / C); // puts the peak of sin(C atan(B s)) at s = 1

	float dFxdSx;
	if (S < 1e-4f)
	{
		const float Slope = MuFz * C * B; // d(MF)/ds at 0
		Out.Fx = Slope * Sx;
		Out.Fy = -Slope * Sy * LateralScale;
		dFxdSx = Slope;
	}
	else
	{
		const float Arg = C * FMath::Atan(B * S);
		const float MF = FMath::Sin(Arg);
		const float MFPrime = FMath::Cos(Arg) * C * B / (1.f + B * B * S * S);
		const float Force = MuFz * MF;
		Out.Fx = Force * Sx / S;
		Out.Fy = -Force * Sy / S * LateralScale;
		const float Wx = (Sx / S) * (Sx / S);
		dFxdSx = MuFz * (MFPrime * Wx + MF / S * (1.f - Wx));
	}
	Out.dFxdOmega = FMath::Max(0.f, dFxdSx * R / (KappaPeak * VRef));

	// Low speed: never apply more force in one physics step than it takes to stop the slip (no jitter when parked).
	const float MaxFx = 0.5f * MassPerWheel * FMath::Abs(SlipVelocity) / Dt;
	if (FMath::Abs(Out.Fx) > MaxFx)
	{
		Out.Fx = FMath::Sign(Out.Fx) * MaxFx;
		Out.dFxdOmega = 0.5f * MassPerWheel * R / Dt;
	}
	const float MaxFy = 0.5f * MassPerWheel * FMath::Abs(Contact.Vy) / Dt;
	Out.Fy = FMath::Clamp(Out.Fy, -MaxFy, MaxFy);

	// Self-aligning moment: lateral force acts behind the steering axis (pneumatic trail, which collapses towards
	// the grip limit, plus caster trail). Lateral force to the right acting behind the axis turns the wheel left.
	const float PneumaticTrail = P.PneumaticTrailM * FMath::Max(0.f, 1.f - FMath::Abs(Sy));
	Out.Mz = -(PneumaticTrail + P.CasterTrailM) * Out.Fy;
	return Out;
}

void FCarDrivetrain::Step(float Dt, const FCarDriverInput& Input, const FWheelContact (&Contacts)[CarNumWheels], float MassPerWheel,
                          FWheelForces (&OutForces)[CarNumWheels])
{
	LastInput = Input;
	for (int32 i = 0; i < CarNumWheels; ++i)
	{
		OutForces[i] = FWheelForces();
		LastLoad[i] = Contacts[i].bContact ? Contacts[i].LoadN : 0.f;
		LastContact[i] = Contacts[i].bContact;
	}
	if (Dt <= 0.f)
	{
		return;
	}

	UpdateGearbox(Input);
	UpdateIgnition(Input, Dt);

	const int32 NumSub = FMath::Clamp(FMath::CeilToInt(Dt / SubStepSeconds), 1, 64);
	const float H = Dt / NumSub;
	const float Weight = 1.f / NumSub;

	const float ClutchCap = EngagedGear != 0 ? ClutchCapacity(Input.Clutch) : 0.f;
	const float G = GearRatio(EngagedGear);
	const float BrakePedal = FMath::Pow(FMath::Clamp(Input.Brake, 0.f, 1.f), P.BrakePedalExponent);
	const float Speed = 0.5f * (FMath::Abs(Contacts[0].Vx) + FMath::Abs(Contacts[1].Vx));
	bLastAbsActive = false;

	for (int32 Sub = 0; Sub < NumSub; ++Sub)
	{
		// --- 1. Engine: combustion + starter (explicit). Friction is handled by the solver below. ---
		const float Combustion = CombustionTorque(Input, H);
		const float Rpm = GetEngineRpm();
		const float Starter = bCranking ? P.StarterTorque * FMath::Max(0.f, 1.f - FMath::Abs(Rpm) / 350.f) : 0.f;
		EngineOmega += H * (Combustion + Starter) / P.EngineInertia;
		LastEngineTorque = Combustion - FrictionTorque(Rpm);

		// --- 2. Tyres: reaction torque on each wheel, semi-implicit in the wheel speed. ---
		float BrakeLimit[CarNumWheels];
		for (int32 i = 0; i < CarNumWheels; ++i)
		{
			const FTireResult Tire = EvaluateTire(i, Contacts[i], WheelOmega[i], MassPerWheel, Dt);
			const float R = P.RollingRadiusM;
			const float I = WheelInertia[i];
			const float K = Tire.dFxdOmega * R; // d(tyre torque)/d(omega)
			const float NewOmega = WheelOmega[i] - H * Tire.Fx * R / (I + H * K);
			const float FxApplied = (WheelOmega[i] - NewOmega) * I / (H * R);
			WheelOmega[i] = NewOmega;

			OutForces[i].Fx += FxApplied * Weight;
			OutForces[i].Fy += Tire.Fy * Weight;
			OutForces[i].Mz += Tire.Mz * Weight;
			LastTire[i] = Tire;

			// ABS: release brake pressure while the wheel is locking, re-apply otherwise.
			float &Abs = AbsFactor[i];
			if (P.bABS && BrakePedal > 0.f && Speed > AbsMinSpeed && Tire.SlipRatio < -P.AbsSlipRatio * Contacts[i].PeakSlipScale)
			{
				Abs = FMath::Max(0.05f, Abs - AbsReleaseRate * H);
				bLastAbsActive = true;
			}
			else
			{
				Abs = FMath::Min(1.f, Abs + AbsApplyRate * H);
			}

			const float MaxBrake = bFront[i] ? P.MaxBrakeTorqueFront : P.MaxBrakeTorqueRear;
			float Brake = MaxBrake * BrakePedal * Abs;
			if (!bFront[i] && Input.bHandbrake)
			{
				Brake += P.HandbrakeTorque;
			}
			const float Rolling = (P.RollingResistance + Contacts[i].ExtraRollingResistance) * LastLoad[i] * R;
			// Gear mesh losses scale with the transmitted torque; modelled as friction on the driven wheels.
			const float DrivelineLoss = bDriven[i] ? (1.f - P.DrivetrainEfficiency) * FMath::Abs(LastClutchTorque * G) * 0.5f : 0.f;
			BrakeLimit[i] = (Brake + Rolling + DrivelineLoss) * H;
		}

		// --- 3. Couplings with torque limits (sequential impulses; all limits are friction-like). ---
		const float EngineFrictionLimit = FrictionTorque(GetEngineRpm()) * H;
		const float ClutchLimit = ClutchCap * H;
		float ClutchImpulse = 0.f, EngineImpulse = 0.f, BrakeImpulse[CarNumWheels] = {};
		const float HalfG = 0.5f * G;
		const float ClutchInvMass = 1.f / P.EngineInertia + HalfG * HalfG * (1.f / WheelInertia[0] + 1.f / WheelInertia[1]);

		for (int32 Iteration = 0; Iteration < SolverIterations; ++Iteration)
		{
			if (ClutchLimit > 0.f)
			{
				// Constraint: engine speed == G * differential carrier speed.
				const float Cdot = EngineOmega - HalfG * (WheelOmega[0] + WheelOmega[1]);
				const float Old = ClutchImpulse;
				ClutchImpulse = FMath::Clamp(Old - Cdot / ClutchInvMass, -ClutchLimit, ClutchLimit);
				const float Lambda = ClutchImpulse - Old;
				EngineOmega += Lambda / P.EngineInertia;
				WheelOmega[0] -= HalfG * Lambda / WheelInertia[0];
				WheelOmega[1] -= HalfG * Lambda / WheelInertia[1];
			}
			{
				const float Old = EngineImpulse;
				EngineImpulse = FMath::Clamp(Old - EngineOmega * P.EngineInertia, -EngineFrictionLimit, EngineFrictionLimit);
				EngineOmega += (EngineImpulse - Old) / P.EngineInertia;
			}
			for (int32 i = 0; i < CarNumWheels; ++i)
			{
				const float Old = BrakeImpulse[i];
				BrakeImpulse[i] = FMath::Clamp(Old - WheelOmega[i] * WheelInertia[i], -BrakeLimit[i], BrakeLimit[i]);
				WheelOmega[i] += (BrakeImpulse[i] - Old) / WheelInertia[i];
			}
		}
		// Clutch torque as seen by the engine (positive = engine drives the car).
		LastClutchTorque = -ClutchImpulse / H;
	}

	LastClutchCapacity = ClutchCap;
}

void FCarDrivetrain::FillTelemetry(FCarTelemetry& Out) const
{
	Out.EngineRpm = GetEngineRpm();
	Out.bEngineRunning = bRunning;
	Out.bCranking = bCranking;
	Out.StallCount = StallCount;
	Out.EngagedGear = EngagedGear;
	Out.SelectedGear = SelectedGear;
	Out.bGrinding = bGrinding;
	Out.ClutchCapacityNm = LastClutchCapacity;
	Out.ClutchTorqueNm = LastClutchTorque;
	const float G = GearRatio(EngagedGear);
	Out.ClutchSlipRpm = EngagedGear != 0 ? (EngineOmega - 0.5f * G * (WheelOmega[0] + WheelOmega[1])) * RadPerSecToRpm : 0.f;
	Out.EngineTorqueNm = LastEngineTorque;
	Out.Boost = Boost;
	Out.Throttle = LastInput.Throttle;
	Out.Brake = LastInput.Brake;
	Out.Clutch = LastInput.Clutch;
	Out.bAbsActive = bLastAbsActive;
	Out.bHandbrake = LastInput.bHandbrake;
	for (int32 i = 0; i < CarNumWheels; ++i)
	{
		Out.WheelRpm[i] = WheelOmega[i] * RadPerSecToRpm;
		Out.SlipRatio[i] = LastTire[i].SlipRatio;
		Out.SlipAngleDeg[i] = FMath::RadiansToDegrees(LastTire[i].SlipAngle);
		Out.LoadN[i] = LastLoad[i];
		Out.ForceLongN[i] = LastTire[i].Fx;
		Out.ForceLatN[i] = LastTire[i].Fy;
		Out.bContact[i] = LastContact[i];
	}
}
