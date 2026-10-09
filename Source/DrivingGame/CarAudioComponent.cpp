#include "CarAudioComponent.h"

#include "CarMovementComponent.h"
#include "CarPawn.h"
#include "CarSettings.h"
#include "CarSynthComponent.h"
#include "Components/AudioComponent.h"
#include "DrivingPreferences.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "IsobarPointWeather.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "AudioDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Sound/SoundBase.h"
#include "WeatherSubsystem.h"
#include "WeatherVisuals.h"
#include "WorldSurfaceQuery.h"

DEFINE_LOG_CATEGORY_STATIC(LogCarAudio, Log, All);

namespace
{
constexpr float SpeedOfSoundMps = 343.f;
constexpr int32 CaptureSampleRate = 48000;
constexpr float SurfaceTraceInterval = 0.1f;
constexpr float WeatherSampleInterval = 0.25f;
const TCHAR* LoopFolder = TEXT("/Game/Audio/Loops");
const TCHAR* OneShotFolder = TEXT("/Game/Audio/OneShots");

/** 0 below Low, 1 above High, smooth in between. */
float SmoothRamp(float Value, float Low, float High)
{
	const float T = FMath::Clamp((Value - Low) / (High - Low), 0.f, 1.f);
	return T * T * (3.f - 2.f * T);
}

/** Maps the ground material name to the tyre noise class. */
ECarRoadSurface ClassifySurfaceName(const FName& Name)
{
	if (Name.IsNone())
	{
		return ECarRoadSurface::Asphalt; // flat test maps and anything without generated ground
	}
	const FString Text = Name.ToString();
	if (Text.Contains(TEXT("Cobble")))
	{
		return ECarRoadSurface::Cobble;
	}
	if (Text.Contains(TEXT("Pavers")) || Text.Contains(TEXT("Pavement")) || Text.Contains(TEXT("Path_Paved")) || Text.Contains(TEXT("Kerb")))
	{
		return ECarRoadSurface::Pavers;
	}
	if (Text.StartsWith(TEXT("Road_")) || Text.Contains(TEXT("Bridge")))
	{
		return ECarRoadSurface::Asphalt;
	}
	return ECarRoadSurface::Rough;
}

const TCHAR* CarAudioSurfaceName(ECarRoadSurface Surface)
{
	switch (Surface)
	{
	case ECarRoadSurface::Asphalt: return TEXT("asphalt");
	case ECarRoadSurface::Cobble: return TEXT("cobble");
	case ECarRoadSurface::Pavers: return TEXT("pavers");
	default: return TEXT("rough");
	}
}

FString PathFor(const TCHAR* Folder, const FString& AssetName)
{
	return FString::Printf(TEXT("%s/%s.%s"), Folder, *AssetName, *AssetName);
}
}

UCarAudioComponent::UCarAudioComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

ACarPawn* UCarAudioComponent::GetCar() const
{
	return Cast<ACarPawn>(GetOwner());
}

void UCarAudioComponent::BeginPlay()
{
	Super::BeginPlay();
	FParse::Value(FCommandLine::Get(), TEXT("AudioCapture="), CaptureDirectory);
	bLogSurfaceTraces = FParse::Param(FCommandLine::Get(), TEXT("LogSurfaceTraces"));
	FAudioDevice* Device = GEngine ? GEngine->GetMainAudioDevice().GetAudioDevice() : nullptr;
	bAudioAvailable = FApp::CanEverRenderAudio() && Device != nullptr;
	if (!CaptureDirectory.IsEmpty())
	{
		const TPair<const TCHAR*, uint32> Stems[] = {{TEXT("car"), CarSoundStem::All}, {TEXT("car_engine"), CarSoundStem::Engine},
			{TEXT("car_road"), CarSoundStem::Road}, {TEXT("car_wind"), CarSoundStem::Wind}, {TEXT("car_events"), CarSoundStem::Events}};
		for (const TPair<const TCHAR*, uint32>& Stem : Stems)
		{
			FCaptureStem& Capture = CaptureStems.AddDefaulted_GetRef();
			Capture.Name = Stem.Key;
			Capture.Dsp = MakeUnique<FCarSoundDsp>(CaptureSampleRate);
			Capture.Dsp->SetStemMask(Stem.Value);
		}
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("AudioMixerRecord="), MixerRecordDirectory) && bAudioAvailable)
	{
		UAudioMixerBlueprintLibrary::StartRecordingOutput(this, 900.f);
		bMixerRecording = true;
	}
	PreferencesHandle = UDrivingPreferences::OnChanged().AddUObject(this, &UCarAudioComponent::ApplyPreferences);
}

void UCarAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UDrivingPreferences::OnChanged().Remove(PreferencesHandle);
	if (!CaptureStems.IsEmpty())
	{
		WriteCapture();
	}
	for (FAmbienceLayer& Layer : Layers)
	{
		if (Layer.Audio)
		{
			Layer.Audio->Stop();
		}
	}
	Super::EndPlay(EndPlayReason);
}

void UCarAudioComponent::FinishMixerRecording()
{
	if (!bMixerRecording)
	{
		return;
	}
	bMixerRecording = false;
	UAudioMixerBlueprintLibrary::StopRecordingOutput(this, EAudioRecordingExportType::WavFile, TEXT("game_mix"), MixerRecordDirectory);
	UE_LOG(LogCarAudio, Display, TEXT("Mixer recording written to %s/game_mix.wav"), *MixerRecordDirectory);
}

void UCarAudioComponent::SetForcedSurface(ECarRoadSurface Surface, float InWetness)
{
	bSurfaceForced = true;
	TracedSurface = Surface;
	RoadWetness = InWetness;
}

void UCarAudioComponent::SetForcedWeather(float PrecipitationMmPerHour, float WindMps, float GustMps, float ThunderActivity, float SunAltitudeDegrees)
{
	bWeatherForced = true;
	CachedWeather.PrecipitationMmPerHour = PrecipitationMmPerHour;
	CachedWeather.WindMps = WindMps;
	CachedWeather.WindVelocityMps = FVector2D(WindMps, 0.f);
	CachedWeather.GustMps = GustMps;
	CachedWeather.ThunderActivity = ThunderActivity;
	CachedWeather.SunAltitudeDegrees = SunAltitudeDegrees;
}

// --- setup ---------------------------------------------------------------------------------------------------------

void UCarAudioComponent::CreateCarSynth()
{
	ACarPawn* Car = GetCar();
	CarSynth = NewObject<UCarSynthComponent>(Car, TEXT("CarSynth"));
	CarSynth->SetupAttachment(Car->GetRootComponent());
	CarSynth->RegisterComponent();
	CarSynth->Start();
}

UCarAudioComponent::FAmbienceLayer& UCarAudioComponent::AddLayer(const FString& AssetName, float MaximumVolume, float LowPassHz)
{
	FAmbienceLayer& Layer = Layers.AddDefaulted_GetRef();
	Layer.AssetName = AssetName;
	Layer.MaximumVolume = MaximumVolume;
	Layer.LowPassHz = LowPassHz;
	USoundBase* Sound = LoadObject<USoundBase>(nullptr, *PathFor(LoopFolder, AssetName), nullptr, LOAD_NoWarn);
	if (!Sound)
	{
		UE_LOG(LogCarAudio, Warning, TEXT("Ambience loop %s missing; run Scripts/import_audio.py"), *AssetName);
		return Layer;
	}
	Layer.Audio = UGameplayStatics::CreateSound2D(GetWorld(), Sound, 0.f, 1.f, 0.f, nullptr, /*bPersistAcrossLevelTransition=*/false, /*bAutoDestroy=*/false);
	if (Layer.Audio)
	{
		Layer.Audio->SetLowPassFilterEnabled(true);
		Layer.Audio->SetLowPassFilterFrequency(LowPassHz);
		Layer.Audio->Play(FMath::FRandRange(0.f, 30.f)); // start somewhere inside the loop so beds do not line up
	}
	return Layer;
}

