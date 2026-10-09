#include "WeatherVisuals.h"

#include "CloudSky.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "IsobarSunGeometry.h"
#include "IsobarTime.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RainEffect.h"
#include "WeatherSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeatherVisuals, Log, All);

namespace WeatherVisualsDetail
{
	/** Sky luminance factor under a traced cloud deck, to match the light of the plain-atmosphere overcast sky. */
	constexpr float TracedDeckSkyGain = 1.2f;

	/** Latitude of the map origin, as in WeatherSubsystem.cpp. */
	constexpr double MapLatitudeDegrees = 53.4880;

	constexpr float SampleIntervalSeconds = 0.5f;
	/** Time constant for following a weather change; the weather itself steps every ten minutes. */
	constexpr float SmoothingSeconds = 20.f;

	/** Rain of 1 mm/h soaks a road in about five minutes; it takes half an hour to dry at 20 degrees. */
	constexpr float SoakSecondsPerMillimetreHour = 300.f;
	constexpr float DrySeconds = 1800.f;
	/** Snow of 1 mm/h water equivalent covers the ground in about twenty minutes. */
	constexpr float SnowCoverSeconds = 1200.f;
	constexpr float MeltSeconds = 3600.f;

	/** Koschmieder: visibility is where contrast drops to 2 %, at an extinction of 3.912 over that distance. */
	constexpr float KoschmiederConstant = 3.912f;
	/** Unreal's FogDensity is extinction per metre times ten (it is divided by 1000 and applied per centimetre). */
	constexpr float FogDensityPerExtinction = 10.f;
	/** Isobar's fog density 1 means about 120 m visibility. */
	constexpr float DenseFogVisibilityMeters = 120.f;
	/** Radiation fog halves in density every 20 m of height (Unreal's falloff is per 1000 cm). */
	constexpr float RadiationFogHeightFalloff = 0.5f;

	/** Exposure the clear-day lighting was calibrated at (Scripts/world_lighting.py), and the sun it saw. */
	constexpr float CalibratedEV100 = 12.8f;
	constexpr float CalibratedSunAltitudeDegrees = 38.f;
	/** Exposure never opens further than this at night, so dark stays dark. */
	constexpr float NightEV100 = 1.5f;
	/**
	 * Sky glow over a town at night: the cloud deck and the haze lit orange by street lamps from below, about
	 * 0.1 cd/m2 under clear skies and several times that under low cloud.
	 */
	const FLinearColor SkyGlowLuminance(0.5f, 0.31f, 0.18f);
	constexpr float OvercastSkyGlowGain = 3.f;

	/** Direct sun left under a closed deck, and the diffuse light such a day has relative to a clear one. */
	constexpr float OvercastSunTransmission = 0.02f;
	constexpr float OvercastDiffuseShare = 0.4f;
	/**
	 * The sun is set to the illuminance calibrated on the ground (75 klx), well below the 128 klx above the
	 * atmosphere that lights a real sky, so the clear sky came out about two stops darker than sunlit asphalt.
	 */
	constexpr float ClearSkyGain = 1.8f;
	/**
	 * Exposure under a closed cloud deck. A thick stratus deck passes only 10 to 25 % of clear-sky illuminance, but
	 * a camera on auto exposure and the eye both adapt almost completely to it (a clear day is EV100 15 by the
	 * sunny-16 rule, a thick overcast about EV100 12), so the adaptation goes from 0.75 on clear days to 1.0.
	 * On top, auto exposure meters for a mid grey while an overcast scene is mostly pale cloud, snow and light
	 * walls: it ends up about one and a half stops brighter than the nominal metering, which is the compensation photographers
	 * dial in for snow. Clear days, twilight and night are not touched (both are weighted by Overcast * Daylight).
	 */
	constexpr float ClearExposureAdaptation = 0.75f;
	constexpr float OvercastExposureAdaptation = 1.0f;
	constexpr float OvercastExposureCompensationEV = 1.5f;

	const TCHAR* RainMaterialPath = TEXT("/Game/World/Materials/M_RainStreaks.M_RainStreaks");
	const TCHAR* LeavesMaterialPath = TEXT("/Game/World/Materials/M_FallingLeaves.M_FallingLeaves");

	/** The deciduous year in Hamburg, days of the year counted from 0 on the first of January. */
	constexpr float LeafOutStart = 99.f;     // 10 April
	constexpr float LeafOutEnd = 125.f;      // 6 May
	constexpr float ColourStart = 273.f;     // 1 October
	constexpr float ColourEnd = 303.f;       // 31 October
	constexpr float LeafFallStart = 292.f;   // 20 October
	constexpr float LeafFallEnd = 328.f;     // 25 November
	constexpr float LeavesClearedBy = 350.f; // mid December: swept, blown away and rotted

