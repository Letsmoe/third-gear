#include "TrafficRuleComponent.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TrafficSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogTrafficRules, Log, All);

namespace
{
constexpr float LimitQueryIntervalSeconds = 0.2f;
constexpr float SpeedingClearSeconds = 0.5f;
/** A jump longer than this between two ticks is a teleport, not driving. */
constexpr float TeleportDistanceCm = 5000.f;
constexpr float SignalLookAheadCm = 9000.f;
constexpr float SignalLookBehindCm = 500.f;
}

UTrafficRuleComponent::UTrafficRuleComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

int32 UTrafficRuleComponent::GetViolationCount(ETrafficViolationType Type) const
{
	int32 Count = 0;
	for (const FTrafficViolation& Violation : History)
	{
		Count += (Violation.Type == Type && Violation.bCounts) ? 1 : 0;
	}
	return Count;
}

void UTrafficRuleComponent::ResetHistoryForTest()
{
	History.Reset();
	ResetTracking();
	bHasPreviousLocation = false;
	SpeedKmh = 0.f;
	ActiveLimitKmh = -1.f;
	CandidateLimitKmh = -1.f;
	CandidateSeconds = 0.f;
	bSpeedingReported = false;
	SecondsOverLimit = 0.f;
	SecondsUnderLimit = 0.f;
	PeakSpeedKmh = 0.f;
}

void UTrafficRuleComponent::ResetTracking()
{
	TrackedApproach = INDEX_NONE;
	bAmberRecorded = false;
}

float UTrafficRuleComponent::ToleranceFor(float LimitKmh) const
{
	return LimitKmh <= 100.f ? SpeedingToleranceKmh : LimitKmh * SpeedingToleranceFraction;
}

void UTrafficRuleComponent::Report(FTrafficViolation Violation)
{
	History.Add(Violation);
	UE_LOG(LogTrafficRules, Display, TEXT("RULECHECK %s: %s (speed %.0f km/h, limit %.0f, traffic time %.1f s, counts %d) at %s"),
		*StaticEnum<ETrafficViolationType>()->GetNameStringByValue(int64(Violation.Type)), *Violation.Message, Violation.SpeedKmh,
		Violation.LimitKmh, Violation.TrafficTimeSeconds, Violation.bCounts ? 1 : 0, *Violation.Location.ToString());
	OnViolation.Broadcast(Violation);
}

void UTrafficRuleComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AActor* Owner = GetOwner();
	if (!Owner || DeltaTime <= 1e-5f)
	{
		return;
	}
	if (!Traffic.IsValid())
	{
		Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	}
	if (!Traffic.IsValid() || !Traffic->GetNetwork().IsLoaded())
	{
		return;
	}
	const FVector Location = Owner->GetActorLocation();
	const FVector Forward = FVector(Owner->GetActorForwardVector().X, Owner->GetActorForwardVector().Y, 0.f).GetSafeNormal();
	if (bHasPreviousLocation)
	{
		const float Moved = float(FVector::Dist2D(Location, PreviousLocation));
		if (Moved > TeleportDistanceCm)
		{
			ResetTracking();
			SpeedKmh = 0.f;
		}
		else
		{
			SpeedKmh = Moved / DeltaTime * 0.036f;
		}
	}
	PreviousLocation = Location;
	bHasPreviousLocation = true;

	const FVector Front = Location + Forward * FrontOffsetCm;
	UpdateSignal(Front, Forward);
	UpdateLimit(Front, Forward, DeltaTime);
	UpdateSpeeding(DeltaTime);
}

void UTrafficRuleComponent::UpdateSignal(const FVector& Front, const FVector& Forward)
{
	FApproachQuery Query;
	if (!Traffic->FindSignalAhead(Front, Forward, Query, SignalLookAheadCm, SignalLookBehindCm, TrackedApproach))
	{
		ResetTracking();
		return;
	}
	if (Query.ApproachId != TrackedApproach)
	{
		ResetTracking();
		TrackedApproach = Query.ApproachId;
		PreviousDistanceCm = Query.DistanceToStopLineCm;
	}
	const ESignalAspect Aspect = Query.State.Aspect;
	if (Aspect == ESignalAspect::Green)
	{
		bAmberRecorded = false;
	}
	else if (Aspect == ESignalAspect::Amber && !bAmberRecorded)
	{
		// The first moment the vehicle sees amber decides whether it could have stopped.
		bAmberRecorded = true;
		AmberDistanceCm = Query.DistanceToStopLineCm;
		AmberSpeedKmh = SpeedKmh;
	}
	if (PreviousDistanceCm >= 0.f && Query.DistanceToStopLineCm < 0.f)
	{
		EvaluateCrossing(Query.ApproachId, Query.State);
	}
	PreviousDistanceCm = Query.DistanceToStopLineCm;
}