void UCarAudioComponent::CreateAmbienceLayers()
{
	// Outside sounds heard through the closed car: the low pass stands for glass and doors.
	AddLayer(TEXT("wind_trees_a"), 0.40f, 1100.f);
	AddLayer(TEXT("wind_trees_b"), 0.40f, 1100.f);
	AddLayer(TEXT("town_day"), 0.45f, 900.f);
	AddLayer(TEXT("town_night"), 0.30f, 900.f);
	AddLayer(TEXT("birds_day_b"), 0.30f, 3500.f);
	AddLayer(TEXT("birds_day_c"), 0.30f, 3500.f);
	AddLayer(TEXT("rain_roof_light"), 0.40f, 6500.f);
	AddLayer(TEXT("rain_roof_heavy"), 0.90f, 6500.f);
	AddLayer(TEXT("rain_glass_light"), 0.22f, 7000.f);
	AddLayer(TEXT("rain_glass_heavy"), 0.50f, 7000.f);
	AddLayer(TEXT("rain_road_light"), 0.20f, 2200.f);
	AddLayer(TEXT("rain_road_heavy"), 0.45f, 2200.f);
}

void UCarAudioComponent::ApplyPreferences()
{
	const UDrivingPreferences* Preferences = GetDefault<UDrivingPreferences>();
	AmbienceVolumeScale = FMath::Clamp(Preferences->AmbienceVolume, 0.f, 1.f);
	if (CarSynth)
	{
		CarSynth->SetVolumeMultiplier(FMath::Clamp(Preferences->EngineVolume, 0.f, 1.f));
	}
}

UCarAudioComponent::FAmbienceLayer* UCarAudioComponent::FindLayer(const FString& AssetName)
{
	return Layers.FindByPredicate([&AssetName](const FAmbienceLayer& Layer) { return Layer.AssetName == AssetName; });
}

// --- world state ---------------------------------------------------------------------------------------------------

UCarAudioComponent::FWeatherNow UCarAudioComponent::SampleWeather()
{
	if (bWeatherForced)
	{
		return CachedWeather;
	}
	FWeatherNow Now = CachedWeather;
	// Rain, wind, thunder and road wetness come smoothed from the weather visuals; the sun's height from the weather itself.
	if (const UWeatherVisualsSubsystem* Visuals = GetWorld() ? GetWorld()->GetSubsystem<UWeatherVisualsSubsystem>() : nullptr)
	{
		const FWeatherVisualState& State = Visuals->GetState();
		Now.PrecipitationMmPerHour = State.RainMillimetresPerHour;
		Now.SnowFraction = State.SnowFraction;
		Now.WindVelocityMps = FVector2D(State.Wind.X, State.Wind.Y);
		Now.WindMps = State.Wind.Size();
		Now.GustMps = State.GustMetresPerSecond;
		Now.ThunderActivity = State.ThunderActivity;
		Now.RoadWetness = Visuals->GetWetness();
	}
	const UWeatherSubsystem* Weather = GetWorld() ? GetWorld()->GetSubsystem<UWeatherSubsystem>() : nullptr;
	FIsobarPointWeather Point;
	if (Weather && Weather->SampleAt(GetOwner()->GetActorLocation(), Point))
	{
		Now.SunAltitudeDegrees = static_cast<float>(Point.SunAltitudeDegrees);
	}
	return Now;
}

void UCarAudioComponent::UpdateSurface(float DeltaTime)
{
	SurfaceTraceCountdown -= DeltaTime;
	if (!bSurfaceForced && SurfaceTraceCountdown <= 0.f)
	{
		SurfaceTraceCountdown = SurfaceTraceInterval;
		const FVector Start = GetOwner()->GetActorLocation();
		FCollisionQueryParams Params(SCENE_QUERY_STAT(CarAudioSurface), /*bTraceComplex=*/true, GetOwner());
		Params.bReturnFaceIndex = true;
		FHitResult Hit;
		const bool bHitGround = GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0.f, 0.f, 300.f), ECC_Visibility, Params);
		if (!bHitGround && bLogSurfaceTraces)
		{
			UE_LOG(LogCarAudio, Display, TEXT("Surface trace at %.0f, %.0f, z %.2f m: no ground"), Start.X / 100.f, Start.Y / 100.f, Start.Z / 100.f);
		}
		if (bHitGround)
		{
			const FName GroundName = FindWorldSurfaceName(Hit);
			if (bLogSurfaceTraces)
			{
				UE_LOG(LogCarAudio, Display, TEXT("Surface trace at %.0f, %.0f, z %.2f m: %s face %d ground material %s"), Start.X / 100.f, Start.Y / 100.f,
					Start.Z / 100.f, *GetNameSafe(Hit.Component.Get()), Hit.FaceIndex, *FindWorldSurfaceName(Hit).ToString());
			}
			const ECarRoadSurface Surface = ClassifySurfaceName(GroundName);
			if (Surface != TracedSurface)
			{
				UE_LOG(LogCarAudio, Display, TEXT("Road surface under the car: %s (ground material %s) at %.0f, %.0f m"),
					CarAudioSurfaceName(Surface), *GroundName.ToString(), Start.X / 100.f, Start.Y / 100.f);
			}
			TracedSurface = Surface;
		}
	}
	const float Follow = 1.f - FMath::Exp(-DeltaTime / 0.08f);
	for (int32 Surface = 0; Surface < static_cast<int32>(ECarRoadSurface::Count); ++Surface)
	{
		const float Target = Surface == static_cast<int32>(TracedSurface) ? 1.f : 0.f;
		SurfaceWeights[Surface] += (Target - SurfaceWeights[Surface]) * Follow;
	}
}

