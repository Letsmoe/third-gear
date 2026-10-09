#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TrafficTest.generated.h"

/**
 * Headless run of AI traffic (-TrafficTest=<minutes>, see Scripts/traffic_test.sh). The free camera rides along random
 * lanes of the lane graph at about 45 km/h, so the traffic around it is spawned and removed as in a real drive, and
 * the runner reports every ten seconds and at the end: cars, spawns, violations of the rules by AI cars (red light,
 * speeding), collisions between AI cars, cars that stood still for a minute, and frame times. Then the game quits.
 */
UCLASS()
class DRIVINGGAME_API ATrafficTestRunner : public AActor
{
	GENERATED_BODY()

public:
	ATrafficTestRunner();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** Picks the lane the camera starts on and its position on it. */
	bool ChooseStart();

	/** Moves the camera along the lane graph by DeltaSeconds, taking a random good continuation at every lane end. */
	void MoveViewer(float DeltaSeconds);

	void ReportProgress();
	void Finish();

	float DurationSeconds = 600.f;
	float WarmupSeconds = 10.f;
	float RunSeconds = 0.f;
	float SecondsSinceReport = 0.f;
	float ViewerSpeedMs = 12.5f;
	bool bViewerMoves = true;
	int32 ViewerLane = INDEX_NONE;
	float ViewerS = 0.f;
	FRandomStream Random;

	TArray<float> FrameMilliseconds;
	double LastFrameStartSeconds = 0.0;
	int32 CarSamples = 0;
	int64 CarSampleSum = 0;
	int32 MaxCars = 0;
};