	/** Season state for materials: foliage density and colour, leaves on the ground and in the air. */
	struct FSeasonState
	{
		float LeafDensity = 1.f;
		float LeafColour = 0.f;
		float FallenLeaves = 0.f;
		float LeafFall = 0.f;
	};

	FSeasonState SeasonAt(float DayOfYear, float WindSpeed)
	{
		FSeasonState State;
		const float Growing = FMath::SmoothStep(LeafOutStart, LeafOutEnd, DayOfYear);
		const float Dropped = FMath::SmoothStep(LeafFallStart, LeafFallEnd, DayOfYear);
		State.LeafDensity = Growing * (1.f - Dropped);
		State.LeafColour = DayOfYear < LeafOutEnd ? 0.f : FMath::SmoothStep(ColourStart, ColourEnd, DayOfYear);
		State.FallenLeaves = Dropped * (1.f - FMath::SmoothStep(LeafFallEnd, LeavesClearedBy, DayOfYear));
		// Most leaves come down in the middle of the fall period, and more in a stiff breeze.
		const float Middle = (LeafFallStart + LeafFallEnd) * 0.5f;
		const float HalfWidth = (LeafFallEnd - LeafFallStart) * 0.5f;
		const float Peak = FMath::Max(0.f, 1.f - FMath::Square((DayOfYear - Middle) / HalfWidth));
		State.LeafFall = Peak * FMath::Lerp(0.3f, 1.f, FMath::Clamp(WindSpeed / 8.f, 0.f, 1.f));
		return State;
	}

	/** At full thunder activity a strike every eight seconds on average; strikes land 1 to 10 km away. */
	constexpr float StrikesPerSecondAtFullActivity = 0.12f;
	constexpr float NearestStrikeMeters = 1000.f;
	constexpr float FarthestStrikeMeters = 10000.f;
	/** Illuminance of a close flash on the ground, lux: about as bright as an overcast day for an instant. */
	constexpr float FlashLux = 15000.f;

	const TCHAR* ParameterCollectionPath = TEXT("/Game/World/MPC_Weather.MPC_Weather");

	/** Smooth 0..1 step between Edge0 and Edge1. */
	float SmoothStep01(float Edge0, float Edge1, float Value)
	{
		return FMath::SmoothStep(Edge0, Edge1, Value);
	}

	float ExtinctionForVisibility(float VisibilityMeters)
	{
		return KoschmiederConstant / FMath::Max(VisibilityMeters, 30.f);
	}

	/** Isobar wind (X east, Y north) in Unreal's frame (X east, Y south). */
	FVector2f ToUnrealWind(const FVector2D& IsobarWind)
	{
		return FVector2f(float(IsobarWind.X), float(0.0 - IsobarWind.Y));
	}

	/**
	 * Illuminance on level ground relative to the calibration sun, for exposure: direct sun through the clouds plus
	 * diffuse skylight, which overcast cloud brightens relative to the direct beam but which falls with the sun.
	 */
	float RelativeGroundIlluminance(float SunAltitudeDegrees, float SunTransmission, float CloudCover)
	{
		const auto Illuminance = [](float AltitudeDegrees, float Transmission, float Cover)
		{
			const float SinAltitude = FMath::Max(FMath::Sin(FMath::DegreesToRadians(AltitudeDegrees)), 0.f);
			const float Direct = SinAltitude * Transmission;
			const float Diffuse = FMath::Lerp(0.18f, 0.32f, Cover) * FMath::Pow(SinAltitude, 0.8f);
			// Twilight: about 1 % of daylight at sunset, falling tenfold every 2.3 degrees the sun sinks below.
			const float Twilight = 0.01f * FMath::Pow(10.f, FMath::Min(AltitudeDegrees, 0.f) / 2.3f);
			return Direct + Diffuse + Twilight;
		};
		const float Calibration = Illuminance(CalibratedSunAltitudeDegrees, 1.f, 0.f);
		return Illuminance(SunAltitudeDegrees, SunTransmission, CloudCover) / Calibration;
	}

	/** Parses "Name=Value,Name=Value" overrides. */
	TMap<FString, float> ParseOverrides(const FString& Text)
	{
		TMap<FString, float> Result;
		TArray<FString> Pairs;
		Text.ParseIntoArray(Pairs, TEXT(","));
		for (const FString& Pair : Pairs)
		{
			FString Name, Value;
			if (Pair.Split(TEXT("="), &Name, &Value))
			{
				Result.Add(Name.TrimStartAndEnd(), FCString::Atof(*Value));
			}
		}
		return Result;
	}

