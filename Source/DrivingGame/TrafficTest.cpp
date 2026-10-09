#include "TrafficTest.h"

#include "AITrafficSubsystem.h"
#include "Engine/Engine.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "WorldStreamer.h"

DEFINE_LOG_CATEGORY_STATIC(LogTrafficTest, Log, All);

namespace
{
constexpr float TrafficTestReportIntervalSeconds = 10.f;
constexpr float TrafficTestCameraHeightCm = 300.f;
}

ATrafficTestRunner::ATrafficTestRunner()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void ATrafficTestRunner::BeginPlay()
{
	Super::BeginPlay();
	float Minutes = 10.f;
	FParse::Value(FCommandLine::Get(), TEXT("TrafficTest="), Minutes);
	DurationSeconds = Minutes * 60.f;
	bBlocker = FParse::Param(FCommandLine::Get(), TEXT("TrafficTestBlocker"));
	bViewerMoves = !FParse::Param(FCommandLine::Get(), TEXT("TrafficTestStatic")) && !bBlocker;
	int32 Seed = 7;
	FParse::Value(FCommandLine::Get(), TEXT("TrafficSeed="), Seed);
	Random.Initialize(Seed);
	UE_LOG(LogTrafficTest, Display, TEXT("TRAFFICTEST %.1f minutes, camera %s"), Minutes, bViewerMoves ? TEXT("rides the lanes") : TEXT("stays at the start"));
}

bool ATrafficTestRunner::ChooseStart()
{
	UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	if (!Traffic || !Traffic->IsRunning())
	{
		return false;
	}
	const TArray<int32>& SpawnLanes = Traffic->GetLaneNetwork().GetSpawnLanes();
	if (SpawnLanes.IsEmpty())
	{
		return false;
	}
	ViewerLane = SpawnLanes[Random.RandHelper(SpawnLanes.Num())];
	ViewerS = 0.f;
	for (int32 Attempt = 0; bBlocker && Attempt < 500; ++Attempt)
	{
		// The stand-in for the player goes on a long stretch of a main road, where traffic is sure to come by.
		const FTrafficLane& Candidate = Traffic->GetLaneNetwork().GetLane(SpawnLanes[Random.RandHelper(SpawnLanes.Num())]);
		if (Candidate.Tier >= 3 && Candidate.Length() > 120.f)
		{
			ViewerLane = Candidate.Id;
			break;
		}
	}
	return true;
}

void ATrafficTestRunner::MoveViewer(float DeltaSeconds)
{
	const UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Traffic || !Controller || !Controller->GetPawn())
	{
		return;
	}
	const FLaneNetwork& Lanes = Traffic->GetLaneNetwork();
	if (bViewerMoves)
	{
		ViewerS += ViewerSpeedMs * DeltaSeconds;
	}
	while (ViewerS > Lanes.GetLane(ViewerLane).Length())
	{
		const FTrafficLane& Lane = Lanes.GetLane(ViewerLane);
		TArray<int32> Candidates;
		for (const int32 NextId : Lane.Next)
		{
			if (Lanes.GetLane(NextId).bGood)
			{
				Candidates.Add(NextId);
			}
		}
		if (Candidates.IsEmpty())
		{
			ChooseStart();
			break;
		}
		ViewerS -= Lane.Length();
		ViewerLane = Candidates[Random.RandHelper(Candidates.Num())];
	}
	const FTrafficLane& Lane = Lanes.GetLane(ViewerLane);
	const FVector Position = Lanes.PositionAt(Lane, ViewerS) * 100.0;
	const FVector2D Direction = Lanes.DirectionAt(Lane, ViewerS);
	Controller->GetPawn()->SetActorLocation(Position + FVector(0.0, 0.0, TrafficTestCameraHeightCm), false, nullptr, ETeleportType::TeleportPhysics);
	Controller->SetControlRotation(FRotator(-5.0, FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.0));
}

void ATrafficTestRunner::PlaceBlocker()
{
	UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	const FLaneNetwork& Lanes = Traffic->GetLaneNetwork();
	const FTrafficLane& Lane = Lanes.GetLane(ViewerLane);
	const float S = FMath::Min(ViewerS + 40.f, Lane.Length() * 0.5f);
	const FVector Position = Lanes.PositionAt(Lane, S) * 100.0;
	const FVector2D Direction = Lanes.DirectionAt(Lane, S);
	Blocker = GetWorld()->SpawnActor<AActor>(Position, FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.0));
	USceneComponent* Root = NewObject<USceneComponent>(Blocker, TEXT("Root"));
	Blocker->SetRootComponent(Root);
	Root->RegisterComponent();
	Blocker->SetActorLocationAndRotation(Position, FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.0));
	Traffic->RegisterExternalVehicle(Blocker);
	UE_LOG(LogTrafficTest, Display, TEXT("TRAFFICTEST parked a stand-in for the player on lane %d at s=%.0f m, %s"), ViewerLane, S, *(Position / 100.0).ToString());
}