void UTrafficRuleComponent::EvaluateCrossing(int32 ApproachId, const FSignalState& State)
{
	FTrafficViolation Violation;
	Violation.ApproachId = ApproachId;
	Violation.SpeedKmh = SpeedKmh;
	Violation.TrafficTimeSeconds = Traffic->GetTrafficTime();
	Violation.Location = GetOwner()->GetActorLocation();
	switch (State.Aspect)
	{
	case ESignalAspect::Green:
		return;
	case ESignalAspect::Amber:
	{
		const float Speed = AmberSpeedKmh / 3.6f;
		const float StoppingMetres = Speed * ReactionSeconds + Speed * Speed / (2.f * ComfortDeceleration);
		if (!bAmberRecorded || AmberDistanceCm * 0.01f < StoppingMetres)
		{
			return; // could not have stopped: fair to go through
		}
		Violation.Type = ETrafficViolationType::AmberRun;
		Violation.bCounts = false;
		Violation.Message = FString::Printf(TEXT("Amber light (could have stopped: %.0f m to the line at %.0f km/h)"), AmberDistanceCm * 0.01f, AmberSpeedKmh);
		break;
	}
	case ESignalAspect::Red:
		if (State.SecondsInState <= RedGraceSeconds)
		{
			return; // changed to red a moment ago; the same as a late amber
		}
		Violation.Type = ETrafficViolationType::RedLight;
		Violation.Message = FString::Printf(TEXT("Red light (red for %.1f s)"), State.SecondsInState);
		break;
	case ESignalAspect::RedAmber:
		Violation.Type = ETrafficViolationType::RedLight;
		Violation.Message = TEXT("Red light (red and amber)");
		break;
	}
	Report(Violation);
}

void UTrafficRuleComponent::UpdateLimit(const FVector& Front, const FVector& Forward, float DeltaTime)
{
	SecondsSinceLimitQuery += DeltaTime;
	CandidateSeconds += DeltaTime;
	if (SecondsSinceLimitQuery < LimitQueryIntervalSeconds)
	{
		return;
	}
	SecondsSinceLimitQuery = 0.f;
	const FSpeedLimitResult Result = Traffic->GetSpeedLimit(Front, Forward);
	if (!Result.bFound)
	{
		return;
	}
	if (!FMath::IsNearlyEqual(Result.LimitKmh, CandidateLimitKmh))
	{
		CandidateLimitKmh = Result.LimitKmh;
		CandidateSeconds = 0.f;
	}
	if (ActiveLimitKmh < 0.f || CandidateSeconds >= LimitDebounceSeconds)
	{
		ActiveLimitKmh = CandidateLimitKmh;
	}
}

void UTrafficRuleComponent::UpdateSpeeding(float DeltaTime)
{
	const bool bOver = ActiveLimitKmh > 0.f && SpeedKmh > ActiveLimitKmh + ToleranceFor(ActiveLimitKmh);
	if (bOver)
	{
		SecondsOverLimit += DeltaTime;
		SecondsUnderLimit = 0.f;
		PeakSpeedKmh = FMath::Max(PeakSpeedKmh, SpeedKmh);
		if (!bSpeedingReported && SecondsOverLimit >= SpeedingMinSeconds)
		{
			bSpeedingReported = true;
			FTrafficViolation Violation;
			Violation.Type = ETrafficViolationType::Speeding;
			Violation.SpeedKmh = SpeedKmh;
			Violation.LimitKmh = ActiveLimitKmh;
			Violation.TrafficTimeSeconds = Traffic->GetTrafficTime();
			Violation.Location = GetOwner()->GetActorLocation();
			Violation.Message = FString::Printf(TEXT("Speeding: %.0f in a %.0f zone"), SpeedKmh, ActiveLimitKmh);
			Report(Violation);
		}
		return;
	}
	SecondsUnderLimit += DeltaTime;
	if (SecondsUnderLimit >= SpeedingClearSeconds)
	{
		if (bSpeedingReported)
		{
			UE_LOG(LogTrafficRules, Display, TEXT("RULECHECK speeding episode ended, peak %.0f km/h"), PeakSpeedKmh);
		}
		bSpeedingReported = false;
		SecondsOverLimit = 0.f;
		PeakSpeedKmh = 0.f;
	}
}
