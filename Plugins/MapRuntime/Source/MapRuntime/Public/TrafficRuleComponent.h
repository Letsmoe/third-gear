#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TrafficRuleComponent.generated.h"

class UTrafficSubsystem;

UENUM(BlueprintType)
enum class ETrafficViolationType : uint8
{
	/** The front of the vehicle crossed a stop line while its signal was red (or red and amber). */
	RedLight,
	/** Over the limit plus the enforcement tolerance for longer than a moment. */
	Speeding,
	/** Crossed on amber although the vehicle could have stopped comfortably; reported, not counted as a violation. */
	AmberRun,
};

USTRUCT(BlueprintType)
struct MAPRUNTIME_API FTrafficViolation
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	ETrafficViolationType Type = ETrafficViolationType::RedLight;

	/** Whether this counts against the driver. */
	UPROPERTY(BlueprintReadOnly)
	bool bCounts = true;

	/** Short text for the driver, e.g. "Red light (red for 3.2 s)". */
	UPROPERTY(BlueprintReadOnly)
	FString Message;

	UPROPERTY(BlueprintReadOnly)
	float SpeedKmh = 0.f;

	/** Speed limit in force; 0 for red light events. */
	UPROPERTY(BlueprintReadOnly)
	float LimitKmh = 0.f;

	/** Traffic time of the event, seconds. */
	UPROPERTY(BlueprintReadOnly)
	double TrafficTimeSeconds = 0.0;

	/** Approach id of the signal (red light events), else -1. */
	UPROPERTY(BlueprintReadOnly)
	int32 ApproachId = -1;

	UPROPERTY(BlueprintReadOnly)
	FVector Location = FVector::ZeroVector;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FTrafficViolationSignature, const FTrafficViolation&, Violation);

/**
 * Checks the rules of the road for the actor it is attached to: running a red light and speeding. Works for any
 * moving actor (the player's car now, AI cars later): it derives speed and heading from the actor's movement and
 * asks UTrafficSubsystem about the signal ahead and the speed limit.
 *
 * Red light: the vehicle's front crosses an approach's stop line while its signal shows red or red and amber. A
 * crossing within RedGraceSeconds of the change to red counts as a late amber. Crossing on amber is fair when the
 * vehicle could not stop: at the start of amber, stopping with ComfortDeceleration after ReactionSeconds had to fit
 * in the distance to the line; if it did, the crossing is reported as an AmberRun without counting.
 *
 * Speeding: German enforcement tolerance (3 km/h up to 100 km/h, 3 % above), and the speed must stay over it for
 * SpeedingMinSeconds. The limit has to stay the same for LimitDebounceSeconds before it applies, so a junction
 * where the nearest road changes does not flicker it. One report per episode.
 */
UCLASS(ClassGroup = (Traffic), meta = (BlueprintSpawnableComponent))
class MAPRUNTIME_API UTrafficRuleComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTrafficRuleComponent();

	/** Distance from the actor origin to the front bumper along its forward axis, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float FrontOffsetCm = 220.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float SpeedingToleranceKmh = 3.f;

	/** Tolerance as a fraction of the limit above 100 km/h. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float SpeedingToleranceFraction = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float SpeedingMinSeconds = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float LimitDebounceSeconds = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float RedGraceSeconds = 0.3f;

	/** m/s², what a driver is expected to brake with when a signal turns amber. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float ComfortDeceleration = 3.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rules")
	float ReactionSeconds = 0.7f;

	/** Called for every reported event, on the game thread. */
	UPROPERTY(BlueprintAssignable)
	FTrafficViolationSignature OnViolation;

	/** Everything reported so far. */
	const TArray<FTrafficViolation>& GetHistory() const { return History; }

	int32 GetViolationCount(ETrafficViolationType Type) const;

	/** Forgets everything reported and tracked so far (a test starting a new drive). */
	void ResetHistoryForTest();

	/** The speed limit currently applied (km/h; 0 none, negative before any limit was seen). */
	float GetActiveLimitKmh() const { return ActiveLimitKmh; }

	/** Speed measured from the actor's movement, km/h. */
	float GetSpeedKmh() const { return SpeedKmh; }

	/**
	 * Uses a speed the vehicle's own simulation knows instead of measuring the actor's movement. A kinematic car that is
	 * moved once per frame would otherwise read wrong whenever this component ticks at another rate than the car moves.
	 */
	void SetExternalSpeedKmh(float Kmh)
	{
		bUseExternalSpeed = true;
		ExternalSpeedKmh = Kmh;
	}

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void UpdateSignal(const FVector& Front, const FVector& Forward);
	void EvaluateCrossing(int32 ApproachId, const struct FSignalState& State);
	void UpdateLimit(const FVector& Front, const FVector& Forward, float DeltaTime);
	void UpdateSpeeding(float DeltaTime);
	float ToleranceFor(float LimitKmh) const;
	void Report(FTrafficViolation Violation);
	void ResetTracking();

	TWeakObjectPtr<UTrafficSubsystem> Traffic;
	TArray<FTrafficViolation> History;

	FVector PreviousLocation = FVector::ZeroVector;
	bool bHasPreviousLocation = false;
	float SpeedKmh = 0.f;
	bool bUseExternalSpeed = false;
	float ExternalSpeedKmh = 0.f;

	// Signal tracking
	int32 TrackedApproach = INDEX_NONE;
	float PreviousDistanceCm = 0.f;
	bool bAmberRecorded = false;
	float AmberDistanceCm = 0.f;
	float AmberSpeedKmh = 0.f;

	// Speed limit tracking
	float SecondsSinceLimitQuery = 1.f;
	float ActiveLimitKmh = -1.f;
	float CandidateLimitKmh = -1.f;
	float CandidateSeconds = 0.f;
	bool bSpeedingReported = false;
	float SecondsOverLimit = 0.f;
	float SecondsUnderLimit = 0.f;
	float PeakSpeedKmh = 0.f;
};
