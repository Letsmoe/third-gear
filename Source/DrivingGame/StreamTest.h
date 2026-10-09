#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "StreamTest.generated.h"

/**
 * Automated run through the generated world (started with -StreamTest, see Scripts/stream_test.sh): moves the
 * free camera along a straight line at driving speed, 2 m above the ground, while the world streams around it,
 * and records every frame time. Prints a "STREAMTEST" summary (average, percentiles, worst frame, hitches) and quits.
 *
 *   -StreamTest="x0,y0,x1,y1"   route in world metres
 *   -StreamSpeed=<km/h>         default 100
 */
UCLASS()
class DRIVINGGAME_API AStreamTestRunner : public AActor
{
	GENERATED_BODY()

public:
	AStreamTestRunner();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** Ground height below a point, or the previous height when there is no ground there yet. */
	double GroundHeightAt(const FVector2D& Point);

	/** Logs the frame time statistics and quits. */
	void Finish();

	FVector2D RouteStart = FVector2D::ZeroVector;
	FVector2D RouteEnd = FVector2D::ZeroVector;
	double SpeedCmPerSecond = 0.0;
	double Travelled = 0.0;
	double LastGroundZ = 0.0;
	float WarmupSeconds = 5.f;
	TArray<float> FrameMilliseconds;
};
