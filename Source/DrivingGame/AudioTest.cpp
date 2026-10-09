#include "AudioTest.h"

#include "CarAudioComponent.h"
#include "CarPawn.h"

void AAudioTestRunner::SetLabel(const FString& Label)
{
	if (UCarAudioComponent* Audio = GetAudio())
	{
		Audio->SetCaptureLabel(Label);
	}
}

UCarAudioComponent* AAudioTestRunner::GetAudio() const
{
	return Car.IsValid() ? Car->GetCarAudio() : nullptr;
}

void AAudioTestRunner::AddScene(const FString& Label, float Seconds, TFunction<void()> Setup, TFunction<void(const FCarTelemetry&)> Drive)
{
	AddStep(Label, [this, Label, Setup]()
	{
		SetLabel(Label);
		if (Setup)
		{
			Setup();
		}
	}, [this, Seconds, Drive](const FCarTelemetry& T)
	{
		if (Drive)
		{
			Drive(T);
		}
		return StepTime(T) > Seconds;
	});
}

void AAudioTestRunner::CruiseTo(const FCarTelemetry& T, float TargetKmh)
{
	SteerToLane(T);
	if (Counter == 0)
	{
		DriveFlatOut(T, 2);
		if (T.SpeedKmh > TargetKmh * 0.75f && ShiftPhase == 3)
		{
			Counter = 1;
			ShiftStart = T.SimTime;
		}
	}
	else if (Counter == 1)
	{
		if (ShiftTo(T, 3, 0.4f, 0.3f))
		{
			Counter = 2;
			SpeedIntegral = 0.f;
		}
	}
	else
	{
		HoldSpeed(T, TargetKmh);
	}
}

void AAudioTestRunner::AddCruiseScenes(float StartX)
{
	AddPlace(StartX, -30.f, 0.f, true, 2.f);
	AddScene(TEXT("cruise_asphalt"), 14.f, [this]() { Counter = 0; ShiftPhase = 0; }, [this](const FCarTelemetry& T) { CruiseTo(T, 50.f); });
	AddScene(TEXT("cruise_cobble"), 9.f, [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->SetForcedSurface(ECarRoadSurface::Cobble, 0.f); }
	}, [this](const FCarTelemetry& T) { CruiseTo(T, 50.f); });
	AddScene(TEXT("cruise_pavers"), 7.f, [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->SetForcedSurface(ECarRoadSurface::Pavers, 0.f); }
	}, [this](const FCarTelemetry& T) { CruiseTo(T, 50.f); });
	AddScene(TEXT("cruise_wet_asphalt"), 7.f, [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->SetForcedSurface(ECarRoadSurface::Asphalt, 1.f); }
	}, [this](const FCarTelemetry& T) { CruiseTo(T, 50.f); });
	AddScene(TEXT("cruise_dry_fast"), 8.f, [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->SetForcedSurface(ECarRoadSurface::Asphalt, 0.f); }
	}, [this](const FCarTelemetry& T) { CruiseTo(T, 90.f); });
	// Cornering beyond the limit: the steering wheel is turned up to 330 degrees over four seconds.
	AddScene(TEXT("cornering_squeal"), 6.f, [this]() { SpeedIntegral = 0.f; }, [this](const FCarTelemetry& T)
	{
		HoldSpeed(T, 55.f);
		Input.SteeringWheelDeg = FMath::Clamp(float(StepTime(T)) / 4.f, 0.f, 1.f) * 330.f;
	});
}

void AAudioTestRunner::AddMechanicalScenes(float StartX)
{
	AddPlace(StartX, -30.f, 0.f, true, 2.f);
	AddScene(TEXT("gear_clunks"), 4.f, nullptr, [this](const FCarTelemetry& T)
	{
		const float S = StepTime(T);
		Input.Clutch = 1.f;
		Input.SelectedGear = S < 0.6f ? 0 : S < 1.4f ? 1 : S < 2.2f ? 2 : 0;
	});
	AddScene(TEXT("handbrake_indicator"), 6.f, [this]()
	{
		Input = FCarDriverInput();
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->SetIndicatorActive(true); }
	}, [this](const FCarTelemetry& T)
	{
		const float S = StepTime(T);
		Input.bHandbrake = S > 0.5f && S < 2.5f;
		if (S > 5.f)
		{
			if (UCarAudioComponent* Audio = GetAudio()) { Audio->SetIndicatorActive(false); }
		}
	});
}

void AAudioTestRunner::BuildSteps()
{
	const float StartX = -1900.f;

	// Engine off, then the starter.
	AddPlace(StartX, -30.f, 0.f, false, 1.f);
	AddScene(TEXT("engine_off"), 1.5f, nullptr, nullptr);
	AddScene(TEXT("starter"), 6.f, nullptr, [this](const FCarTelemetry& T)
	{
		Input.bStarter = StepTime(T) > 0.3f && !T.bEngineRunning;
	});
	AddScene(TEXT("idle"), 6.f, [this]() { Input.bStarter = false; }, nullptr);
	AddScene(TEXT("rev_blips"), 6.f, nullptr, [this](const FCarTelemetry& T)
	{
		const float S = StepTime(T);
		Input.Throttle = S < 0.8f ? 0.9f : S < 2.4f ? 0.f : S < 3.2f ? 0.45f : S < 3.4f ? 0.f : S < 4.2f ? 1.f : 0.f;
	});

	// Full throttle through the gears, then the rev limiter in first gear.
	AddPlace(StartX, -30.f, 0.f, true, 2.f);
	AddScene(TEXT("wot_gears"), 26.f, [this]() { ShiftPhase = 0; }, [this](const FCarTelemetry& T)
	{
		SteerToLane(T);
		DriveFlatOut(T, 6);
	});
	AddScene(TEXT("coast_down"), 9.f, [this]() { Input.Throttle = 0.f; }, [this](const FCarTelemetry& T)
	{
		SteerToLane(T);
		Input.Throttle = 0.f;
		Input.Clutch = 0.f;
	});
	AddScene(TEXT("brake_to_stall"), 9.f, nullptr, [this](const FCarTelemetry& T)
	{
		SteerToLane(T);
		Input.Brake = 0.9f;
		Input.Clutch = 0.f;
	});
	AddScene(TEXT("after_stall"), 2.f, [this]() { Input = FCarDriverInput(); }, nullptr);
	AddPlace(StartX, -30.f, 0.f, true, 2.f);
	AddScene(TEXT("rev_limiter"), 7.f, [this]() { ShiftPhase = 0; }, [this](const FCarTelemetry& T)
	{
		SteerToLane(T);
		DriveFlatOut(T, 1);
	});

	AddCruiseScenes(StartX);
	AddMechanicalScenes(StartX);
}
