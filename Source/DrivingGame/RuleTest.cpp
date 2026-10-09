#include "RuleTest.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TrafficSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogRuleTest, Log, All);

namespace
{
constexpr float ApproachDistanceCm = 6000.f;
constexpr float CruiseKmh = 50.f;
constexpr float StopBeforeLineCm = 300.f;
constexpr float BrakeDecelerationMs2 = 3.f;
constexpr float RestartAccelerationMs2 = 2.f;
constexpr float SpeedingRunSeconds = 5.f;
constexpr float SpeedingLeadInCm = 1500.f;
constexpr float MinimumSpeedingSegmentCm = 14000.f;
constexpr float IsolationDistanceCm = 3500.f;

/** First traffic time at or after From where the approach shows Aspect for at least MinSeconds of it, with MinSeconds already elapsed. */
bool FindTime(const FTrafficNetwork& Network, int32 ApproachId, ESignalAspect Aspect, float MinSecondsInState, float MaxSecondsInState,
	double& OutTime)
{
	for (double Time = 0.0; Time < 400.0; Time += 0.05)
	{
		const FSignalState State = Network.GetApproachState(ApproachId, Time);
		if (State.Aspect == Aspect && State.SecondsInState >= MinSecondsInState && State.SecondsInState <= MaxSecondsInState)
		{
			OutTime = Time;
			return true;
		}
	}
	return false;
}

float KmhToCmPerSecond(float Kmh)
{
	return Kmh / 3.6f * 100.f;
}
}

ARuleTestRunner::ARuleTestRunner()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void ARuleTestRunner::BeginPlay()
{
	Super::BeginPlay();
	Probe = GetWorld()->SpawnActor<AActor>();
	USceneComponent* Root = NewObject<USceneComponent>(Probe, TEXT("Root"));
	Probe->SetRootComponent(Root);
	Root->RegisterComponent();
	Rules = NewObject<UTrafficRuleComponent>(Probe, TEXT("Rules"));
	Rules->FrontOffsetCm = 0.f;
	Rules->RegisterComponent();
}

void ARuleTestRunner::AddSignalCases(const UTrafficSubsystem& Traffic)
{
	const FTrafficNetwork& Network = Traffic.GetNetwork();
	// Use a junction with several phases so every aspect occurs on its approaches.
	const FTrafficApproach* Approach = nullptr;
	for (const FTrafficApproach& Candidate : Network.GetApproaches())
	{
		const FSignalJunction& Junction = Network.GetJunctions()[Candidate.JunctionIndex];
		if (!Junction.bCrossingOnly && Junction.Phases.Num() >= 2 && Junction.CycleSeconds > 40.f)
		{
			Approach = &Candidate;
			break;
		}
	}
	if (!Approach)
	{
		UE_LOG(LogRuleTest, Error, TEXT("RULETEST no signalised junction in the region"));
		return;
	}
	const FVector2D LineCenter = Approach->StopLineCenter();
	const FVector2D Start = LineCenter - Approach->Direction * ApproachDistanceCm;
	const float ArrivalSeconds = ApproachDistanceCm / KmhToCmPerSecond(CruiseKmh);
	const float TotalSeconds = ArrivalSeconds + 4.f;
	const int32 Id = Approach->Id;
	const auto AddPass = [&](const FString& Name, ESignalAspect Aspect, float MinInState, float MaxInState, int32 Red, int32 Amber)
	{
		double CrossingTime = 0.0;
		if (!FindTime(Network, Id, Aspect, MinInState, MaxInState, CrossingTime))
		{
			UE_LOG(LogRuleTest, Error, TEXT("RULETEST %s: no suitable moment in the cycle of approach %d"), *Name, Id);
			return;
		}
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = Name;
		Case.Start = Start;
		Case.Direction = Approach->Direction;
		Case.StartTrafficTime = CrossingTime - ArrivalSeconds;
		Case.DurationSeconds = TotalSeconds;
		Case.Speed = [](float, float, const UTrafficSubsystem&) { return CruiseKmh; };
		Case.ExpectedRedLight = Red;
		Case.ExpectedAmberRun = Amber;
	};
	AddPass(TEXT("red light at 50 km/h, red for 4 s"), ESignalAspect::Red, 4.f, 8.f, 1, 0);
	AddPass(TEXT("green light at 50 km/h"), ESignalAspect::Green, 3.f, 6.f, 0, 0);
	AddPass(TEXT("amber, 1 s after the change (could not stop)"), ESignalAspect::Amber, 0.9f, 1.1f, 0, 0);
	AddPass(TEXT("amber, 2.9 s after the change (could have stopped)"), ESignalAspect::Amber, 2.8f, 2.95f, 0, 1);
	AddPass(TEXT("red 0.1 s after the change (late amber)"), ESignalAspect::Red, 0.05f, 0.15f, 0, 0);
	AddPass(TEXT("red and amber"), ESignalAspect::RedAmber, 0.3f, 0.7f, 1, 0);
}

