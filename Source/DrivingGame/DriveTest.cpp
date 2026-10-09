#include "DriveTest.h"

#include "CarMovementComponent.h"
#include "CarPawn.h"
#include "CarSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "GameFramework/PlayerController.h"

DEFINE_LOG_CATEGORY_STATIC(LogDriveTest, Log, All);

namespace
{
/** Fraction of the way from A to B at which Threshold is crossed. */
float CrossFraction(float A, float B, float Threshold)
{
	return FMath::IsNearlyEqual(A, B) ? 1.f : FMath::Clamp((Threshold - A) / (B - A), 0.f, 1.f);
}

/**
 * Clutch pedal over time for a pull-away: from floored to the start of the bite point in ToBite seconds, through
 * the bite zone (pedal 0.8 -> 0.4) in Through seconds, then fully released in Out seconds. T < 0: floored.
 */
float ClutchProfile(float T, float ToBite, float Through, float Out)
{
	if (T < 0.f)
	{
		return 1.f;
	}
	if (T < ToBite)
	{
		return FMath::Lerp(1.f, 0.8f, T / ToBite);
	}
	T -= ToBite;
	if (T < Through)
	{
		return FMath::Lerp(0.8f, 0.4f, T / Through);
	}
	T -= Through;
	return FMath::Lerp(0.4f, 0.f, FMath::Clamp(T / Out, 0.f, 1.f));
}

/** Time at which Value crossed Threshold between the previous and current telemetry sample. */
double CrossTime(const FCarTelemetry& Prev, const FCarTelemetry& Cur, float PrevValue, float CurValue, float Threshold)
{
	return FMath::Lerp(Prev.SimTime, Cur.SimTime, double(CrossFraction(PrevValue, CurValue, Threshold)));
}
}

ADriveTestRunner::ADriveTestRunner()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void ADriveTestRunner::BeginPlay()
{
	Super::BeginPlay();
	FCarSurfaceConditions Conditions;
	if (FParse::Value(FCommandLine::Get(), TEXT("DriveTestSurface="), TestSurfaceName) && CarSurfaceGrip::TestPreset(TestSurfaceName, Conditions))
	{
		TestSurface = Conditions;
	}
	BuildSteps();
	if (TestSurface.IsSet())
	{
		KeepSurfaceSteps();
	}
	Report(TEXT("Drive test started"));
}

void ADriveTestRunner::Report(const FString& Line)
{
	UE_LOG(LogDriveTest, Display, TEXT("DRIVETEST %s"), *Line);
}

void ADriveTestRunner::AddStep(const FString& Name, TFunction<void()> Start, TFunction<bool(const FCarTelemetry&)> Update)
{
	Steps.Add({Name, MoveTemp(Start), MoveTemp(Update)});
}

void ADriveTestRunner::AddPlace(float XM, float YM, float Yaw, bool bEngineRunning, float SettleSeconds)
{
	AddStep(TEXT("place"), [this, XM, YM, Yaw, bEngineRunning]()
	{
		Input = FCarDriverInput();
		ApplyTestSurface(false);
		ShiftPhase = 0;
		LaneY = YM;
		ShiftRpm = 6200.f;
		LaunchRpm = 3000.f;
		if (ACarPawn* Pawn = Car.Get())
		{
			FVector Ground(XM * 100.f, YM * 100.f, 0.f);
			FHitResult Hit;
			FCollisionQueryParams Params;
			Params.AddIgnoredActor(Pawn);
			if (GetWorld()->LineTraceSingleByChannel(Hit, Ground + FVector(0, 0, 500), Ground - FVector(0, 0, 500), ECC_WorldStatic, Params))
			{
				Ground = Hit.ImpactPoint;
			}
			Pawn->PlaceCar(Ground, Yaw, bEngineRunning);
		}
	}, [this, SettleSeconds](const FCarTelemetry& T) { return StepTime(T) > SettleSeconds; });
}

void ADriveTestRunner::AddWait(float Seconds)
{
	AddStep(TEXT("wait"), nullptr, [this, Seconds](const FCarTelemetry& T) { return StepTime(T) > Seconds; });
}

