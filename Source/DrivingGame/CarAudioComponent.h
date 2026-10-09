#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CarSoundDsp.h"
#include "CarSoundMapper.h"
#include "CarAudioComponent.generated.h"

class ACarPawn;
class UAudioComponent;
class UCarSynthComponent;
class USoundBase;

/**
 * Everything the driver hears, on the player car: the procedural engine, tyre, wind and mechanical sounds
 * (UCarSynthComponent, driven from the physics telemetry) and the ambience around the car (town, birds, wind in
 * trees, rain on the roof, glass and road, thunder), driven from the weather and the time of day.
 *
 * Volumes follow UDrivingPreferences: EngineVolume for the car, AmbienceVolume for the world. With `-nosound` nothing
 * is created, so the headless drive test is unaffected. With `-AudioCapture=<dir>` the car sound is also rendered
 * offline (same DSP, fixed time step) to <dir>/car.wav (and car_engine.wav, car_road.wav, car_wind.wav, car_events.wav) with a telemetry CSV next to it, for Scripts/audio_test.sh.
 */
UCLASS(ClassGroup = "Audio", meta = (BlueprintSpawnableComponent))
class DRIVINGGAME_API UCarAudioComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCarAudioComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Switches the flasher relay sound. The cockpit's indicator stalk calls this. */
	void SetIndicatorActive(bool bActive) { bIndicatorActive = bActive; }

	/** Plays thunder from a strike this far away; it arrives after distance / speed of sound. */
	void TriggerThunder(float DistanceMeters);

	/**
	 * A lightning flash at a world location (UWeatherVisualsSubsystem's OnLightning delegate): thunder follows after
	 * the distance at the speed of sound, louder for a stronger strike. Once a flash source is bound the component
	 * stops inventing strikes of its own.
	 */
	void HandleLightning(const FVector& WorldLocation, float Strength);

	/** Test hook: pins the road surface and wetness instead of tracing the ground. */
	void SetForcedSurface(ECarRoadSurface Surface, float InWetness);

	/** Test hook: weather values that replace the weather subsystem's (wind blows along +X, towards the east). */
	void SetForcedWeather(float PrecipitationMmPerHour, float WindMps, float GustMps, float ThunderActivity, float SunAltitudeDegrees);

	/** Tells the component that real lightning strikes arrive through HandleLightning. */
	void SetLightningFromWeather(bool bEnabled) { bLightningFromWeather = bEnabled; }

	/** Test hook: with -AudioMixerRecord=<dir>, writes everything the mixer played since the start to <dir>/game_mix.wav. */
	void FinishMixerRecording();

	/** Label stored with every captured sample, so a recording can be cut into scenes. */
	void SetCaptureLabel(const FString& Label) { CaptureLabel = Label; }

private:
	/** A looping ambience bed with its own volume and low pass. */
	struct FAmbienceLayer
	{
		FString AssetName;
		float MaximumVolume = 1.f;
		float LowPassHz = 20000.f;
		float Volume = 0.f;
		float TargetVolume = 0.f;
		TObjectPtr<UAudioComponent> Audio;
	};

	struct FPendingThunder
	{
		float SecondsLeft = 0.f;
		FString AssetName;
		float Volume = 1.f;
		float LowPassHz = 20000.f;
		float Pitch = 1.f;
	};

	struct FWeatherNow
	{
		float PrecipitationMmPerHour = 0.f;
		float WindMps = 0.f;
		FVector2D WindVelocityMps = FVector2D::ZeroVector; // Unreal frame: x east, y south
		float GustMps = 0.f;
		float SnowFraction = 0.f;
		float RoadWetness = 0.f;
	bool bLightningFromWeather = false;
		float ThunderActivity = 0.f;
		float SunAltitudeDegrees = 30.f;
	};

	void CreateCarSynth();
	void CreateAmbienceLayers();
	FAmbienceLayer& AddLayer(const FString& AssetName, float MaximumVolume, float LowPassHz);
	void ApplyPreferences();

	FWeatherNow SampleWeather();
	void UpdateSurface(float DeltaTime);
	FCarSoundEnvironment BuildEnvironment(const FWeatherNow& Weather);
	void UpdateAmbience(const FWeatherNow& Weather, float SpeedMps, float DeltaTime);
	void UpdateThunder(const FWeatherNow& Weather, float DeltaTime);
	void PlayThunder(const FPendingThunder& Thunder);
	void FeedCarSound(const FCarTelemetry& Telemetry, const FCarSoundEnvironment& Environment, float DeltaTime);
	void CaptureFrame(const FCarTelemetry& Telemetry, const FCarSoundInputs& Inputs, float DeltaTime);
	void WriteCapture();
	static void WriteWav(const FString& Path, const TArray<int16>& Samples);
	FAmbienceLayer* FindLayer(const FString& AssetName);
	ACarPawn* GetCar() const;

	UPROPERTY(Transient)
	TObjectPtr<UCarSynthComponent> CarSynth;

	TArray<FAmbienceLayer> Layers;
	TArray<FPendingThunder> PendingThunder;
	FCarSoundMapper Mapper;
	FDelegateHandle PreferencesHandle;

	bool bAudioAvailable = false;
	bool bIndicatorActive = false;
	float AmbienceVolumeScale = 1.f;

	// Surface under the car and wetness of the road.
	float SurfaceWeights[static_cast<int32>(ECarRoadSurface::Count)] = {1.f, 0.f, 0.f, 0.f};
	float SurfaceTraceCountdown = 0.f;
	ECarRoadSurface TracedSurface = ECarRoadSurface::Asphalt;
	bool bSurfaceForced = false;
	bool bLogSurfaceTraces = false; // -LogSurfaceTraces: every ground trace goes to the log
	float RoadWetness = 0.f;
	bool bLightningFromWeather = false;
	float WeatherSampleCountdown = 0.f;
	FWeatherNow CachedWeather;
	bool bWeatherForced = false;
	float SmoothedPrecipitation = 0.f;
	float NextThunderSeconds = 8.f;
	FRandomStream Random{4711};

	// Offline capture (-AudioCapture=<dir>).
	FString CaptureDirectory;
	FString MixerRecordDirectory;
	bool bMixerRecording = false;
	/** One offline renderer per stem, so the recording can also be analysed engine, road, wind and one-shots apart. */
	struct FCaptureStem
	{
		FString Name;
		TUniquePtr<FCarSoundDsp> Dsp;
		TArray<int16> Samples;
	};
	TArray<FCaptureStem> CaptureStems;
	TArray<FString> CaptureRows;
	FString CaptureLabel = TEXT("run");
	double CaptureTime = 0.0;
	double CaptureFrameCarry = 0.0;

	// Cost measurement, reported at the end of a capture.
	double TickCostTotalSeconds = 0.0;
	double TickCostMaxSeconds = 0.0;
	double DspRenderSeconds = 0.0;
	double CaptureRenderSecondsThisTick = 0.0;
	int32 TickCostCount = 0;
};
