#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CarSimTypes.h"
#include "CarSurfaceGrip.h"
#include "DriveTest.generated.h"

class ACarPawn;

/**
 * Automated, headless drivetrain/handling test on the flat ProvingGround map (started with -DriveTest, see
 * Scripts/drive_test.sh). Drives the car with scripted pedals/gears/steering like a test driver would:
 * idle, full-throttle run with gear changes (0-50, 0-100, top speed), 50 km/h cruise in each gear, gentle and idle
 * clutch starts, a deliberate stall, 100-0 braking and a ramp-steer test for maximum lateral acceleration.
 * Times are physics-simulation time. Results go to the log as "DRIVETEST" lines, then the game quits.
 */
UCLASS()
class DRIVINGGAME_API ADriveTestRunner : public AActor
{
	GENERATED_BODY()

public:
	ADriveTestRunner();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	struct FStep
	{
		FString Name;
		TFunction<void()> Start;
		/** Returns true when finished. */
		TFunction<bool(const FCarTelemetry&)> Update;
	};

	virtual void BuildSteps();
	void AddStep(const FString& Name, TFunction<void()> Start, TFunction<bool(const FCarTelemetry&)> Update);
	void AddPlace(float XM, float YM, float Yaw, bool bEngineRunning, float SettleSeconds = 1.5f);
	void AddWait(float Seconds);
	void Report(const FString& Line);

	/** Brakes from FromKmh with full pedal and reports the distance. The test surface starts at the brake point. */
	void AddBrakingStep(float FromKmh);
	/** Switches between the test surface (-DriveTestSurface=wet|flood|snow|ice) and dry asphalt. No-op without the switch. */
	void ApplyTestSurface(bool bOn);
	/** With -DriveTestSurface only 0-50, braking and ramp steer run (the other steps say nothing about grip). */
	void KeepSurfaceSteps();

	/** Keeps the car on the line y = LaneY (heading +X). */
	void SteerToLane(const FCarTelemetry& T);
	/** Full-throttle driving with clutch launch and upshifts at ShiftRpm. Returns current phase description. */
	void DriveFlatOut(const FCarTelemetry& T, int32 MaxGear);
	/** Throttle PI to hold a speed in the current gear. */
	void HoldSpeed(const FCarTelemetry& T, float TargetKmh);
	/** Shift to a gear: clutch in, select, clutch out (over ClutchOutSeconds). Returns true when complete. */
	bool ShiftTo(const FCarTelemetry& T, int32 Gear, float ClutchOutSeconds, float ThrottleAfter);
	float StepTime(const FCarTelemetry& T) const { return float(T.SimTime - StepStartTime); }

	TWeakObjectPtr<ACarPawn> Car;
	TArray<FStep> Steps;
	int32 StepIndex = -1;
	double StepStartTime = 0.0;
	bool bStepStarted = false;
	FCarDriverInput Input;
	FCarTelemetry Prev;
	TArray<FString> Summary;

	// Per-test scratch state
	TOptional<FCarSurfaceConditions> TestSurface;
	FString TestSurfaceName;
	float LaneY = 0.f;
	float ShiftRpm = 6200.f;
	float LaunchRpm = 3000.f;
	int32 ShiftPhase = 0;          // 0 = driving, 1 = shifting
	double ShiftStart = 0.0;
	int32 ShiftTarget = 0;
	double MarkTime = 0.0;
	FVector MarkPosition = FVector::ZeroVector;
	float MaxValue = 0.f;
	float MinValue = 0.f;
	float Value2 = 0.f;
	float SpeedIntegral = 0.f;
	bool bFlag = false;
	int32 Counter = 0;
	int32 Samples = 0;
	float T160 = -1.f;
	double QuitAt = 0.0;
};