void ADriveTestRunner::SteerToLane(const FCarTelemetry& T)
{
	// Simple lane keeping along +X: road wheel angle from lateral offset and heading error, softer at speed.
	const float LateralError = T.PositionM.Y - LaneY;
	const float HeadingError = FRotator::NormalizeAxis(T.YawDeg);
	const float Speed = FMath::Max(1.f, FMath::Abs(T.LocalVelocityMps.X));
	const float RoadDeg = FMath::Clamp(-(1.5f * LateralError + 2.f * HeadingError) / FMath::Max(1.f, Speed / 8.f), -8.f, 8.f);
	Input.SteeringWheelDeg = RoadDeg * GetDefault<UCarSettings>()->SteeringRatio;
}

bool ADriveTestRunner::ShiftTo(const FCarTelemetry& T, int32 Gear, float ClutchOutSeconds, float ThrottleAfter)
{
	const float S = float(T.SimTime - ShiftStart);
	if (S < 0.12f)
	{
		Input.Throttle = 0.f;
		Input.Clutch = 1.f;
		return false;
	}
	if (S < 0.25f)
	{
		Input.Throttle = 0.f;
		Input.Clutch = 1.f;
		Input.SelectedGear = Gear;
		return false;
	}
	const float Alpha = FMath::Clamp((S - 0.25f) / FMath::Max(0.01f, ClutchOutSeconds), 0.f, 1.f);
	Input.SelectedGear = Gear;
	Input.Clutch = 1.f - Alpha;
	Input.Throttle = ThrottleAfter * Alpha;
	return Alpha >= 1.f;
}

void ADriveTestRunner::DriveFlatOut(const FCarTelemetry& T, int32 MaxGear)
{
	switch (ShiftPhase)
	{
	case 0: // prepare launch: clutch in, first gear
		Input.Clutch = 1.f;
		Input.SelectedGear = 1;
		Input.Throttle = 0.f;
		ShiftStart = T.SimTime;
		ShiftPhase = 1;
		break;
	case 1: // hold launch rpm
		Input.Throttle = FMath::Clamp(0.4f + (LaunchRpm - T.EngineRpm) * 0.001f, 0.f, 1.f);
		if (T.SimTime - ShiftStart > 1.5)
		{
			ShiftPhase = 2;
			ShiftStart = T.SimTime;
			MarkTime = T.SimTime; // launch = start of clutch release
		}
		break;
	case 2: // launch: full throttle, clutch out over 0.8 s
	{
		const float Alpha = FMath::Clamp(float(T.SimTime - ShiftStart) / 0.8f, 0.f, 1.f);
		Input.Throttle = 1.f;
		Input.Clutch = 1.f - Alpha;
		if (Alpha >= 1.f)
		{
			ShiftPhase = 3;
		}
		break;
	}
	case 3: // flat out, upshift at ShiftRpm
		Input.Throttle = 1.f;
		Input.Clutch = 0.f;
		if (T.EngineRpm >= ShiftRpm && T.EngagedGear > 0 && T.EngagedGear < MaxGear)
		{
			ShiftTarget = T.EngagedGear + 1;
			ShiftStart = T.SimTime;
			ShiftPhase = 4;
		}
		break;
	case 4:
		if (ShiftTo(T, ShiftTarget, 0.3f, 1.f))
		{
			ShiftPhase = 3;
		}
		break;
	default:
		break;
	}
}

void ADriveTestRunner::HoldSpeed(const FCarTelemetry& T, float TargetKmh)
{
	const float Dt = float(T.SimTime - Prev.SimTime);
	const float Error = TargetKmh - T.SpeedKmh;
	SpeedIntegral = FMath::Clamp(SpeedIntegral + Error * Dt, -20.f, 40.f);
	Input.Throttle = FMath::Clamp(0.1f + 0.08f * Error + 0.03f * SpeedIntegral, 0.f, 1.f);
	Input.Clutch = 0.f;
	Input.Brake = 0.f;
}