	/** The -WeatherOverride="Name=Value,..." of the command line, empty without one. */
	FString CommandLineOverrides()
	{
		FString Text;
		FParse::Value(FCommandLine::Get(), TEXT("WeatherOverride="), Text, /*bShouldStopOnSeparator=*/false);
		return Text;
	}

	/** `Weather.Override Name=Value,...`: replaces the overrides (none clears them) and shows the result at once. */
	void OverrideWeather(const TArray<FString>& Args, UWorld* World, FOutputDevice& Output)
	{
		UWeatherVisualsSubsystem* Visuals = World ? World->GetSubsystem<UWeatherVisualsSubsystem>() : nullptr;
		if (!Visuals)
		{
			Output.Log(TEXT("No weather visuals in this world."));
			return;
		}
		Visuals->SetOverrides(FString::Join(Args, TEXT(",")));
	}

	FAutoConsoleCommandWithWorldArgsAndOutputDevice OverrideCommand(
		TEXT("Weather.Override"), TEXT("Weather.Override CloudCover=1,Rain=8,...: replaces the weather overrides (as -WeatherOverride); no arguments clears them."),
		FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(&OverrideWeather));
}

void UWeatherVisualsSubsystem::SetOverrides(const FString& Spec)
{
	Overrides = WeatherVisualsDetail::ParseOverrides(Spec);
	Snap();
}

void UWeatherVisualsSubsystem::Snap()
{
	bHasState = false;
	SecondsUntilSample = 0.f;
}

bool UWeatherVisualsSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && Super::ShouldCreateSubsystem(Outer);
}

void UWeatherVisualsSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	FindLevelActors();
	Overrides = WeatherVisualsDetail::ParseOverrides(WeatherVisualsDetail::CommandLineOverrides());
	Parameters = LoadObject<UMaterialParameterCollection>(nullptr, WeatherVisualsDetail::ParameterCollectionPath, nullptr, LOAD_NoWarn);
	if (!Parameters)
	{
		UE_LOG(LogWeatherVisuals, Warning, TEXT("%s missing; run Scripts/create_weather_parameters.py"), WeatherVisualsDetail::ParameterCollectionPath);
	}
	SpawnEffects();
	CloudSky = NewObject<UCloudSkyRig>(this);
	CloudSky->Initialise(&InWorld);
	UE_LOG(LogWeatherVisuals, Log, TEXT("Weather visuals: sun %s, atmosphere %s, fog %s, post process %s, %d overrides"),
		Sun.IsValid() ? TEXT("yes") : TEXT("no"), Atmosphere.IsValid() ? TEXT("yes") : TEXT("no"),
		Fog.IsValid() ? TEXT("yes") : TEXT("no"), PostProcess.IsValid() ? TEXT("yes") : TEXT("no"), Overrides.Num());
}

void UWeatherVisualsSubsystem::SpawnEffects()
{
	UWorld* World = GetWorld();
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, WeatherVisualsDetail::RainMaterialPath, nullptr, LOAD_NoWarn))
	{
		ARainEffect* Effect = World->SpawnActor<ARainEffect>(SpawnParameters);
		Effect->Initialise(Material);
		Effect->SetActorHiddenInGame(true);
		Rain = Effect;
	}
	if (UMaterialInterface* LeafMaterial = LoadObject<UMaterialInterface>(nullptr, WeatherVisualsDetail::LeavesMaterialPath, nullptr, LOAD_NoWarn))
	{
		ARainEffect* Effect = World->SpawnActor<ARainEffect>(SpawnParameters);
		Effect->Initialise(LeafMaterial, 2500);
		Effect->SetActorHiddenInGame(true);
		Leaves = Effect;
	}
	else
	{
		UE_LOG(LogWeatherVisuals, Warning, TEXT("%s missing; run Scripts/create_rain_material.py"), WeatherVisualsDetail::RainMaterialPath);
	}
	ADirectionalLight* Flash = World->SpawnActor<ADirectionalLight>(SpawnParameters);
	UDirectionalLightComponent* Component = Cast<UDirectionalLightComponent>(Flash->GetLightComponent());
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetIntensity(0.f);
	Component->SetCastShadows(false);
	Component->SetAtmosphereSunLight(false);
	Component->SetLightColor(FLinearColor(0.85f, 0.9f, 1.f));
	FlashLight = Flash;
}