void ATrafficTestRunner::ReportProgress()
{
	const UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	if (!Traffic)
	{
		return;
	}
	const FAITrafficStats& Stats = Traffic->GetStats();
	const FAITrafficViolationTotals Violations = Traffic->GetViolationTotals();
	UE_LOG(LogTrafficTest, Display, TEXT("TRAFFICTEST t=%.0f s: %d cars, %d spawned, %d removed, red light %d, speeding %d, amber runs %d, AI collisions %d, deadlocks %d"),
		RunSeconds, Traffic->GetCarCount(), Stats.Spawned, Stats.Removed, Violations.RedLight, Violations.Speeding, Violations.AmberRun,
		Stats.Collisions, Stats.Deadlocks);
}

void ATrafficTestRunner::Finish()
{
	SetActorTickEnabled(false);
	ReportProgress();
	const UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	if (Traffic)
	{
		const FAITrafficStats& Stats = Traffic->GetStats();
		const FAITrafficViolationTotals Violations = Traffic->GetViolationTotals();
		const bool bPass = Violations.RedLight == 0 && Violations.Speeding == 0 && Stats.Collisions == 0 && Stats.Deadlocks == 0 && Stats.AgentContacts == 0
			&& Stats.Spawned > 0;
		UE_LOG(LogTrafficTest, Display, TEXT("TRAFFICTEST %s: %.1f minutes, %d cars spawned (at most %d at once, %.1f on average), %.1f km driven in total"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), RunSeconds / 60.f, Stats.Spawned, MaxCars, CarSamples > 0 ? double(CarSampleSum) / CarSamples : 0.0,
			Stats.DistanceDrivenM / 1000.0);
		UE_LOG(LogTrafficTest, Display, TEXT("TRAFFICTEST violations by AI cars: red light %d, speeding %d, amber runs %d (not counted); collisions between AI cars %d; contacts with the player's stand-in %d; cars stuck for a minute for no good reason %d (%d cars stood still for a minute in all, at long red lights or behind the player)"),
			Violations.RedLight, Violations.Speeding, Violations.AmberRun, Stats.Collisions, Stats.AgentContacts, Stats.Deadlocks, Stats.LongWaits);
	}
	TArray<float> Sorted = FrameMilliseconds;
	Sorted.Sort();
	if (!Sorted.IsEmpty())
	{
		double Sum = 0.0;
		for (const float Milliseconds : Sorted)
		{
			Sum += Milliseconds;
		}
		const auto Percentile = [&](double Fraction) { return Sorted[FMath::Min(Sorted.Num() - 1, int32(Sorted.Num() * Fraction))]; };
		UE_LOG(LogTrafficTest, Display, TEXT("TRAFFICTEST %d frames: average %.2f ms, median %.2f ms, 99th percentile %.2f ms, worst %.2f ms"), Sorted.Num(),
			Sum / Sorted.Num(), Percentile(0.5), Percentile(0.99), Sorted.Last());
	}
	GEngine->Exec(GetWorld(), TEXT("quit"));
}

void ATrafficTestRunner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UAITrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UAITrafficSubsystem>();
	APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Traffic || !Traffic->IsRunning() || !Controller || !Controller->GetPawn())
	{
		return;
	}
	if (ViewerLane == INDEX_NONE)
	{
		if (!ChooseStart())
		{
			return;
		}
		if (AWorldStreamer* Streamer = TActorIterator<AWorldStreamer>(GetWorld()) ? *TActorIterator<AWorldStreamer>(GetWorld()) : nullptr)
		{
			Streamer->LoadAroundBlocking(Traffic->GetLaneNetwork().PositionAt(Traffic->GetLaneNetwork().GetLane(ViewerLane), 0.f) * 100.0);
		}
		if (bBlocker)
		{
			PlaceBlocker();
		}
	}
	MoveViewer(DeltaSeconds);
	if (WarmupSeconds > 0.f)
	{
		WarmupSeconds -= DeltaSeconds;
		LastFrameStartSeconds = FPlatformTime::Seconds();
		return;
	}
	const double Now = FPlatformTime::Seconds();
	FrameMilliseconds.Add(float((Now - LastFrameStartSeconds) * 1000.0));
	LastFrameStartSeconds = Now;
	RunSeconds += DeltaSeconds;
	SecondsSinceReport += DeltaSeconds;
	CarSampleSum += Traffic->GetCarCount();
	++CarSamples;
	MaxCars = FMath::Max(MaxCars, Traffic->GetCarCount());
	if (SecondsSinceReport >= TrafficTestReportIntervalSeconds)
	{
		SecondsSinceReport = 0.f;
		ReportProgress();
	}
	if (RunSeconds >= DurationSeconds)
	{
		Finish();
	}
}