FCarSoundEnvironment UCarAudioComponent::BuildEnvironment(const FWeatherNow& Weather)
{
	FCarSoundEnvironment Environment;
	for (int32 Surface = 0; Surface < static_cast<int32>(ECarRoadSurface::Count); ++Surface)
	{
		Environment.SurfaceWeights[Surface] = SurfaceWeights[Surface];
	}
	Environment.Wetness = bSurfaceForced ? RoadWetness : Weather.RoadWetness;
	Environment.bIndicatorOn = bIndicatorActive;

	const FVector WindWorld(Weather.WindVelocityMps.X, Weather.WindVelocityMps.Y, 0.f);
	const AActor* Car = GetOwner();
	Environment.WindLocalMps = FVector2D(FVector::DotProduct(WindWorld, Car->GetActorForwardVector()), FVector::DotProduct(WindWorld, Car->GetActorRightVector()));

	const UCarSettings* Settings = GetDefault<UCarSettings>();
	for (const FVector2D& Point : Settings->TorqueCurve)
	{
		Environment.FullLoadTorqueNm = FMath::Max(Environment.FullLoadTorqueNm, static_cast<float>(Point.Y));
	}
	Environment.RevLimitRpm = Settings->RevLimitRpm;
	return Environment;
}

// --- car sound -------------------------------------------------------------------------------------------------------

void UCarAudioComponent::FeedCarSound(const FCarTelemetry& Telemetry, const FCarSoundEnvironment& Environment, float DeltaTime)
{
	TArray<FCarSoundEventRequest> Events;
	const FCarSoundInputs Inputs = Mapper.Update(Telemetry, Environment, DeltaTime, Events);
	if (CarSynth && CarSynth->GetDsp().IsValid())
	{
		CarSynth->GetDsp()->SetInputs(Inputs);
		for (const FCarSoundEventRequest& Request : Events)
		{
			CarSynth->GetDsp()->PostEvent(Request.Event, Request.Strength);
		}
	}
	if (!CaptureStems.IsEmpty())
	{
		for (const FCaptureStem& Stem : CaptureStems)
		{
			Stem.Dsp->SetInputs(Inputs);
			for (const FCarSoundEventRequest& Request : Events)
			{
				Stem.Dsp->PostEvent(Request.Event, Request.Strength);
			}
		}
		const double CaptureStart = FPlatformTime::Seconds();
		CaptureFrame(Telemetry, Inputs, DeltaTime);
		CaptureRenderSecondsThisTick = FPlatformTime::Seconds() - CaptureStart; // test overhead, not part of the game's cost
	}
}