float UWeatherVisualsSubsystem::FlashLevel() const
{
	// Return strokes of one flash: a bright first stroke, then one or two weaker ones.
	const float Time = float(SecondsSinceStrike);
	const float Pulses[3][2] = {{0.f, 1.f}, {0.09f, 0.6f}, {0.21f, 0.8f}};
	float Level = 0.f;
	for (const auto& Pulse : Pulses)
	{
		const float Age = Time - Pulse[0];
		if (Age >= 0.f)
		{
			Level = FMath::Max(Level, Pulse[1] * FMath::Exp(-Age / 0.035f));
		}
	}
	return Level * StrikeStrength;
}

void UWeatherVisualsSubsystem::UpdateLightning(float DeltaTime)
{
	SecondsSinceStrike += DeltaTime;
	const float Rate = Current.ThunderActivity * WeatherVisualsDetail::StrikesPerSecondAtFullActivity;
	if (Rate > 0.f && LightningRandom.FRand() < Rate * DeltaTime)
	{
		FVector Viewer;
		const UWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UWeatherSubsystem>();
		if (Weather && Weather->FindViewerLocation(Viewer))
		{
			// Closer strikes as the storm gets more active; strength falls with distance.
			const float Closeness = LightningRandom.FRand() * Current.ThunderActivity;
			const float DistanceMeters = FMath::Lerp(WeatherVisualsDetail::FarthestStrikeMeters, WeatherVisualsDetail::NearestStrikeMeters, Closeness);
			const float Bearing = LightningRandom.FRandRange(0.f, 2.f * PI);
			const FVector Strike = Viewer + FVector(FMath::Cos(Bearing), FMath::Sin(Bearing), 0.f) * DistanceMeters * 100.f;
			StrikeStrength = FMath::Clamp(WeatherVisualsDetail::NearestStrikeMeters * 2.f / DistanceMeters, 0.15f, 1.f);
			SecondsSinceStrike = 0.0;
			if (ADirectionalLight* Flash = FlashLight.Get())
			{
				// Light from the strike's direction, from above the cloud base.
				const FVector FromStrike = (Viewer - Strike).GetSafeNormal2D() + FVector(0.f, 0.f, -0.6f);
				Flash->SetActorRotation(FromStrike.Rotation());
			}
			UE_LOG(LogWeatherVisuals, Log, TEXT("Lightning %.0f m away, strength %.2f"), DistanceMeters, StrikeStrength);
			OnLightning.Broadcast(Strike, StrikeStrength);
		}
	}
	if (ADirectionalLight* Flash = FlashLight.Get())
	{
		Cast<UDirectionalLightComponent>(Flash->GetLightComponent())->SetIntensity(WeatherVisualsDetail::FlashLux * FlashLevel());
	}
}

void UWeatherVisualsSubsystem::FindLevelActors()
{
	UWorld* World = GetWorld();
	TActorIterator<ADirectionalLight> SunIt(World);
	if (SunIt)
	{
		Sun = *SunIt;
		BaseSunLux = SunIt->GetComponent()->Intensity;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (USkyAtmosphereComponent* Component = It->FindComponentByClass<USkyAtmosphereComponent>())
		{
			Atmosphere = Component;
			break;
		}
	}
	TActorIterator<AExponentialHeightFog> FogIt(World);
	if (FogIt)
	{
		Fog = FogIt->GetComponent();
		BaseFogInscattering = FogIt->GetComponent()->FogInscatteringLuminance;
	}
	for (TActorIterator<APostProcessVolume> It(World); It; ++It)
	{
		if (It->bUnbound)
		{
			PostProcess = *It;
			break;
		}
	}
}

void UWeatherVisualsSubsystem::Tick(float DeltaTime)
{
	ElapsedSeconds += DeltaTime;
	SecondsUntilSample -= DeltaTime;
	if (SecondsUntilSample <= 0.f)
	{
		SampleTarget();
		SecondsUntilSample = WeatherVisualsDetail::SampleIntervalSeconds;
	}
	if (!bHasState)
	{
		return;
	}
	Smooth(DeltaTime);
	UpdateLightning(DeltaTime);
	ApplySun();
	ApplySky();
	ApplyFog();
	ApplyExposure();
	ApplyMaterialParameters();
	ApplyClouds();
}

TStatId UWeatherVisualsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWeatherVisualsSubsystem, STATGROUP_Tickables);
}