void ADriveTestRunner::BuildSteps()
{
	const float StartX = -1900.f;

	// ---------------- 1. Idle and static state ----------------
	AddPlace(StartX, -30.f, 0.f, true, 3.f);
	AddStep(TEXT("idle"), [this]() { MaxValue = 0.f; MinValue = 1e6f; Counter = 0; Value2 = 0.f; },
		[this](const FCarTelemetry& T)
		{
			MaxValue = FMath::Max(MaxValue, T.EngineRpm);
			MinValue = FMath::Min(MinValue, T.EngineRpm);
			Value2 += T.EngineRpm;
			++Counter;
			if (StepTime(T) < 2.f)
			{
				return false;
			}
			Report(FString::Printf(TEXT("idle: rpm avg %.0f (min %.0f max %.0f), wheel loads FL %.0f FR %.0f RL %.0f RR %.0f N (front %.1f %%), ride z %.3f m"),
				Value2 / FMath::Max(1, Counter), MinValue, MaxValue, T.LoadN[0], T.LoadN[1], T.LoadN[2], T.LoadN[3],
				100.f * (T.LoadN[0] + T.LoadN[1]) / FMath::Max(1.f, T.LoadN[0] + T.LoadN[1] + T.LoadN[2] + T.LoadN[3]), T.PositionM.Z));
			Summary.Add(FString::Printf(TEXT("Idle: %.0f rpm"), Value2 / FMath::Max(1, Counter)));
			return true;
		});

	// ---------------- 2. Full-throttle run: 0-50, 0-100, top speed ----------------
	AddPlace(StartX, -30.f, 0.f, true);
	AddStep(TEXT("acceleration"), [this]() { MaxValue = 0.f; bFlag = false; Value2 = -1.f; MinValue = -1.f; T160 = -1.f; MarkTime = 0.0; ApplyTestSurface(true); },
		[this](const FCarTelemetry& T)
		{
			SteerToLane(T);
			DriveFlatOut(T, 6);
			const float V = T.SpeedKmh, PV = Prev.SpeedKmh;
			const float Since = float(T.SimTime - MarkTime);
			auto Mark = [&](float Kmh, float& Out)
			{
				if (Out < 0.f && ShiftPhase >= 2 && PV < Kmh && V >= Kmh)
				{
					Out = float(CrossTime(Prev, T, PV, V, Kmh) - MarkTime);
					Report(FString::Printf(TEXT("accel: 0-%.0f km/h in %.2f s (gear %d, %.0f rpm, %.0f m)"), Kmh, Out, T.EngagedGear, T.EngineRpm, T.PositionM.X + 1900.f));
				}
			};
			Mark(50.f, Value2);
			Mark(100.f, MinValue);
			Mark(160.f, T160);
			// Force balance every 20 km/h (diagnostics for tuning).
			if (ShiftPhase == 3 && FMath::FloorToInt(V / 20.f) > FMath::FloorToInt(PV / 20.f) && V > 15.f)
			{
				const float DriveForce = T.ForceLongN[0] + T.ForceLongN[1] + T.ForceLongN[2] + T.ForceLongN[3];
				const float Vms = T.LocalVelocityMps.X;
				const float Drag = 0.5f * 1.2f * GetDefault<UCarSettings>()->DragAreaM2 * Vms * Vms;
				Report(FString::Printf(TEXT("  %3.0f km/h gear %d %4.0f rpm: engine %.0f Nm, clutch %.0f Nm (slip %.0f rpm), tyre Fx %.0f/%.0f/%.0f/%.0f N (slip %.3f/%.3f), drag %.0f N, accel %.2f m/s^2 -> m*a %.0f N vs applied %.0f N, boost %.2f"),
					V, T.EngagedGear, T.EngineRpm, T.EngineTorqueNm, T.ClutchTorqueNm, T.ClutchSlipRpm, T.ForceLongN[0], T.ForceLongN[1], T.ForceLongN[2], T.ForceLongN[3],
					T.SlipRatio[0], T.SlipRatio[1], Drag, T.LocalAccelMps2.X, T.LocalAccelMps2.X * GetDefault<UCarSettings>()->MassKg, T.AppliedForceN.X, T.Boost));
				(void)DriveForce;
			}
			if (V > MaxValue + 0.05f)
			{
				MaxValue = V;
				MarkPosition = FVector(Since, T.EngagedGear, T.EngineRpm);
			}
			const bool bSurfaceDone = TestSurface.IsSet() && (Value2 >= 0.f || Since > 40.f);
			if (bSurfaceDone || T.PositionM.X > 1850.f || Since > 120.f)
			{
				Report(FString::Printf(TEXT("accel: max %.1f km/h at %.1f s in gear %.0f (%.0f rpm), still accelerating %.2f km/h/s, stalls %d"),
					MaxValue, MarkPosition.X, MarkPosition.Y, MarkPosition.Z, T.LocalAccelMps2.X * 3.6f, T.StallCount));
				Summary.Add(FString::Printf(TEXT("0-50 km/h: %.2f s | 0-100 km/h: %.2f s (target ~8.5-9.5) | 0-160: %.1f s | top speed reached: %.1f km/h (target ~210-216)"),
					Value2, MinValue, T160, MaxValue));
				return true;
			}
			return false;
		});

	// ---------------- 3. 50 km/h in each gear ----------------
	AddPlace(StartX, -60.f, 0.f, true);
	AddStep(TEXT("cruise50"), [this]() { ShiftRpm = 4000.f; LaunchRpm = 2000.f; Counter = 2; Samples = 0; bFlag = false; MarkTime = 0.0; SpeedIntegral = 0.f; Value2 = 0.f; MaxValue = 0.f; MinValue = 0.f; },
		[this](const FCarTelemetry& T)
		{
			SteerToLane(T);
			const int32 Gear = Counter;
			if (!bFlag)
			{
				// Get up to 50 km/h in 2nd.
				DriveFlatOut(T, 2);
				Input.Throttle = FMath::Min(Input.Throttle, 0.6f);
				if (T.SpeedKmh >= 50.f && T.EngagedGear == 2)
				{
					bFlag = true;
					ShiftPhase = 10;
					MarkTime = T.SimTime;
					SpeedIntegral = 0.f;
				}
				return false;
			}
			if (ShiftPhase == 11) // shifting to the next gear
			{
				if (ShiftTo(T, Gear, 0.6f, 0.15f))
				{
					ShiftPhase = 10;
					MarkTime = T.SimTime;
				}
				return false;
			}
			HoldSpeed(T, 50.f);
			const float Held = float(T.SimTime - MarkTime);
			if (Held > 3.f)
			{
				Value2 += T.EngineRpm;
				MaxValue += Input.Throttle;
				MinValue += T.SpeedKmh;
				++Samples;
			}
			if (Held > 6.f)
			{
				const float N = float(FMath::Max(1, Samples));
				Report(FString::Printf(TEXT("cruise: gear %d at %.1f km/h: %.0f rpm, throttle %.2f, stalls %d"), Gear, MinValue / N, Value2 / N, MaxValue / N, T.StallCount));
				Summary.Add(FString::Printf(TEXT("50 km/h in gear %d: %.0f rpm"), Gear, Value2 / N));
				Value2 = MaxValue = MinValue = 0.f;
				Samples = 0;
				if (Gear >= 6)
				{
					return true;
				}
				Counter = Gear + 1;
				ShiftPhase = 11;
				ShiftStart = T.SimTime;
			}
			return false;
		});

	// ---------------- 4. Gentle clutch start with a little throttle ----------------
	AddPlace(StartX, -90.f, 0.f, true);
	AddStep(TEXT("clutch start"), [this]() { MinValue = 1e6f; MaxValue = 0.f; Value2 = -1.f; Counter = 0; },
		[this](const FCarTelemetry& T)
		{
			SteerToLane(T);
			const float S = StepTime(T);
			if (Counter == 0) { Counter = T.StallCount + 1; } // remember stall count + 1
			Input.SelectedGear = 1;
			// Like a driving-school start: some throttle (~1600 rpm, more if it bogs), clutch quickly to the bite
			// point, slowly through it, then fully out.
			Input.Throttle = S < 1.f ? 0.f : FMath::Clamp(0.3f + (1600.f - T.EngineRpm) * 0.0015f, 0.f, 0.8f);
			Input.Clutch = ClutchProfile(S - 1.5f, 0.3f, 1.5f, 0.7f);
			if (S > 1.5f)
			{
				MinValue = FMath::Min(MinValue, T.EngineRpm);
				MaxValue = FMath::Max(MaxValue, T.LocalAccelMps2.X);
			}
			if (Value2 < 0.f && T.SpeedKmh >= 10.f)
			{
				Value2 = S - 1.5f;
			}
			if (S > 8.f)
			{
				const bool bStalled = T.StallCount >= Counter;
				Report(FString::Printf(TEXT("clutch start (~1600 rpm, 2.5 s release): %s, min rpm %.0f, 0-10 km/h %.2f s after release began, peak accel %.2f m/s^2"),
					bStalled ? TEXT("STALLED") : TEXT("ok"), MinValue, Value2, MaxValue));
				Summary.Add(FString::Printf(TEXT("Gentle clutch start: %s (min %.0f rpm, peak %.2f m/s^2)"), bStalled ? TEXT("STALLED") : TEXT("ok"), MinValue, MaxValue));
				return true;
			}
			return false;
		});

	// ---------------- 5. Clutch start without throttle (idle controller only) ----------------
	AddPlace(StartX, -120.f, 0.f, true);
	AddStep(TEXT("idle start"), [this]() { MinValue = 1e6f; Counter = 0; },
		[this](const FCarTelemetry& T)
		{
			SteerToLane(T);
			const float S = StepTime(T);
			if (Counter == 0) { Counter = T.StallCount + 1; }
			Input.SelectedGear = 1;
			Input.Throttle = 0.f;
			Input.Clutch = ClutchProfile(S - 1.f, 0.3f, 3.5f, 1.f);
			if (S > 1.f)
			{
				MinValue = FMath::Min(MinValue, T.EngineRpm);
				if (FMath::FloorToInt(S * 4.f) != FMath::FloorToInt(float(Prev.SimTime - StepStartTime) * 4.f) && S < 7.f)
				{
					Report(FString::Printf(TEXT("  t %.2f clutch %.2f cap %.0f Nm torque %.0f Nm, engine %.0f rpm %.0f Nm, %.2f km/h, applied %.0f N, Fx %.0f/%.0f/%.0f/%.0f, wheel rpm %.1f/%.1f, accel %.2f, gear %d"),
						S, Input.Clutch, T.ClutchCapacityNm, T.ClutchTorqueNm, T.EngineRpm, T.EngineTorqueNm, T.SpeedKmh, T.AppliedForceN.X,
						T.ForceLongN[0], T.ForceLongN[1], T.ForceLongN[2], T.ForceLongN[3], T.WheelRpm[0], T.WheelRpm[2], T.LocalAccelMps2.X, T.EngagedGear));
				}
			}
			if (S > 9.f)
			{
				const bool bStalled = T.StallCount >= Counter;
				Report(FString::Printf(TEXT("idle clutch start (no throttle, 3.5 s through the bite point): %s, min rpm %.0f, speed after 8 s %.1f km/h"),
					bStalled ? TEXT("STALLED") : TEXT("ok"), MinValue, T.SpeedKmh));
				Summary.Add(FString::Printf(TEXT("Slow clutch start without throttle: %s (min %.0f rpm, %.1f km/h)"), bStalled ? TEXT("STALLED") : TEXT("ok"), MinValue, T.SpeedKmh));
				return true;
			}
			return false;
		});

	// ---------------- 6. Deliberate stall: dump the clutch in 1st without throttle ----------------
	AddPlace(StartX, -150.f, 0.f, true);
	AddStep(TEXT("stall"), [this]() { Counter = 0; Value2 = -1.f; },
		[this](const FCarTelemetry& T)
		{
			const float S = StepTime(T);
			if (Counter == 0) { Counter = T.StallCount + 1; }
			Input.SelectedGear = 1;
			Input.Throttle = 0.f;
			Input.Clutch = S < 1.f ? 1.f : FMath::Clamp(1.f - (S - 1.f) / 0.1f, 0.f, 1.f);
			if (Value2 < 0.f && T.StallCount >= Counter)
			{
				Value2 = S - 1.f;
			}
			if (S > 4.f)
			{
				Report(FString::Printf(TEXT("clutch dump at idle in 1st: %s after %.2f s, rpm now %.0f, speed %.1f km/h"),
					Value2 >= 0.f ? TEXT("stalled") : TEXT("DID NOT STALL"), Value2, T.EngineRpm, T.SpeedKmh));
				Summary.Add(FString::Printf(TEXT("Clutch dump at idle: %s"), Value2 >= 0.f ? TEXT("stalls (correct)") : TEXT("does NOT stall")));
				return true;
			}
			return false;
		});
	// Restart with the starter in neutral, clutch pressed.
	AddStep(TEXT("restart"), [this]() { Value2 = -1.f; },
		[this](const FCarTelemetry& T)
		{
			const float S = StepTime(T);
			Input.SelectedGear = 0;
			Input.Clutch = 1.f;
			Input.bStarter = S < 1.5f && !T.bEngineRunning;
			if (Value2 < 0.f && T.bEngineRunning && T.EngineRpm > 700.f)
			{
				Value2 = S;
			}
			if (S > 4.f)
			{
				Report(FString::Printf(TEXT("restart: engine %s, reached 700 rpm after %.2f s, now %.0f rpm"), T.bEngineRunning ? TEXT("running") : TEXT("NOT running"), Value2, T.EngineRpm));
				Input.bStarter = false;
				return true;
			}
			return false;
		});

	// ---------------- Reverse: pull away backwards ----------------
	AddPlace(StartX, -200.f, 0.f, true);
	AddStep(TEXT("reverse"), nullptr, [this](const FCarTelemetry& T)
		{
			const float S = StepTime(T);
			Input.SelectedGear = -1;
			Input.Throttle = S < 1.f ? 0.f : FMath::Clamp(0.3f + (1400.f - T.EngineRpm) * 0.0015f, 0.f, 0.6f);
			Input.Clutch = ClutchProfile(S - 1.f, 0.3f, 1.5f, 0.7f);
			Input.SteeringWheelDeg = 0.f;
			if (S > 6.f)
			{
				Report(FString::Printf(TEXT("reverse: gear %d, %.1f km/h, %.0f rpm, stalls %d"), T.EngagedGear, T.SpeedKmh, T.EngineRpm, T.StallCount));
				Summary.Add(FString::Printf(TEXT("Reverse pull-away: %.1f km/h after 5 s (negative = backwards)"), T.SpeedKmh));
				return true;
			}
			return false;
		});

	// ---------------- Gear change without clutch must grind; with clutch it engages ----------------
	AddPlace(StartX, -230.f, 0.f, true);
	AddStep(TEXT("grind"), [this]() { ShiftRpm = 4000.f; LaunchRpm = 2000.f; Counter = 0; bFlag = false; },
		[this](const FCarTelemetry& T)
		{
			SteerToLane(T);
			if (Counter == 0)
			{
				DriveFlatOut(T, 2);
				Input.Throttle = FMath::Min(Input.Throttle, 0.5f);
				if (T.SpeedKmh > 40.f && ShiftPhase == 3)
				{
					Counter = 1;
					MarkTime = T.SimTime;
				}
				return false;
			}
			const float S = float(T.SimTime - MarkTime);
			Input.Throttle = 0.15f;
			Input.SelectedGear = 4; // 4th at 40 km/h without the clutch: engine at ~2600 rpm, gearbox wants ~1300
			Input.Clutch = S < 1.f ? 0.f : 1.f;
			if (S > 0.5f && S < 0.6f)
			{
				bFlag = T.bGrinding && T.EngagedGear == 0;
			}
			if (S > 1.5f)
			{
				Report(FString::Printf(TEXT("clutchless shift 2->4 at 40 km/h: %s; with clutch pressed: gear %d"), bFlag ? TEXT("grinds (refused)") : TEXT("NOT refused"), T.EngagedGear));
				Summary.Add(FString::Printf(TEXT("Clutchless mismatched shift: %s, with clutch: engages gear %d"), bFlag ? TEXT("refused/grinds") : TEXT("NOT refused"), T.EngagedGear));
				return true;
			}
			return false;
		});

	// ---------------- 7. Braking 100-0 (and 50-0 on a test surface) ----------------
	if (TestSurface.IsSet())
	{
		AddBrakingStep(50.f);
	}
	AddBrakingStep(100.f);

	// ---------------- 8. Ramp steer at 70 km/h: maximum lateral acceleration ----------------
	AddPlace(-1500.f, 600.f, 0.f, true);
	AddStep(TEXT("ramp steer"), [this]() { bFlag = false; MaxValue = 0.f; Value2 = 0.f; SpeedIntegral = 0.f; MinValue = 0.f; },
		[this](const FCarTelemetry& T)
		{
			if (!bFlag)
			{
				SteerToLane(T);
				ShiftRpm = 5000.f;
				DriveFlatOut(T, 3);
				if (T.SpeedKmh >= 70.f && ShiftPhase == 3)
				{
					ApplyTestSurface(true);
					bFlag = true;
					MarkTime = T.SimTime;
				}
				return false;
			}
			const float S = float(T.SimTime - MarkTime);
			HoldSpeed(T, 70.f);
			Input.SteeringWheelDeg = FMath::Min(12.f * S, 360.f);
			const float LatG = FMath::Abs(T.LocalAccelMps2.Y) / 9.81f;
			if (FMath::FloorToInt(S / 2.5f) != FMath::FloorToInt(float(Prev.SimTime - MarkTime) / 2.5f))
			{
				// Steering feel diagnostics: torque at the steering wheel without and with power assistance.
				const UCarSettings* Settings = GetDefault<UCarSettings>();
				const float Assist = FMath::Lerp(Settings->PowerSteeringAssistLowSpeed, Settings->PowerSteeringAssistHighSpeed, FMath::Clamp(T.LocalVelocityMps.X / 27.8f, 0.f, 1.f));
				const float Hand = T.SteeringRackTorqueNm * (1.f - Assist);
				Report(FString::Printf(TEXT("  steer %3.0f deg (road %.1f): lateral %.2f g, slip F %.1f R %.1f deg, rack %.1f Nm -> hands %.2f Nm (FFB %.2f)"),
					Input.SteeringWheelDeg, T.RoadWheelAngleDeg, LatG, T.SlipAngleDeg[0], T.SlipAngleDeg[2], T.SteeringRackTorqueNm, Hand, Hand / Settings->FfbFullScaleNm));
			}
			if (LatG > MaxValue && S > 1.f)
			{
				MaxValue = LatG;
				Value2 = Input.SteeringWheelDeg;
				MinValue = T.SpeedKmh;
			}
			if (S > 30.f)
			{
				Report(FString::Printf(TEXT("ramp steer 70 km/h: max lateral %.2f g at %.0f deg steering wheel, %.1f km/h; end speed %.1f km/h, slip angles F %.1f R %.1f deg"),
					MaxValue, Value2, MinValue, T.SpeedKmh, T.SlipAngleDeg[0], T.SlipAngleDeg[2]));
				Summary.Add(FString::Printf(TEXT("Max lateral acceleration (ramp steer): %.2f g (target ~0.9-1.0)"), MaxValue));
				return true;
			}
			return false;
		});
}