void UCarAudioComponent::CaptureFrame(const FCarTelemetry& Telemetry, const FCarSoundInputs& Inputs, float DeltaTime)
{
	CaptureFrameCarry += DeltaTime * CaptureSampleRate;
	const int32 Frames = FMath::FloorToInt(CaptureFrameCarry);
	CaptureFrameCarry -= Frames;
	for (FCaptureStem& Stem : CaptureStems)
	{
		TArray<float> Block;
		Block.SetNumZeroed(Frames * 2);
		const double RenderStart = FPlatformTime::Seconds();
		Stem.Dsp->Render(Block.GetData(), Frames);
		const double RenderSeconds = FPlatformTime::Seconds() - RenderStart;
		if (Stem.Name == TEXT("car"))
		{
			DspRenderSeconds += RenderSeconds;
		}
		for (const float Sample : Block)
		{
			Stem.Samples.Add(static_cast<int16>(FMath::Clamp(Sample, -1.f, 1.f) * 32767.f));
		}
	}
	CaptureTime += DeltaTime;
	CaptureRows.Add(FString::Printf(TEXT("%.3f,%.1f,%.2f,%d,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f,%s"), CaptureTime, Telemetry.EngineRpm,
		Telemetry.SpeedKmh, Telemetry.EngagedGear, Inputs.Throttle, Inputs.Load, Telemetry.Boost, Inputs.SquealLevel,
		Telemetry.bEngineRunning ? 1 : 0, Telemetry.bCranking ? 1 : 0, Telemetry.Clutch, Telemetry.LoadN[0], *CaptureLabel));
}

void UCarAudioComponent::WriteWav(const FString& Path, const TArray<int16>& Samples)
{
	const int32 DataBytes = Samples.Num() * sizeof(int16);
	TArray<uint8> File;
	auto Append = [&File](const void* Data, int32 Size) { File.Append(static_cast<const uint8*>(Data), Size); };
	const uint32 RiffSize = 36 + DataBytes, FormatSize = 16, SampleRate = CaptureSampleRate, ByteRate = CaptureSampleRate * 4;
	const uint16 PcmFormat = 1, Channels = 2, BlockAlign = 4, Bits = 16;
	const uint32 DataSize = DataBytes;
	Append("RIFF", 4); Append(&RiffSize, 4); Append("WAVEfmt ", 8); Append(&FormatSize, 4); Append(&PcmFormat, 2); Append(&Channels, 2);
	Append(&SampleRate, 4); Append(&ByteRate, 4); Append(&BlockAlign, 2); Append(&Bits, 2); Append("data", 4); Append(&DataSize, 4);
	Append(Samples.GetData(), DataBytes);
	FFileHelper::SaveArrayToFile(File, *Path);
}

void UCarAudioComponent::WriteCapture()
{
	IFileManager::Get().MakeDirectory(*CaptureDirectory, true);
	for (const FCaptureStem& Stem : CaptureStems)
	{
		WriteWav(FPaths::Combine(CaptureDirectory, Stem.Name + TEXT(".wav")), Stem.Samples);
	}

	FString Csv = TEXT("time,rpm,speed_kmh,gear,throttle,load,boost,squeal,running,cranking,clutch,load_fl,label\n");
	Csv += FString::Join(CaptureRows, TEXT("\n"));
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(CaptureDirectory, TEXT("telemetry.csv")));
	UE_LOG(LogCarAudio, Display, TEXT("Audio capture written to %s (%.1f s)"), *CaptureDirectory, CaptureTime);
	UE_LOG(LogCarAudio, Display, TEXT("AUDIOCOST game thread: %.1f us per frame on average, %.1f us worst (%d frames); synth on the audio thread: %.2f ms per second of sound (%.2f %% of one core)"),
		1e6 * TickCostTotalSeconds / FMath::Max(1, TickCostCount), 1e6 * TickCostMaxSeconds, TickCostCount,
		1000.0 * DspRenderSeconds / FMath::Max(CaptureTime, 1e-3), 100.0 * DspRenderSeconds / FMath::Max(CaptureTime, 1e-3));
}

// --- ambience ------------------------------------------------------------------------------------------------------