void UWeatherVisualsSubsystem::SampleTarget()
{
	const UWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UWeatherSubsystem>();
	FVector Viewer;
	FIsobarPointWeather Here;
	if (!Weather || !Weather->FindViewerLocation(Viewer) || !Weather->SampleAt(Viewer, Here))
	{
		return;
	}
	const float FogExtinction = float(Here.FogDensity) * WeatherVisualsDetail::ExtinctionForVisibility(WeatherVisualsDetail::DenseFogVisibilityMeters);
	const float TotalExtinction = WeatherVisualsDetail::ExtinctionForVisibility(float(Here.VisibilityMeters));
	const float HazeExtinction = FMath::Max(TotalExtinction - FogExtinction, WeatherVisualsDetail::ExtinctionForVisibility(60000.f));

	Target.CloudCover = float(Here.CloudCover);
	Target.HazeVisibilityMeters = WeatherVisualsDetail::KoschmiederConstant / HazeExtinction;
	Target.FogDensity = float(Here.FogDensity);
	Target.RainMillimetresPerHour = float(Here.PrecipitationRateMillimetresPerHour);
	Target.SnowFraction = float(Here.SnowFraction);
	Target.Wind = WeatherVisualsDetail::ToUnrealWind(Here.WindVelocityMetersPerSecond);
	Target.GustMetresPerSecond = float(Here.GustSpeedMetersPerSecond);
	Target.ThunderActivity = float(Here.ThunderActivity);
	Target.TemperatureCelsius = float(Here.TemperatureCelsius);

	const auto Override = [this](const TCHAR* Name, float& Value)
	{
		if (const float* Found = Overrides.Find(Name))
		{
			Value = *Found;
		}
	};
	Override(TEXT("CloudCover"), Target.CloudCover);
	Override(TEXT("Visibility"), Target.HazeVisibilityMeters);
	Override(TEXT("Fog"), Target.FogDensity);
	Override(TEXT("Rain"), Target.RainMillimetresPerHour);
	Override(TEXT("Snow"), Target.SnowFraction);
	Override(TEXT("Thunder"), Target.ThunderActivity);
	Override(TEXT("Temperature"), Target.TemperatureCelsius);
	if (const float* WindSpeed = Overrides.Find(TEXT("Wind")))
	{
		Target.Wind = FVector2f(*WindSpeed, 0.f);
		Target.GustMetresPerSecond = *WindSpeed * 1.6f;
	}

	if (!bHasState)
	{
		Current = Target;
		Wetness = Overrides.Contains(TEXT("Rain")) && Target.RainMillimetresPerHour > 0.1f && Target.SnowFraction < 0.5f ? 1.f : 0.f;
		SnowCover = Target.SnowFraction > 0.5f && Target.TemperatureCelsius < 1.f && Overrides.Contains(TEXT("Snow")) ? 1.f : 0.f;
		bHasState = true;
	}
}

void UWeatherVisualsSubsystem::Smooth(float DeltaTime)
{
	const float Alpha = 1.f - FMath::Exp(-DeltaTime / WeatherVisualsDetail::SmoothingSeconds);
	Current.CloudCover = FMath::Lerp(Current.CloudCover, Target.CloudCover, Alpha);
	Current.HazeVisibilityMeters = FMath::Lerp(Current.HazeVisibilityMeters, Target.HazeVisibilityMeters, Alpha);
	Current.FogDensity = FMath::Lerp(Current.FogDensity, Target.FogDensity, Alpha);
	Current.RainMillimetresPerHour = FMath::Lerp(Current.RainMillimetresPerHour, Target.RainMillimetresPerHour, Alpha);
	Current.SnowFraction = FMath::Lerp(Current.SnowFraction, Target.SnowFraction, Alpha);
	Current.Wind = FMath::Lerp(Current.Wind, Target.Wind, Alpha);
	Current.GustMetresPerSecond = FMath::Lerp(Current.GustMetresPerSecond, Target.GustMetresPerSecond, Alpha);
	Current.ThunderActivity = FMath::Lerp(Current.ThunderActivity, Target.ThunderActivity, Alpha);
	Current.TemperatureCelsius = FMath::Lerp(Current.TemperatureCelsius, Target.TemperatureCelsius, Alpha);

	const float RainRate = Current.RainMillimetresPerHour * (1.f - Current.SnowFraction);
	const float Snow = Current.RainMillimetresPerHour * Current.SnowFraction;
	if (RainRate > 0.05f)
	{
		Wetness += DeltaTime * RainRate / WeatherVisualsDetail::SoakSecondsPerMillimetreHour;
	}
	else
	{
		const float DryingRate = FMath::Max(0.2f, 1.f + Current.TemperatureCelsius / 20.f - 0.5f * Current.FogDensity);
		Wetness -= DeltaTime * DryingRate / WeatherVisualsDetail::DrySeconds;
	}
	Wetness = FMath::Clamp(Wetness, 0.f, 1.f);
	if (Snow > 0.05f && Current.TemperatureCelsius < 1.5f)
	{
		SnowCover += DeltaTime * Snow / WeatherVisualsDetail::SnowCoverSeconds;
	}
	else if (Current.TemperatureCelsius > 1.f)
	{
		SnowCover -= DeltaTime * (Current.TemperatureCelsius + RainRate) / WeatherVisualsDetail::MeltSeconds;
	}
	SnowCover = FMath::Clamp(SnowCover, 0.f, 1.f);
}