void ARuleTestRunner::AddStopAtRedCase(const UTrafficSubsystem& Traffic)
{
	const FTrafficNetwork& Network = Traffic.GetNetwork();
	if (Cases.IsEmpty())
	{
		return;
	}
	const FTrafficApproach* Approach = nullptr;
	for (const FTrafficApproach& Candidate : Network.GetApproaches())
	{
		if (FVector2D::Distance(Candidate.StopLineCenter() - Candidate.Direction * ApproachDistanceCm, Cases[0].Start) < 1.0)
		{
			Approach = &Candidate;
			break;
		}
	}
	if (!Approach)
	{
		return;
	}
	double RedTime = 0.0;
	if (!FindTime(Network, Approach->Id, ESignalAspect::Red, 4.f, 6.f, RedTime))
	{
		return;
	}
	const FVector2D LineCenter = Approach->StopLineCenter();
	FCase& Case = Cases.AddDefaulted_GetRef();
	Case.Name = TEXT("stops at red, goes on at green");
	Case.Start = LineCenter - Approach->Direction * ApproachDistanceCm;
	Case.Direction = Approach->Direction;
	Case.StartTrafficTime = RedTime - ApproachDistanceCm / KmhToCmPerSecond(CruiseKmh);
	Case.DurationSeconds = 150.f;
	const int32 Id = Approach->Id;
	const FVector2D Direction = Approach->Direction;
	Case.Speed = [Id](float, float DrivenCm, const UTrafficSubsystem& Subsystem)
	{
		const FSignalState State = Subsystem.GetNetwork().GetApproachState(Id, Subsystem.GetTrafficTime());
		const float RemainingCm = ApproachDistanceCm - StopBeforeLineCm - DrivenCm;
		if (State.Aspect == ESignalAspect::Green && DrivenCm >= ApproachDistanceCm - StopBeforeLineCm - 50.f)
		{
			return 30.f; // go on once it is green
		}
		const float BrakingKmh = FMath::Sqrt(FMath::Max(0.f, 2.f * BrakeDecelerationMs2 * RemainingCm * 0.01f)) * 3.6f;
		return FMath::Min(CruiseKmh, BrakingKmh);
	};
	(void)RestartAccelerationMs2;
	(void)Direction;
}

void ARuleTestRunner::AddSpeedingCases(const UTrafficSubsystem& Traffic)
{
	const FTrafficNetwork& Network = Traffic.GetNetwork();
	const TArray<FSpeedWay>& Ways = Network.GetWays();
	// A long straight piece of a road with a plain 50 or 30 limit and no other road close by.
	for (const FSpeedWay& Way : Ways)
	{
		if (Way.bOneway || !(FMath::IsNearlyEqual(Way.LimitKmh, 50.f) || FMath::IsNearlyEqual(Way.LimitKmh, 30.f)))
		{
			continue;
		}
		for (int32 Index = 0; Index + 1 < Way.Points.Num(); ++Index)
		{
			const FVector2D A = Way.Points[Index];
			const FVector2D B = Way.Points[Index + 1];
			if (FVector2D::Distance(A, B) < MinimumSpeedingSegmentCm)
			{
				continue;
			}
			const FVector2D Direction = (B - A).GetSafeNormal();
			const FVector2D From = A + Direction * SpeedingLeadInCm;
			bool bIsolated = true;
			for (const FSpeedWay& Other : Ways)
			{
				if (Other.Id == Way.Id)
				{
					continue;
				}
				for (const FVector2D& Point : Other.Points)
				{
					if (FVector2D::Distance(Point, From) < IsolationDistanceCm || FVector2D::Distance(Point, B) < IsolationDistanceCm * 0.5f)
					{
						bIsolated = false;
						break;
					}
				}
				if (!bIsolated)
				{
					break;
				}
			}
			if (!bIsolated)
			{
				continue;
			}
			const float Limit = Way.LimitKmh;
			const auto AddRun = [&](const FString& Label, float SpeedKmh, int32 ExpectedSpeeding)
			{
				FCase& Case = Cases.AddDefaulted_GetRef();
				Case.Name = FString::Printf(TEXT("%s: %.0f km/h on a %.0f road"), *Label, SpeedKmh, Limit);
				Case.Start = From;
				Case.Direction = Direction;
				Case.StartTrafficTime = 0.0;
				Case.DurationSeconds = SpeedingRunSeconds;
				Case.Speed = [SpeedKmh](float, float, const UTrafficSubsystem&) { return SpeedKmh; };
				Case.ExpectedSpeeding = ExpectedSpeeding;
			};
			AddRun(TEXT("over the limit"), Limit + 12.f, 1);
			AddRun(TEXT("inside the tolerance"), Limit + 2.f, 0);
			AddRun(TEXT("just over the tolerance"), Limit + 4.f, 1);
			AddRun(TEXT("at the limit"), Limit, 0);
			return;
		}
	}
	UE_LOG(LogRuleTest, Error, TEXT("RULETEST no isolated straight road for the speeding cases"));
}

