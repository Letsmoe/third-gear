#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TrafficRuleComponent.h"
#include "RuleTest.generated.h"

class UTrafficSubsystem;

/**
 * Headless check of the rule checker (-RuleTest, see Scripts/rule_test.sh). A probe actor with a UTrafficRuleComponent
 * is moved along real stop lines and roads of the loaded region at scripted speeds: through a signal on red, on
 * green, on amber where stopping was and was not possible, stopping at red and going on at green, and along a road
 * over, at and just under the speed limit plus tolerance. Each case prints a "RULETEST" line with what was expected
 * and what the checker reported, then a summary, and the game quits.
 */
UCLASS()
class DRIVINGGAME_API ARuleTestRunner : public AActor
{
	GENERATED_BODY()

public:
	ARuleTestRunner();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** One scripted drive: a start, a heading, a speed rule and what the checker should say. */
	struct FCase
	{
		FString Name;
		FVector2D Start = FVector2D::ZeroVector;
		FVector2D Direction = FVector2D::ZeroVector;
		/** Traffic time at the moment the probe starts moving. */
		double StartTrafficTime = 0.0;
		float DurationSeconds = 0.f;
		/** Speed in km/h from the case time, the distance driven (cm) and the traffic subsystem. */
		TFunction<float(float, float, const UTrafficSubsystem&)> Speed;
		int32 ExpectedRedLight = 0;
		int32 ExpectedSpeeding = 0;
		int32 ExpectedAmberRun = 0;
	};

	bool BuildCases(const UTrafficSubsystem& Traffic);
	void AddSignalCases(const UTrafficSubsystem& Traffic);
	void AddStopAtRedCase(const UTrafficSubsystem& Traffic);
	void AddSpeedingCases(const UTrafficSubsystem& Traffic);
	void StartCase();
	void FinishCase();
	void Finish();

	TArray<FCase> Cases;
	int32 CaseIndex = INDEX_NONE;
	float CaseTime = 0.f;
	float DistanceDrivenCm = 0.f;
	int32 Passed = 0;
	float PreviousSpeedKmh = 0.f;
	float WarmupSeconds = 4.f;

	UPROPERTY(Transient)
	TObjectPtr<AActor> Probe;
	UPROPERTY(Transient)
	TObjectPtr<UTrafficRuleComponent> Rules;
};