float UWeatherVisualsSubsystem::SunTransmission() const
{
	const float Cover = Current.CloudCover;
	// Broken cloud: the sun is behind a cloud for about the covered share of the time, in spells of a minute or so.
	const float Noise = 0.5f + 0.5f * FMath::PerlinNoise1D(float(ElapsedSeconds / 45.0));
	const float BehindCloud = WeatherVisualsDetail::SmoothStep01(-0.04f, 0.04f, Cover - Noise);
	const float ThroughCloud = FMath::Lerp(1.f, 0.1f, BehindCloud);
	// Overcast: one closed deck that lets only a trace of the beam through.
	const float Overcast = WeatherVisualsDetail::SmoothStep01(0.5f, 1.f, Cover);
	return FMath::Min(ThroughCloud, FMath::Lerp(1.f, WeatherVisualsDetail::OvercastSunTransmission, Overcast));
}

void UWeatherVisualsSubsystem::ApplySun()
{
	ADirectionalLight* Light = Sun.Get();
	const UWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UWeatherSubsystem>();
	if (!Light || !Weather)
	{
		return;
	}
	const FIsobarCalendar Calendar = IsobarCalendarAt(Weather->GetWeatherSeconds());
	const FIsobarSunPosition Position = IsobarSunAtTimeOfDay(WeatherVisualsDetail::MapLatitudeDegrees, Calendar.DayOfYear, Calendar.DayFraction);
	// Isobar: X east, Y north, Z up. Unreal: X east, Y south. The light shines away from the sun.
	SunAltitudeDegrees = float(Position.GetAltitudeDegrees());
	const FVector ToSun(Position.Direction.X, 0.0 - Position.Direction.Y, Position.Direction.Z);
	Light->SetActorRotation((-ToSun).Rotation());
	const float Transmission = SunTransmission();
	UDirectionalLightComponent* Component = Cast<UDirectionalLightComponent>(Light->GetLightComponent());
	Component->SetIntensity(BaseSunLux * Transmission);
	// Below the horizon the sun only lights the atmosphere; its virtual shadow maps would cost milliseconds for nothing.
	const bool bSunCastsShadows = SunAltitudeDegrees > -1.f;
	if (Component->CastShadows != bSunCastsShadows)
	{
		Component->SetCastShadows(bSunCastsShadows);
	}
	Component->SetAtmosphereSunDiskColorScale(FLinearColor::White * WeatherVisualsDetail::SmoothStep01(0.3f, 0.9f, Transmission));
}

void UWeatherVisualsSubsystem::ApplySky()
{
	USkyAtmosphereComponent* Component = Atmosphere.Get();
	if (!Component)
	{
		return;
	}
	// Under a closed deck the sky is a bright, even grey: much more Mie scattering, much less Rayleigh blue.
	const float Overcast = WeatherVisualsDetail::SmoothStep01(0.5f, 1.f, Current.CloudCover);
	Component->SetRayleighScatteringScale(FMath::Lerp(0.0331f, 0.005f, Overcast));
	Component->SetMieScatteringScale(FMath::Lerp(0.003996f, 0.06f, Overcast));
	Component->SetMieAbsorptionScale(FMath::Lerp(0.000444f, 0.002f, Overcast));
	Component->SetMieAnisotropy(FMath::Lerp(0.8f, 0.4f, Overcast));
	// The atmosphere is lit by the sun alone, so dimming the sun for the cloud deck would darken the sky with it.
	// A real deck turns the beam into diffuse light: the sky gets brighter by what the beam loses, minus what the
	// cloud absorbs and reflects back up (an overcast day has about 40 % of a clear day's light).
	// The deck brightens the sky only with the sun up: at night it hides the twilight instead of amplifying it.
	const float Daylight = WeatherVisualsDetail::SmoothStep01(-3.f, 4.f, SunAltitudeDegrees);
	const float DeckTransmission = FMath::Lerp(1.f, WeatherVisualsDetail::OvercastSunTransmission, Overcast);
	const float DeckGain = FMath::Lerp(1.f, WeatherVisualsDetail::OvercastDiffuseShare / DeckTransmission, Overcast);
	const float SkyGain = WeatherVisualsDetail::ClearSkyGain * FMath::Lerp(1.f - 0.95f * Overcast, DeckGain, Daylight);
	// The traced cloud deck absorbs part of the light the atmosphere alone would give, which would make an overcast
	// day darker than it was without clouds; this puts back what it takes (measured against the plain atmosphere).
	float TracedDeckGain = 1.f;
	if (CloudSky && CloudSky->IsActive())
	{
		TracedDeckGain = FMath::Lerp(1.f, WeatherVisualsDetail::TracedDeckSkyGain, Overcast);
	}
	// A flash lights the cloud deck from inside.
	const float Flash = 1.f + 40.f * FlashLevel();
	Component->SetSkyLuminanceFactor(FLinearColor(SkyGain, SkyGain, SkyGain) * Flash * TracedDeckGain);
}

