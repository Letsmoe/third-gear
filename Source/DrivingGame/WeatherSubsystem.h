#pragma once

#include "CoreMinimal.h"
#include "IsobarPointWeather.h"
#include "IsobarWeather.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeatherSubsystem.generated.h"

/**
 * The weather over the current map, from the shared Isobar plugin (issue #27).
 *
 * Built at begin play with the temperate maritime (Hamburg) climate, placed at
 * the project's map origin in Bergedorf. The ground is level for now: the map
 * pipeline has no runtime elevation data to hand Isobar until the world data tiles
 * of #23 exist, and Bergedorf is flat enough that the terrain would change little.
 *
 * Owns the weather clock. It starts at a day and hour from the command line
 * (`-WeatherDay=` 1-365, `-WeatherHour=`, defaults midsummer noon), advances with
 * game time times `-WeatherTimeScale=`, and is seeded by `-WeatherSeed=`. Nothing
 * is drawn yet; rain, sky and road surfaces read from here later (#15, #8, #25). `Weather.SetTime <day> <hour>`
 * restarts it at another time, for screenshots of several times of day in one run.
 */
UCLASS()
class DRIVINGGAME_API UWeatherSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Only game worlds get weather; the editor's own world does not. */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The weather at a point in the world, in Unreal coordinates. False before begin play. */
	bool SampleAt(const FVector& WorldLocation, FIsobarPointWeather& OutWeather) const;

	/** Moves the weather clock forward, running every weather step on the way. */
	void SkipHours(double Hours);

	/** Starts the weather afresh at a 1-based day of the year and an hour, earlier or later than now. */
	void RestartAt(int32 DayOfYear, double Hour);

	/** Weather time in seconds since the first of January, 00:00 local solar time. */
	double GetWeatherSeconds() const;

	/** Where the player is: the car, or the camera when there is no pawn. */
	bool FindViewerLocation(FVector& OutLocation) const;

private:
	/** Creates the Isobar weather starting at the given weather seconds. */
	void StartWeather(double StartSeconds);

	/** Brings the terrain tiles around the viewer in and lets distant ones go. */
	void KeepTerrainAroundViewer();

	TUniquePtr<FIsobarWeather> Weather;
	double WeatherSeconds = 0.0;
	double TimeScale = 1.0;
};
