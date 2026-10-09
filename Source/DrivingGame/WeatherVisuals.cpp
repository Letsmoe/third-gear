#include "WeatherVisuals.h"

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
#include "WeatherSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeatherVisuals, Log, All);

namespace WeatherVisualsDetail
{
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
	constexpr float NightEV100 = 2.5f;

	/** Direct sun left under a closed deck, and the diffuse light such a day has relative to a clear one. */
	constexpr float OvercastSunTransmission = 0.02f;
	constexpr float OvercastDiffuseShare = 0.4f;
	/**
	 * The sun is set to the illuminance calibrated on the ground (75 klx), well below the 128 klx above the
	 * atmosphere that lights a real sky, so the clear sky came out about two stops darker than sunlit asphalt.
	 */
	constexpr float ClearSkyGain = 1.8f;

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

	/** Parses -WeatherOverride="Name=Value,Name=Value". */
	TMap<FString, float> ParseOverrides()
	{
		TMap<FString, float> Result;
		FString Text;
		if (!FParse::Value(FCommandLine::Get(), TEXT("WeatherOverride="), Text, /*bShouldStopOnSeparator=*/false))
		{
			return Result;
		}
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
	Overrides = WeatherVisualsDetail::ParseOverrides();
	Parameters = LoadObject<UMaterialParameterCollection>(nullptr, WeatherVisualsDetail::ParameterCollectionPath, nullptr, LOAD_NoWarn);
	if (!Parameters)
	{
		UE_LOG(LogWeatherVisuals, Warning, TEXT("%s missing; run Scripts/create_weather_parameters.py"), WeatherVisualsDetail::ParameterCollectionPath);
	}
	UE_LOG(LogWeatherVisuals, Log, TEXT("Weather visuals: sun %s, atmosphere %s, fog %s, post process %s, %d overrides"),
		Sun.IsValid() ? TEXT("yes") : TEXT("no"), Atmosphere.IsValid() ? TEXT("yes") : TEXT("no"),
		Fog.IsValid() ? TEXT("yes") : TEXT("no"), PostProcess.IsValid() ? TEXT("yes") : TEXT("no"), Overrides.Num());
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
	ApplySun();
	ApplySky();
	ApplyFog();
	ApplyExposure();
	ApplyMaterialParameters();
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
		Wetness = Overrides.Contains(TEXT("Rain")) && Target.RainMillimetresPerHour > 0.1f ? 1.f : 0.f;
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

	const float Rain = Current.RainMillimetresPerHour * (1.f - Current.SnowFraction);
	const float Snow = Current.RainMillimetresPerHour * Current.SnowFraction;
	if (Rain > 0.05f)
	{
		Wetness += DeltaTime * Rain / WeatherVisualsDetail::SoakSecondsPerMillimetreHour;
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
		SnowCover -= DeltaTime * (Current.TemperatureCelsius + Rain) / WeatherVisualsDetail::MeltSeconds;
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
	const FVector ToSun(Position.Direction.X, 0.0 - Position.Direction.Y, Position.Direction.Z);
	Light->SetActorRotation((-ToSun).Rotation());
	const float Transmission = SunTransmission();
	UDirectionalLightComponent* Component = Cast<UDirectionalLightComponent>(Light->GetLightComponent());
	Component->SetIntensity(BaseSunLux * Transmission);
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
	const float DeckTransmission = FMath::Lerp(1.f, WeatherVisualsDetail::OvercastSunTransmission, Overcast);
	const float SkyGain = WeatherVisualsDetail::ClearSkyGain * FMath::Lerp(1.f, WeatherVisualsDetail::OvercastDiffuseShare / DeckTransmission, Overcast);
	Component->SetSkyLuminanceFactor(FLinearColor(SkyGain, SkyGain, SkyGain));
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
	// The eye adapts only part of the way: a dull day still looks duller than a sunny one.
	const float EV100 = FMath::Max(WeatherVisualsDetail::CalibratedEV100 + 0.75f * FMath::Log2(FMath::Max(Relative, 1e-6f)), WeatherVisualsDetail::NightEV100);
	Volume->Settings.AutoExposureMinBrightness = EV100;
	Volume->Settings.AutoExposureMaxBrightness = EV100;
	// The VR menu panel cancels the exposure with an explicit gain (GameMenuPanel.cpp); keep it in step.
	static IConsoleVariable* PanelExposure = IConsoleManager::Get().FindConsoleVariable(TEXT("dg.MenuPanelExposureEv"));
	if (PanelExposure)
	{
		PanelExposure->Set(EV100, ECVF_SetByCode);
	}
}

void UWeatherVisualsSubsystem::ApplyMaterialParameters()
{
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
}