float UWeatherVisualsSubsystem::NightFactor() const
{
	return 1.f - WeatherVisualsDetail::SmoothStep01(-6.f, 0.f, SunAltitudeDegrees);
}

void UWeatherVisualsSubsystem::ApplyFog()
{
	UExponentialHeightFogComponent* Component = Fog.Get();
	if (!Component)
	{
		return;
	}
	const float HazeExtinction = WeatherVisualsDetail::ExtinctionForVisibility(Current.HazeVisibilityMeters);
	Component->SetFogDensity(HazeExtinction * WeatherVisualsDetail::FogDensityPerExtinction);
	const float FogExtinction = Current.FogDensity * WeatherVisualsDetail::ExtinctionForVisibility(WeatherVisualsDetail::DenseFogVisibilityMeters);
	Component->SetSecondFogDensity(FogExtinction * WeatherVisualsDetail::FogDensityPerExtinction);
	Component->SetSecondFogHeightFalloff(WeatherVisualsDetail::RadiationFogHeightFalloff);
	Component->SetSecondFogHeightOffset(0.f);
	const float Glow = NightFactor() * FMath::Lerp(1.f, WeatherVisualsDetail::OvercastSkyGlowGain, Current.CloudCover);
	Component->SetFogInscatteringColor(BaseFogInscattering + WeatherVisualsDetail::SkyGlowLuminance * Glow);
}

void UWeatherVisualsSubsystem::ApplyExposure()
{
	APostProcessVolume* Volume = PostProcess.Get();
	const UWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UWeatherSubsystem>();
	if (!Volume || !Weather)
	{
		return;
	}
	const FIsobarCalendar Calendar = IsobarCalendarAt(Weather->GetWeatherSeconds());
	const FIsobarSunPosition Position = IsobarSunAtTimeOfDay(WeatherVisualsDetail::MapLatitudeDegrees, Calendar.DayOfYear, Calendar.DayFraction);
	const float Relative = WeatherVisualsDetail::RelativeGroundIlluminance(float(Position.GetAltitudeDegrees()), SunTransmission(), Current.CloudCover);
	// The eye adapts only part of the way on a clear day: a dull day still looks duller than a sunny one. Under a
	// closed deck by day the adaptation is almost complete and a compensation applies (see the constants).
	const float OvercastDaylight = WeatherVisualsDetail::SmoothStep01(0.5f, 1.f, Current.CloudCover) * WeatherVisualsDetail::SmoothStep01(3.f, 10.f, float(Position.GetAltitudeDegrees()));
	const float Adaptation = FMath::Lerp(WeatherVisualsDetail::ClearExposureAdaptation, WeatherVisualsDetail::OvercastExposureAdaptation, OvercastDaylight);
	const float Compensation = WeatherVisualsDetail::OvercastExposureCompensationEV * OvercastDaylight;
	const float EV100 = FMath::Max(WeatherVisualsDetail::CalibratedEV100 + Adaptation * FMath::Log2(FMath::Max(Relative, 1e-6f)) - Compensation, WeatherVisualsDetail::NightEV100);
	// Headlights light the road far above the night exposure's range; the eye closes down for them (higher EV100 is darker).
	const float AdaptedEV100 = EV100 + HeadlightAdaptationStops * NightFactor();
	Volume->Settings.AutoExposureMinBrightness = AdaptedEV100;
	Volume->Settings.AutoExposureMaxBrightness = AdaptedEV100;
	// The VR menu panel cancels the exposure with an explicit gain (GameMenuPanel.cpp); keep it in step.
	static IConsoleVariable* PanelExposure = IConsoleManager::Get().FindConsoleVariable(TEXT("dg.MenuPanelExposureEv"));
	if (PanelExposure)
	{
		PanelExposure->Set(AdaptedEV100, ECVF_SetByCode);
	}
}