void UCarAudioComponent::UpdateAmbience(const FWeatherNow& Weather, float SpeedMps, float DeltaTime)
{
	const float Precipitation = SmoothedPrecipitation * (1.f - Weather.SnowFraction); // snow falls silently
	const float Heavy = SmoothRamp(Precipitation, 2.5f, 12.f);
	const float Light = SmoothRamp(Precipitation, 0.05f, 1.2f) * (1.f - Heavy);
	const float Day = SmoothRamp(Weather.SunAltitudeDegrees, -3.f, 8.f);
	const float Wind = SmoothRamp(Weather.WindMps, 1.5f, 14.f);
	const float Gusty = SmoothRamp(Weather.GustMps - Weather.WindMps, 1.5f, 8.f);
	const float Parked = 1.f - SmoothRamp(SpeedMps, 3.f, 14.f);
	const float RainMask = 1.f - 0.6f * SmoothRamp(Precipitation, 0.3f, 4.f);

	auto Set = [this](const TCHAR* Name, float Target)
	{
		if (FAmbienceLayer* Layer = FindLayer(Name))
		{
			Layer->TargetVolume = FMath::Clamp(Target, 0.f, 1.f);
		}
	};
	Set(TEXT("wind_trees_a"), Wind * (1.f - 0.7f * Gusty));
	Set(TEXT("wind_trees_b"), Wind * (0.3f + 0.7f * Gusty));
	Set(TEXT("town_day"), Day * RainMask);
	Set(TEXT("town_night"), (1.f - Day) * RainMask);
	// Birds: daylight, dry, calm air, and the car standing or creeping so the engine does not cover them.
	const float Birds = SmoothRamp(Weather.SunAltitudeDegrees, -1.f, 12.f) * (1.f - SmoothRamp(Precipitation, 0.f, 0.6f)) * (1.f - SmoothRamp(Weather.WindMps, 5.f, 11.f));
	Set(TEXT("birds_day_b"), Birds * (0.4f + 0.6f * Parked));
	Set(TEXT("birds_day_c"), Birds * 0.7f * (0.4f + 0.6f * Parked));
	Set(TEXT("rain_roof_light"), Light);
	Set(TEXT("rain_roof_heavy"), Heavy);
	Set(TEXT("rain_glass_light"), Light);
	Set(TEXT("rain_glass_heavy"), Heavy);
	Set(TEXT("rain_road_light"), Light * (0.6f + 0.4f * (1.f - Parked)));
	Set(TEXT("rain_road_heavy"), Heavy * (0.7f + 0.3f * (1.f - Parked)));

	const float Follow = 1.f - FMath::Exp(-DeltaTime / 1.5f);
	for (FAmbienceLayer& Layer : Layers)
	{
		Layer.Volume += (Layer.TargetVolume - Layer.Volume) * Follow;
		if (Layer.Audio)
		{
			Layer.Audio->SetVolumeMultiplier(Layer.Volume * Layer.MaximumVolume * AmbienceVolumeScale);
		}
	}
}

void UCarAudioComponent::TriggerThunder(float DistanceMeters)
{
	// Closer strikes crack and are louder; far ones are only a low roll, because air absorbs the highs.
	const bool bClose = DistanceMeters < 2500.f;
	const bool bMid = DistanceMeters < 7000.f;
	const TCHAR* Kind = bClose ? TEXT("close") : bMid ? TEXT("mid") : TEXT("far");
	const TCHAR* Variants = bMid && !bClose ? TEXT("bcd") : TEXT("ab"); // thunder_mid_a is a flat noise bed, not used
	const int32 VariantCount = FCString::Strlen(Variants);
	FPendingThunder Thunder;
	Thunder.AssetName = FString::Printf(TEXT("thunder_%s_%c"), Kind, Variants[Random.RandRange(0, VariantCount - 1)]);
	Thunder.SecondsLeft = DistanceMeters / SpeedOfSoundMps;
	Thunder.Volume = FMath::Clamp(1.1f - 0.9f * FMath::Sqrt(DistanceMeters / 12000.f), 0.15f, 1.f);
	Thunder.LowPassHz = FMath::Lerp(9000.f, 1500.f, FMath::Clamp(DistanceMeters / 10000.f, 0.f, 1.f));
	Thunder.Pitch = Random.FRandRange(0.94f, 1.06f);
	PendingThunder.Add(Thunder);
}

void UCarAudioComponent::HandleLightning(const FVector& WorldLocation, float Strength)
{
	const float Distance = FVector::Dist(WorldLocation, GetOwner()->GetActorLocation()) / 100.f;
	TriggerThunder(Distance);
	if (!PendingThunder.IsEmpty())
	{
		PendingThunder.Last().Volume = FMath::Clamp(PendingThunder.Last().Volume * FMath::Clamp(Strength, 0.3f, 1.5f), 0.1f, 1.f);
	}
}

