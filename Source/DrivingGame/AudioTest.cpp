#include "AudioTest.h"

#include "CarAudioComponent.h"
#include "CarPawn.h"
#include "CarRadioComponent.h"
#include "CarSettings.h"
#include "Components/SkeletalMeshComponent.h"
#include "WorldSurfaceQuery.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

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
	// Kerb and pothole hits: the car is thrown up 25 cm three times and lands on the suspension.
	AddScene(TEXT("cruise_jolts"), 7.f, [this]() { SpeedIntegral = 0.f; }, [this](const FCarTelemetry& T)
	{
		HoldSpeed(T, 40.f);
		SteerToLane(T);
		const float S = StepTime(T);
		const bool bJolt = (S > 1.5f && S < 1.52f) || (S > 3.f && S < 3.02f) || (S > 4.5f && S < 4.52f);
		if (bJolt && Car.IsValid())
		{
			Car->GetMesh()->AddImpulse(FVector(0.f, 0.f, GetDefault<UCarSettings>()->MassKg * 220.f));
		}
	});
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

void AAudioTestRunner::ProbeSurfaceAt(const FVector2D& PointMeters)
{
	ACarPawn* Pawn = Car.Get();
	if (!Pawn)
	{
		return;
	}
	// Hold the car high above the point so the streamer builds the ground there without the car falling through it.
	Pawn->SetActorLocationAndRotation(FVector(PointMeters.X * 100.0, PointMeters.Y * 100.0, 4000.0), FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
	Pawn->GetMesh()->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
	Pawn->GetMesh()->SetAllPhysicsAngularVelocityInRadians(FVector::ZeroVector);
}

void AAudioTestRunner::AddSurfaceProbes(const FString& Points)
{
	TArray<FString> Entries;
	Points.ParseIntoArray(Entries, TEXT(";"));
	for (const FString& Entry : Entries)
	{
		TArray<FString> Parts;
		Entry.ParseIntoArray(Parts, TEXT(","));
		if (Parts.Num() < 2)
		{
			continue;
		}
		const FVector2D Point(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]));
		AddScene(TEXT("probe"), 8.f, nullptr, [this, Point](const FCarTelemetry& T)
		{
			ProbeSurfaceAt(Point);
			if (StepTime(T) < 7.f || Counter == 1)
			{
				return;
			}
			Counter = 1;
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(AudioProbe), /*bTraceComplex=*/true, Car.Get());
			Params.bReturnFaceIndex = true;
			const FVector Start(Point.X * 100.0, Point.Y * 100.0, 3000.0);
			const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0, 0, 6000), ECC_Visibility, Params);
			Report(FString::Printf(TEXT("SURFACEPROBE at %.0f, %.0f m: %s, ground z %.2f m, material %s"), Point.X, Point.Y,
				bHit ? *GetNameSafe(Hit.Component.Get()) : TEXT("no ground"), Hit.ImpactPoint.Z / 100.0, *FindWorldSurfaceName(Hit).ToString()));
		});
		AddStep(TEXT("probe_reset"), [this]() { Counter = 0; }, [](const FCarTelemetry&) { return true; });
	}
}

void AAudioTestRunner::AddWeatherScene(const FString& Label, float Seconds, float RainMmPerHour, float WindMps, float ThunderActivity, float SunAltitude,
	TFunction<void(float)> Extra)
{
	AddScene(Label, Seconds, [this, RainMmPerHour, WindMps, ThunderActivity, SunAltitude]()
	{
		Counter = 0;
		if (UCarAudioComponent* Audio = GetAudio())
		{
			Audio->SetForcedWeather(RainMmPerHour, WindMps, WindMps * 1.6f, ThunderActivity, SunAltitude);
		}
	}, [this, Extra](const FCarTelemetry& T)
	{
		if (Extra)
		{
			Extra(StepTime(T));
		}
	});
}

void AAudioTestRunner::AddAmbienceScenes(float StartX)
{
	AddPlace(StartX, -30.f, 0.f, true, 2.f);
	AddWeatherScene(TEXT("day_dry_calm"), 14.f, 0.f, 2.f, 0.f, 40.f, nullptr);
	AddWeatherScene(TEXT("day_windy"), 9.f, 0.f, 11.f, 0.f, 40.f, nullptr);
	AddWeatherScene(TEXT("night_calm"), 9.f, 0.f, 2.f, 0.f, -25.f, nullptr);
	AddWeatherScene(TEXT("drizzle"), 12.f, 0.8f, 3.f, 0.f, 10.f, nullptr);
	AddWeatherScene(TEXT("heavy_rain"), 12.f, 10.f, 7.f, 0.f, 10.f, nullptr);
	// Strikes at 600 m, 2.5 km and 6 km: the thunder arrives after 1.7 s, 7.3 s and 17.5 s.
	AddWeatherScene(TEXT("thunderstorm"), 28.f, 8.f, 8.f, 0.8f, 5.f, [this](float S)
	{
		UCarAudioComponent* Audio = GetAudio();
		if (!Audio)
		{
			return;
		}
		if (Counter == 0 && S > 1.f) { Counter = 1; Audio->TriggerThunder(600.f); }
		if (Counter == 1 && S > 4.f) { Counter = 2; Audio->TriggerThunder(2500.f); }
		if (Counter == 2 && S > 8.f) { Counter = 3; Audio->TriggerThunder(6000.f); }
	});
	AddStep(TEXT("finish_recording"), [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->FinishMixerRecording(); }
	}, [](const FCarTelemetry&) { return true; });
}

void AAudioTestRunner::AddRadioScenes(float StartX)
{
	const auto RadioAction = [this](TFunction<void(UCarRadioComponent&)> Action)
	{
		return [this, Action]()
		{
			if (UCarRadioComponent* Radio = Car.IsValid() ? Car->GetCarRadio() : nullptr)
			{
				Action(*Radio);
			}
		};
	};
	AddPlace(StartX, -30.f, 0.f, true, 2.f);
	AddScene(TEXT("radio_first_station"), 16.f, nullptr, nullptr);
	AddScene(TEXT("radio_next_station"), 16.f, RadioAction([](UCarRadioComponent& Radio) { Radio.NextStation(); }), nullptr);
	AddScene(TEXT("radio_off"), 4.f, RadioAction([](UCarRadioComponent& Radio) { Radio.ToggleRadio(); }), nullptr);
	AddScene(TEXT("radio_on_again"), 12.f, RadioAction([](UCarRadioComponent& Radio) { Radio.ToggleRadio(); }), nullptr);
	AddStep(TEXT("finish_recording"), [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->FinishMixerRecording(); }
	}, [](const FCarTelemetry&) { return true; });
}

void AAudioTestRunner::BuildSteps()
{
	// -AudioProbe="x,y;x,y": only park the car at these points (metres) and let the audio component log the surface it finds.
	FString Probes;
	if (FParse::Value(FCommandLine::Get(), TEXT("AudioProbe="), Probes, false))
	{
		AddSurfaceProbes(Probes);
		return;
	}
	const float StartX = -1900.f;
	if (FParse::Param(FCommandLine::Get(), TEXT("RadioTest")))
	{
		AddRadioScenes(StartX);
		return;
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("AmbienceTest")))
	{
		AddAmbienceScenes(StartX);
		return;
	}

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
	AddStep(TEXT("finish_recording"), [this]()
	{
		if (UCarAudioComponent* Audio = GetAudio()) { Audio->FinishMixerRecording(); }
	}, [](const FCarTelemetry&) { return true; });
}
