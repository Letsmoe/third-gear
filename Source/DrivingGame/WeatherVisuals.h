#pragma once

#include "CoreMinimal.h"
#include "IsobarPointWeather.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeatherVisuals.generated.h"

class ADirectionalLight;
class ARainEffect;
class UCloudSkyRig;
class APostProcessVolume;
class UExponentialHeightFogComponent;
class UMaterialParameterCollection;
class USkyAtmosphereComponent;

/** A lightning strike: where it hit (world cm) and how strong it is, 0 to 1. Thunder follows at the speed of sound. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnLightningStrike, const FVector& /*WorldLocation*/, float /*Strength*/);

/** The weather as the renderer shows it, smoothed so nothing jumps when the weather steps. */
USTRUCT()
struct FWeatherVisualState
{
	GENERATED_BODY()

	/** 0 to 1. */
	float CloudCover = 0.f;
	/** Horizontal visibility from haze and precipitation, without radiation fog. */
	float HazeVisibilityMeters = 40000.f;
	/** Radiation fog, 0 to 1: 1 is about 120 m visibility. */
	float FogDensity = 0.f;
	float RainMillimetresPerHour = 0.f;
	/** Share of the precipitation falling as snow, 0 to 1. */
	float SnowFraction = 0.f;
	/** Wind at car height, X east and Y south (Unreal), metres per second. */
	FVector2f Wind = FVector2f::ZeroVector;
	float GustMetresPerSecond = 0.f;
	/** 0 to 1, how close and active the nearest thunderstorm is. */
	float ThunderActivity = 0.f;
	float TemperatureCelsius = 15.f;
};

/**
 * Drives the level's sun, sky, fog and exposure from the Isobar weather (UWeatherSubsystem), and publishes the weather
 * for materials in the parameter collection /Game/World/MPC_Weather (wetness, snow, wind, rain).
 *
 * The level's lighting actors come from Scripts/world_lighting.py; this only adjusts them. The sun stands where it
 * would over Bergedorf at the weather clock's date and time. Clouds dim it, and in broken cloud it comes and goes.
 * Overcast skies turn grey through stronger Mie scattering in the atmosphere. Haze and radiation fog are two height
 * fog layers set from Isobar's visibility, and exposure adapts slowly between day and night.
 *
 * For screenshots and tests, `-WeatherOverride="CloudCover=0.9,Fog=0.6,Rain=4,Snow=0,Wind=8,Thunder=0"` replaces
 * the sampled values; the console command `Weather.Override` does the same at runtime.
 */
UCLASS()
class DRIVINGGAME_API UWeatherVisualsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Only game worlds get weather visuals. */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The smoothed weather currently shown. */
	const FWeatherVisualState& GetState() const { return Current; }

	/** How wet the roads are, 0 to 1: rises in rain and dries slowly afterwards. */
	float GetWetness() const { return Wetness; }

	/** How much of the ground is under snow, 0 to 1. */
	float GetSnowCover() const { return SnowCover; }

	/** Replaces the overrides ("Name=Value,...", as -WeatherOverride) and jumps to the result without smoothing. */
	void SetOverrides(const FString& Spec);

	/** Jumps to the current weather on the next tick instead of easing towards it (after a clock change). */
	void Snap();

	/** Broadcast at every lightning flash. */
	FOnLightningStrike OnLightning;

private:
	/** Finds the sun, atmosphere, fog and post process volume of the level. */
	void FindLevelActors();

	/** Reads the weather where the viewer is into Target, applying any command-line override. */
	void SampleTarget();

	/** Moves Current towards Target and integrates wetness and snow cover. */
	void Smooth(float DeltaTime);

	void ApplySun();
	void ApplySky();
	void ApplyFog();
	void ApplyExposure();
	void ApplyMaterialParameters();
	void ApplyClouds();

	/** How much direct sun gets through the clouds right now, 0 to 1. */
	float SunTransmission() const;

	/** Spawns the rain streaks and the light that lightning flashes with. */
	void SpawnEffects();

	/** Starts strikes at a rate that grows with the thunder activity, and updates the flash light. */
	void UpdateLightning(float DeltaTime);

	/** Brightness of the current flash, 0 to 1: a few pulses over a third of a second. */
	float FlashLevel() const;

	TWeakObjectPtr<ADirectionalLight> Sun;
	TWeakObjectPtr<USkyAtmosphereComponent> Atmosphere;
	TWeakObjectPtr<UExponentialHeightFogComponent> Fog;
	TWeakObjectPtr<APostProcessVolume> PostProcess;
	TWeakObjectPtr<ARainEffect> Rain;
	/** Falling autumn leaves: the rain's streak mesh with M_FallingLeaves. */
	TWeakObjectPtr<ARainEffect> Leaves;
	TWeakObjectPtr<ADirectionalLight> FlashLight;
	double SecondsSinceStrike = 1000.0;
	float StrikeStrength = 0.f;
	FRandomStream LightningRandom{2207};

	UPROPERTY()
	TObjectPtr<UMaterialParameterCollection> Parameters;

	/** Clouds in the sky, from the sky light capture (CloudSky.h). */
	UPROPERTY()
	TObjectPtr<UCloudSkyRig> CloudSky;

	FWeatherVisualState Target;
	FWeatherVisualState Current;
	bool bHasState = false;
	float Wetness = 0.f;
	float SnowCover = 0.f;
	float SecondsUntilSample = 0.f;
	double ElapsedSeconds = 0.0;
	float BaseSunLux = 75000.f;
	/** Fog colour the level was made with, before the night's sky glow is added. */
	FLinearColor BaseFogInscattering = FLinearColor::Black;
	/** Sun altitude this frame, degrees. */
	float SunAltitudeDegrees = 45.f;

	/** 0 by day, 1 once the sun is 6 degrees below the horizon (civil twilight over). */
	float NightFactor() const;

	/** Values from -WeatherOverride, by name; applied over every sample. */
	TMap<FString, float> Overrides;
};