void UCarAudioComponent::UpdateThunder(const FWeatherNow& Weather, float DeltaTime)
{
	if (Weather.ThunderActivity > 0.01f && !bLightningFromWeather)
	{
		NextThunderSeconds -= DeltaTime;
		if (NextThunderSeconds <= 0.f)
		{
			// A busy storm strikes every 15 to 40 seconds and sits closer; a weak one is rare and far.
			const float Closeness = FMath::Clamp(Weather.ThunderActivity, 0.f, 1.f);
			const float MeanGap = FMath::Lerp(120.f, 22.f, Closeness);
			NextThunderSeconds = MeanGap * Random.FRandRange(0.5f, 1.5f);
			const float Distance = FMath::Lerp(11000.f, 1200.f, Closeness) * FMath::Pow(Random.FRandRange(0.35f, 1.4f), 1.4f);
			TriggerThunder(Distance);
		}
	}
	for (int32 Index = PendingThunder.Num() - 1; Index >= 0; --Index)
	{
		PendingThunder[Index].SecondsLeft -= DeltaTime;
		if (PendingThunder[Index].SecondsLeft <= 0.f)
		{
			PlayThunder(PendingThunder[Index]);
			PendingThunder.RemoveAtSwap(Index);
		}
	}
}

void UCarAudioComponent::PlayThunder(const FPendingThunder& Thunder)
{
	if (!bAudioAvailable)
	{
		return;
	}
	USoundBase* Sound = LoadObject<USoundBase>(nullptr, *PathFor(OneShotFolder, Thunder.AssetName), nullptr, LOAD_NoWarn);
	if (!Sound)
	{
		UE_LOG(LogCarAudio, Warning, TEXT("Thunder %s missing; run Scripts/import_audio.py"), *Thunder.AssetName);
		return;
	}
	UAudioComponent* Audio = UGameplayStatics::CreateSound2D(GetWorld(), Sound, Thunder.Volume * AmbienceVolumeScale, Thunder.Pitch);
	if (Audio)
	{
		Audio->SetLowPassFilterEnabled(true);
		Audio->SetLowPassFilterFrequency(Thunder.LowPassHz);
		Audio->Play();
	}
}

// --- tick ----------------------------------------------------------------------------------------------------------

void UCarAudioComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const double TickStart = FPlatformTime::Seconds();
	ACarPawn* Car = GetCar();
	if (!Car || DeltaTime <= 0.f || (!bAudioAvailable && CaptureStems.IsEmpty()))
	{
		return;
	}
	// Only the car the player sits in makes sound; a parked second car stays silent.
	if (!Car->IsPlayerControlled())
	{
		return;
	}
	if (bAudioAvailable && Layers.IsEmpty())
	{
		CreateCarSynth();
		CreateAmbienceLayers();
		ApplyPreferences();
	}

	WeatherSampleCountdown -= DeltaTime;
	if (WeatherSampleCountdown <= 0.f)
	{
		WeatherSampleCountdown = WeatherSampleInterval;
		CachedWeather = SampleWeather();
	}
	SmoothedPrecipitation += (CachedWeather.PrecipitationMmPerHour - SmoothedPrecipitation) * (1.f - FMath::Exp(-DeltaTime / 2.f));

	UpdateSurface(DeltaTime);
	const FCarTelemetry Telemetry = Car->GetTelemetry();
	FeedCarSound(Telemetry, BuildEnvironment(CachedWeather), DeltaTime);
	if (bAudioAvailable)
	{
		UpdateAmbience(CachedWeather, FMath::Abs(Telemetry.LocalVelocityMps.X), DeltaTime);
	}
	UpdateThunder(CachedWeather, DeltaTime);

	const double TickSeconds = FPlatformTime::Seconds() - TickStart - CaptureRenderSecondsThisTick;
	TickCostTotalSeconds += TickSeconds;
	TickCostMaxSeconds = FMath::Max(TickCostMaxSeconds, TickSeconds);
	++TickCostCount;
	CaptureRenderSecondsThisTick = 0.0;
}