void ADriveTestRunner::ApplyTestSurface(bool bOn)
{
	ACarPawn* Pawn = Car.Get();
	UCarMovementComponent* Movement = Pawn ? Pawn->GetCarMovement() : nullptr;
	if (!Movement || !TestSurface.IsSet())
	{
		return;
	}
	Movement->SetSurfaceConditionsOverride(bOn ? TestSurface.GetValue() : FCarSurfaceConditions());
}

void ADriveTestRunner::KeepSurfaceSteps()
{
	const TSet<FString> Kept = {TEXT("acceleration"), TEXT("braking"), TEXT("ramp steer")};
	TArray<FStep> Filtered;
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		const bool bKeep = Kept.Contains(Steps[Index].Name);
		const bool bPlaceBeforeKept = Steps[Index].Name == TEXT("place") && Steps.IsValidIndex(Index + 1) && Kept.Contains(Steps[Index + 1].Name);
		if (bKeep || bPlaceBeforeKept)
		{
			Filtered.Add(Steps[Index]);
		}
	}
	Steps = MoveTemp(Filtered);
	Report(FString::Printf(TEXT("surface test: %s (wetness %.2f, snow %.2f, %.0f C)"), *TestSurfaceName, TestSurface->Wetness, TestSurface->SnowCover, TestSurface->TemperatureCelsius));
}