void UWeatherVisualsSubsystem::ApplyClouds()
{
	CloudSkyFrameLog::Record(GetWorld());
	if (!CloudSky || !CloudSky->IsActive())
	{
		return;
	}
	FCloudSkyInputs Inputs;
	Inputs.CloudCover = Current.CloudCover;
	Inputs.RainIntensity = FMath::Clamp(Current.RainMillimetresPerHour * (1.f - Current.SnowFraction) / 10.f, 0.f, 1.f);
	Inputs.Wind = Current.Wind;
	Inputs.Seconds = ElapsedSeconds;
	if (Sun.IsValid())
	{
		Inputs.SunDirection = -Sun->GetActorForwardVector();
	}
	Inputs.SunVisibility = WeatherVisualsDetail::SmoothStep01(0.3f, 0.9f, SunTransmission());
	CloudSky->Update(Inputs);
}

void UWeatherVisualsSubsystem::ApplyMaterialParameters()
{
	if (ARainEffect* Effect = Rain.Get())
	{
		// Translucent streaks cost fill rate, so the mesh is hidden whenever it isn't raining.
		Effect->SetActorHiddenInGame(Current.RainMillimetresPerHour * (1.f - Current.SnowFraction) < 0.05f);
	}
	if (!Parameters)
	{
		return;
	}
	UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Parameters);
	if (!Instance)
	{
		return;
	}
	const float WindSpeed = Current.Wind.Size();
	const FVector2f WindDirection = WindSpeed > 0.01f ? Current.Wind / WindSpeed : FVector2f(1.f, 0.f);
	// Gusts come and go over a few seconds on top of the mean wind.
	const float GustNoise = FMath::Max(0.f, FMath::PerlinNoise1D(float(ElapsedSeconds / 4.0)));
	const float WindNow = WindSpeed + (Current.GustMetresPerSecond - WindSpeed) * GustNoise;
	Instance->SetScalarParameterValue(TEXT("Wetness"), Wetness);
	Instance->SetScalarParameterValue(TEXT("Puddles"), WeatherVisualsDetail::SmoothStep01(0.6f, 1.f, Wetness));
	Instance->SetScalarParameterValue(TEXT("RainIntensity"), FMath::Clamp(Current.RainMillimetresPerHour * (1.f - Current.SnowFraction) / 10.f, 0.f, 1.f));
	Instance->SetScalarParameterValue(TEXT("SnowCover"), SnowCover);
	Instance->SetScalarParameterValue(TEXT("WindSpeed"), WindNow);
	Instance->SetScalarParameterValue(TEXT("WindDirectionX"), WindDirection.X);
	Instance->SetScalarParameterValue(TEXT("WindDirectionY"), WindDirection.Y);
	Instance->SetScalarParameterValue(TEXT("CloudCover"), Current.CloudCover);
	Instance->SetScalarParameterValue(TEXT("Thunder"), Current.ThunderActivity);
	Instance->SetScalarParameterValue(TEXT("Night"), NightFactor());
	const UWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UWeatherSubsystem>();
	if (Weather)
	{
		const FIsobarCalendar Calendar = IsobarCalendarAt(Weather->GetWeatherSeconds());
		WeatherVisualsDetail::FSeasonState Season = WeatherVisualsDetail::SeasonAt(float(Calendar.GetContinuousDayOfYear()), WindSpeed);
		if (const float* Fallen = Overrides.Find(TEXT("FallenLeaves")))
		{
			Season.FallenLeaves = *Fallen;
		}
		Instance->SetScalarParameterValue(TEXT("LeafDensity"), Season.LeafDensity);
		Instance->SetScalarParameterValue(TEXT("LeafColour"), Season.LeafColour);
		Instance->SetScalarParameterValue(TEXT("FallenLeaves"), Season.FallenLeaves);
		Instance->SetScalarParameterValue(TEXT("LeafFall"), Season.LeafFall);
		if (ARainEffect* Effect = Leaves.Get())
		{
			Effect->SetActorHiddenInGame(Season.LeafFall < 0.02f);
		}
	}
}
