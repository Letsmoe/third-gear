#pragma once

#include "CoreMinimal.h"
#include "DriveTest.h"
#include "AudioTest.generated.h"

/**
 * Scripted drive for recording what the car sounds like (started with -AudioTest, see Scripts/audio_test.sh): engine
 * off, starter, idle, a blip, a full-throttle run through the gears, the rev limiter, coasting, hard braking into a
 * stall, cruising on asphalt, cobbles, pavers and wet road, cornering to the limit, gear clunks, handbrake and indicator.
 * Runs on the DriveTest machinery; every scene labels the audio capture (-AudioCapture=<dir>) so a recording can be cut.
 */
UCLASS()
class DRIVINGGAME_API AAudioTestRunner : public ADriveTestRunner
{
	GENERATED_BODY()

protected:
	virtual void BuildSteps() override;

private:
	/** A timed scene: Setup runs once, Drive every physics step; it ends after Seconds. */
	void AddScene(const FString& Label, float Seconds, TFunction<void()> Setup, TFunction<void(const FCarTelemetry&)> Drive);

	/** Pull away and accelerate to TargetKmh in third gear, then hold it. */
	void CruiseTo(const FCarTelemetry& T, float TargetKmh);

	void SetLabel(const FString& Label);
	class UCarAudioComponent* GetAudio() const;
	void AddCruiseScenes(float StartX);
	void AddAmbienceScenes(float StartX);
	/** -RadioTest (with -RadioStation=<n>): the first station, the next one, radio off, radio on again. */
	void AddRadioScenes(float StartX);
	void AddWeatherScene(const FString& Label, float Seconds, float RainMmPerHour, float WindMps, float ThunderActivity, float SunAltitude,
		TFunction<void(float)> Extra);
	void AddSurfaceProbes(const FString& Points);
	void ProbeSurfaceAt(const FVector2D& PointMeters);
	void AddMechanicalScenes(float StartX);
};