void ADriveTestRunner::AddBrakingStep(float FromKmh)
{
	AddPlace(-1900.f, -180.f, 0.f, true);
	AddStep(TEXT("braking"), [this]() { bFlag = false; Counter = 0; Samples = 0; MaxValue = 0.f; Value2 = 0.f; },
		[this, FromKmh](const FCarTelemetry& T)
		{
			SteerToLane(T);
			if (Counter == 0)
			{
				DriveFlatOut(T, 4);
				if (T.SpeedKmh >= FromKmh + 12.f)
				{
					Counter = 1; // coast with clutch pressed
					MarkTime = T.SimTime;
				}
				return false;
			}
			if (Counter == 1 && T.SimTime - MarkTime > 1.5 && !bFlag)
			{
				bFlag = true; // once: coast-down diagnostics (clutch pressed: only drag + rolling resistance)
				const float Vms = T.LocalVelocityMps.X;
				Report(FString::Printf(TEXT("  coast %.0f km/h: decel %.3f m/s^2 (expected drag+rolling %.3f), tyre Fx %.0f/%.0f/%.0f/%.0f N, Fy %.0f/%.0f/%.0f/%.0f N, loads %.0f/%.0f/%.0f/%.0f"),
					T.SpeedKmh, -T.LocalAccelMps2.X, (0.5f * 1.2f * 0.65f * Vms * Vms + 0.011f * 1300.f * 9.81f) / 1300.f,
					T.ForceLongN[0], T.ForceLongN[1], T.ForceLongN[2], T.ForceLongN[3], T.ForceLatN[0], T.ForceLatN[1], T.ForceLatN[2], T.ForceLatN[3],
					T.LoadN[0], T.LoadN[1], T.LoadN[2], T.LoadN[3]));
			}
			Input.Throttle = 0.f;
			Input.Clutch = 1.f;
			if (Counter == 1 && Prev.SpeedKmh > FromKmh && T.SpeedKmh <= FromKmh)
			{
				Counter = 2;
				ApplyTestSurface(true);
				const float F = CrossFraction(Prev.SpeedKmh, T.SpeedKmh, FromKmh);
				MarkPosition = FMath::Lerp(Prev.PositionM, T.PositionM, F);
				MarkTime = FMath::Lerp(Prev.SimTime, T.SimTime, double(F));
			}
			if (Counter == 2)
			{
				Input.Brake = 1.f;
				Samples |= T.bAbsActive ? 1 : 0;
				MaxValue = FMath::Max(MaxValue, -T.LocalAccelMps2.X);
				if (T.SpeedKmh < 0.5f)
				{
					const float F = CrossFraction(Prev.SpeedKmh, T.SpeedKmh, 0.f);
					const FVector StopPosition = FMath::Lerp(Prev.PositionM, T.PositionM, F);
					const float Distance = FVector::Dist2D(StopPosition, MarkPosition);
					const float Time = float(FMath::Lerp(Prev.SimTime, T.SimTime, double(F)) - MarkTime);
					Report(FString::Printf(TEXT("braking %.0f-0: %.1f m in %.2f s (mean %.2f m/s^2 = %.2f g, peak %.2f m/s^2), ABS %s"),
						FromKmh, Distance, Time, FromKmh / 3.6f / Time, FromKmh / 3.6f / Time / 9.81f, MaxValue, Samples ? TEXT("active") : TEXT("not triggered")));
					Summary.Add(FString::Printf(TEXT("Braking %.0f-0: %.1f m%s"), FromKmh, Distance, FromKmh > 99.f ? TEXT(" (dry target ~36-40 m)") : TEXT("")));
					Input.Brake = 0.5f;
					return true;
				}
			}
			return false;
		});
}