bool ARuleTestRunner::BuildCases(const UTrafficSubsystem& Traffic)
{
	AddSignalCases(Traffic);
	AddStopAtRedCase(Traffic);
	AddSpeedingCases(Traffic);
	return !Cases.IsEmpty();
}

void ARuleTestRunner::StartCase()
{
	++CaseIndex;
	CaseTime = 0.f;
	DistanceDrivenCm = 0.f;
	if (!Cases.IsValidIndex(CaseIndex))
	{
		Finish();
		return;
	}
	UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	const FCase& Case = Cases[CaseIndex];
	Traffic->SetTimeOffsetSeconds(Case.StartTrafficTime - GetWorld()->GetTimeSeconds());
	Rules->ResetHistoryForTest();
	Probe->SetActorLocation(FVector(Case.Start.X, Case.Start.Y, 500.0), false, nullptr, ETeleportType::TeleportPhysics);
	Probe->SetActorRotation(FRotator(0.f, FMath::RadiansToDegrees(FMath::Atan2(Case.Direction.Y, Case.Direction.X)), 0.f));
}

void ARuleTestRunner::FinishCase()
{
	const FCase& Case = Cases[CaseIndex];
	const int32 Red = Rules->GetViolationCount(ETrafficViolationType::RedLight);
	const int32 Speeding = Rules->GetViolationCount(ETrafficViolationType::Speeding);
	int32 Amber = 0;
	for (const FTrafficViolation& Event : Rules->GetHistory())
	{
		Amber += Event.Type == ETrafficViolationType::AmberRun ? 1 : 0;
	}
	const bool bPass = Red == Case.ExpectedRedLight && Speeding == Case.ExpectedSpeeding && Amber == Case.ExpectedAmberRun;
	Passed += bPass ? 1 : 0;
	UE_LOG(LogRuleTest, Display, TEXT("RULETEST %s: %s (red light %d/%d, speeding %d/%d, amber run %d/%d reported/expected)"),
		bPass ? TEXT("PASS") : TEXT("FAIL"), *Case.Name, Red, Case.ExpectedRedLight, Speeding, Case.ExpectedSpeeding, Amber, Case.ExpectedAmberRun);
}

void ARuleTestRunner::Finish()
{
	UE_LOG(LogRuleTest, Display, TEXT("RULETEST summary: %d of %d cases passed"), Passed, Cases.Num());
	GEngine->Exec(GetWorld(), TEXT("quit"));
}

void ARuleTestRunner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	if (!Traffic || !Traffic->GetNetwork().IsLoaded() || !Rules)
	{
		return;
	}
	if (CaseIndex == INDEX_NONE)
	{
		WarmupSeconds -= DeltaSeconds; // the streamer loads traffic.json when it starts
		if (WarmupSeconds > 0.f)
		{
			return;
		}
		if (!BuildCases(*Traffic))
		{
			Finish();
			return;
		}
		StartCase();
		return;
	}
	const FCase& Case = Cases[CaseIndex];
	CaseTime += DeltaSeconds;
	const float SpeedKmh = Case.Speed(CaseTime, DistanceDrivenCm, *Traffic);
	DistanceDrivenCm += KmhToCmPerSecond(SpeedKmh) * DeltaSeconds;
	const FVector2D Position = Case.Start + Case.Direction * DistanceDrivenCm;
	Probe->SetActorLocation(FVector(Position.X, Position.Y, 500.0), false, nullptr, ETeleportType::TeleportPhysics);
	// The case ends after its duration; the stop-at-red drive ends once it is clear of the line again.
	const bool bPassedLine = DistanceDrivenCm > ApproachDistanceCm + 1200.f;
	if (CaseTime >= Case.DurationSeconds || (Case.DurationSeconds > 100.f && bPassedLine))
	{
		FinishCase();
		StartCase();
	}
}