void ADriveTestRunner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!Car.IsValid())
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		Car = PC ? Cast<ACarPawn>(PC->GetPawn()) : nullptr;
		if (!Car.IsValid())
		{
			return;
		}
		Car->SetAutopilot(true);
	}

	const FCarTelemetry T = Car->GetTelemetry();
	if (QuitAt > 0.0)
	{
		if (T.SimTime > QuitAt)
		{
			GEngine->Exec(GetWorld(), TEXT("quit"));
			QuitAt = 1e30;
		}
		return;
	}
	if (T.SimTime <= Prev.SimTime && StepIndex >= 0)
	{
		return; // no new physics step yet
	}

	if (StepIndex < 0 || (bStepStarted && Steps[StepIndex].Update(T)))
	{
		++StepIndex;
		bStepStarted = false;
		if (!Steps.IsValidIndex(StepIndex))
		{
			Report(TEXT("===== SUMMARY ====="));
			for (const FString& Line : Summary)
			{
				Report(Line);
			}
			Report(TEXT("===== END ====="));
			QuitAt = T.SimTime + 0.5;
			Car->SetAutopilotInput(FCarDriverInput());
			return;
		}
	}
	if (!bStepStarted)
	{
		bStepStarted = true;
		StepStartTime = T.SimTime;
		if (Steps[StepIndex].Start)
		{
			Steps[StepIndex].Start();
		}
		Report(FString::Printf(TEXT("--- %s"), *Steps[StepIndex].Name));
	}
	Car->SetAutopilotInput(Input);
	Prev = T;
}
