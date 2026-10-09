#include "AITrafficSubsystem.h"

#include "AITrafficCar.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Components/SpotLightComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "TrafficRuleComponent.h"
#include "TrafficSubsystem.h"
#include "WorldStreamer.h"

DEFINE_LOG_CATEGORY_STATIC(LogAITraffic, Log, All);

static TAutoConsoleVariable<int32> CVarSpawnAnywhere(TEXT("tg.TrafficSpawnAnywhere"), 0,
	TEXT("1 lets cars appear in view and close to the viewer (staging screenshots); normally they only appear out of sight."));
static TAutoConsoleVariable<int32> CVarHeadlightLights(TEXT("tg.TrafficHeadlights"), 8,
	TEXT("How many AI cars nearest the viewer get a real headlight at night."));
static TAutoConsoleVariable<int32> CVarTrafficCars(TEXT("tg.TrafficCars"), 30, TEXT("How many AI cars the traffic aims for around the viewer."));

namespace
{
constexpr float MinimumSpawnDistanceM = 110.f;
constexpr float MaximumSpawnDistanceM = 360.f;
/** Cars are only put into the viewer's field of view when they are this far away, where they can't be told apart. */
constexpr float VisibleSpawnDistanceM = 300.f;
constexpr float RemoveDistanceM = 430.f;
constexpr float SpawnIntervalSeconds = 0.25f;
/** Road length per car, weighted by the importance of the road, that the density aims for. */
constexpr float MetersOfRoadPerCar = 70.f;
constexpr float SpawnClearanceM = 30.f;
constexpr float SpawnSpeedFraction = 0.6f;
constexpr float ViewConeCosine = 0.17f;
constexpr float RoadLiftCm = 0.5f;
constexpr float BlinkPeriodSeconds = 0.7f;
constexpr float NightHeadlightFactor = 0.35f;
constexpr float StuckRemoveSeconds = 150.f;
constexpr float RuleCheckDelaySeconds = 0.5f;
constexpr float HeadlightRangeM = 110.f;
constexpr float HeadlightIntensityCandela = 9000.f;
constexpr float HeadlightAttenuationCm = 5500.f;
constexpr float HeadlightForwardCm = 80.f;
constexpr float HeadlightHeightCm = 68.f;

struct FPaintOption
{
	FLinearColor Color;
	float Weight;
};

/** Colours of new cars in Germany: mostly grey, black, white and silver, with some blue and red. Linear albedo. */
const FPaintOption PaintPalette[] = {
	{FLinearColor(0.150f, 0.155f, 0.162f), 18.f},   // grey
	{FLinearColor(0.045f, 0.047f, 0.050f), 14.f},   // anthracite
	{FLinearColor(0.012f, 0.012f, 0.013f), 18.f},   // black
	{FLinearColor(0.640f, 0.640f, 0.620f), 20.f},   // white
	{FLinearColor(0.330f, 0.340f, 0.350f), 10.f},   // silver
	{FLinearColor(0.010f, 0.022f, 0.075f), 8.f},    // dark blue
	{FLinearColor(0.300f, 0.012f, 0.010f), 5.f},    // red
	{FLinearColor(0.012f, 0.040f, 0.022f), 1.5f},   // dark green
	{FLinearColor(0.330f, 0.270f, 0.190f), 2.f},    // champagne
	{FLinearColor(0.380f, 0.150f, 0.020f), 1.f},    // orange
	{FLinearColor(0.070f, 0.035f, 0.025f), 1.5f},   // brown
};

TSharedPtr<FTrafficVehicleModel> MakeModel(const TCHAR* Folder, const TCHAR* Type, const TCHAR* Tag, float Weight)
{
	TSharedPtr<FTrafficVehicleModel> Model = MakeShared<FTrafficVehicleModel>();
	Model->Folder = Folder;
	Model->Type = Type;
	Model->Tag = Tag;
	Model->Weight = Weight;
	return Model;
}
}

bool UAITrafficSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld() && !FParse::Param(FCommandLine::Get(), TEXT("NoTraffic"));
}

TStatId UAITrafficSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UAITrafficSubsystem, STATGROUP_Tickables);
}

void UAITrafficSubsystem::Deinitialize()
{
	CarActors.Reset();
	ExternalVehicles.Reset();
	Super::Deinitialize();
}

void UAITrafficSubsystem::RegisterExternalVehicle(AActor* Actor, float HalfLengthM, float HalfWidthM)
{
	if (!Actor)
	{
		return;
	}
	ExternalVehicles.RemoveAll([](const FExternalVehicle& Vehicle) { return !Vehicle.Actor.IsValid(); });
	ExternalVehicles.Add({Actor, HalfLengthM, HalfWidthM});
}

bool UAITrafficSubsystem::TryStart()
{
	UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	if (!Traffic || Traffic->GetRegionDirectory().IsEmpty())
	{
		return false;   // the streamer hasn't loaded the region yet
	}
	bStartTried = true;
	FString Error;
	if (!Lanes.Load(FPaths::Combine(Traffic->GetRegionDirectory(), TEXT("lanes.json")), Error))
	{
		UE_LOG(LogAITraffic, Warning, TEXT("No AI traffic: %s (build it with Tools/osmimport/build_lanes.py)"), *Error);
		return false;
	}
	int32 Seed = FMath::Rand();
	FParse::Value(FCommandLine::Get(), TEXT("TrafficSeed="), Seed);
	Random.Initialize(Seed);
	Simulation.Initialize(&Lanes, &Traffic->GetNetwork(), Seed + 1);
	Models.Add(MakeModel(TEXT("vehicle07_Car"), TEXT("vehCar"), TEXT("vehicle07"), 1.6f));
	Models.Add(MakeModel(TEXT("vehicle02_Car"), TEXT("vehCar"), TEXT("vehicle02"), 1.0f));
	Models.Add(MakeModel(TEXT("vehicle03_Car"), TEXT("vehCar"), TEXT("vehicle03"), 0.8f));
	Models.Add(MakeModel(TEXT("vehicle05_Car"), TEXT("vehCar"), TEXT("vehicle05"), 1.2f));
	Models.Add(MakeModel(TEXT("vehicle06_Car"), TEXT("vehCar"), TEXT("vehicle06"), 0.5f));
	Models.Add(MakeModel(TEXT("vehicle01_Van"), TEXT("vehVan"), TEXT("vehicle01"), 0.45f));
	TSharedPtr<FTrafficVehicleModel> Taxi = MakeModel(TEXT("vehicle12_Car"), TEXT("vehCar"), TEXT("vehicle12"), 0.25f);
	Taxi->bFixedPaint = true;
	Taxi->FixedPaint = FLinearColor(0.50f, 0.45f, 0.33f);
	Models.Add(Taxi);
	int32 Cars = CVarTrafficCars.GetValueOnGameThread();
	FParse::Value(FCommandLine::Get(), TEXT("TrafficCars="), Cars);
	TargetCarCount = Cars;
	bRunning = true;
	UE_LOG(LogAITraffic, Log, TEXT("AI traffic: %d lanes, %d spawn lanes, aiming for %d cars"), Lanes.GetLanes().Num(), Lanes.GetSpawnLanes().Num(), TargetCarCount);
	return true;
}

bool UAITrafficSubsystem::GetViewer(FVector& OutLocationCm, FVector2D& OutForward) const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!Controller || !Controller->PlayerCameraManager)
	{
		return false;
	}
	OutLocationCm = Controller->PlayerCameraManager->GetCameraLocation();
	const FVector Forward = Controller->PlayerCameraManager->GetCameraRotation().Vector();
	OutForward = FVector2D(Forward.X, Forward.Y).GetSafeNormal();
	return true;
}

void UAITrafficSubsystem::CollectAgents(TArray<FSimAgent>& OutAgents)
{
	ExternalVehicles.RemoveAll([](const FExternalVehicle& Vehicle) { return !Vehicle.Actor.IsValid(); });
	for (const FExternalVehicle& Vehicle : ExternalVehicles)
	{
		const AActor* Actor = Vehicle.Actor.Get();
		FSimAgent& Agent = OutAgents.AddDefaulted_GetRef();
		Agent.Position = FVector2D(Actor->GetActorLocation()) * 0.01;
		Agent.Forward = FVector2D(Actor->GetActorForwardVector()).GetSafeNormal();
		Agent.Velocity = FVector2D(Actor->GetVelocity()) * 0.01;
		Agent.HalfLengthM = Vehicle.HalfLengthM;
		Agent.HalfWidthM = Vehicle.HalfWidthM;
	}
}

int32 UAITrafficSubsystem::PickModelIndex()
{
	float Total = 0.f;
	for (const TSharedPtr<FTrafficVehicleModel>& Model : Models)
	{
		Total += Model->Weight;
	}
	float Pick = Random.FRand() * Total;
	for (int32 Index = 0; Index < Models.Num(); ++Index)
	{
		Pick -= Models[Index]->Weight;
		if (Pick <= 0.f)
		{
			return Index;
		}
	}
	return 0;
}

FLinearColor UAITrafficSubsystem::PickPaint(const FTrafficVehicleModel& Model)
{
	if (Model.bFixedPaint)
	{
		return Model.FixedPaint;
	}
	float Total = 0.f;
	for (const FPaintOption& Option : PaintPalette)
	{
		Total += Option.Weight;
	}
	float Pick = Random.FRand() * Total;
	for (const FPaintOption& Option : PaintPalette)
	{
		Pick -= Option.Weight;
		if (Pick <= 0.f)
		{
			return Option.Color;
		}
	}
	return PaintPalette[0].Color;
}

bool UAITrafficSubsystem::TrySpawnOne(const FVector& ViewerCm, const FVector2D& ViewerForward)
{
	const TArray<int32>& SpawnLanes = Lanes.GetSpawnLanes();
	if (SpawnLanes.IsEmpty())
	{
		return false;
	}
	if (!Streamer.IsValid())
	{
		TActorIterator<AWorldStreamer> It(GetWorld());
		if (It)
		{
			Streamer = *It;
		}
	}
	const FVector2D Viewer = FVector2D(ViewerCm) * 0.01;
	for (int32 Attempt = 0; Attempt < 24; ++Attempt)
	{
		const FTrafficLane& Lane = Lanes.GetLane(SpawnLanes[Random.RandHelper(SpawnLanes.Num())]);
		const float TierChance = 0.25f + 0.2f * float(FMath::Clamp(Lane.Tier, 0, 4)) * 1.5f;
		if (Random.FRand() > TierChance)
		{
			continue;
		}
		const float S = Random.FRandRange(6.f, Lane.Length() - 12.f);
		const FVector Position = Lanes.PositionAt(Lane, S);
		const float Distance = float(FVector2D::Distance(FVector2D(Position), Viewer));
		const bool bAnywhere = CVarSpawnAnywhere.GetValueOnGameThread() != 0;
		if (Distance < (bAnywhere ? 25.f : MinimumSpawnDistanceM) || Distance > MaximumSpawnDistanceM)
		{
			continue;
		}
		const FVector2D Direction = (FVector2D(Position) - Viewer) / Distance;
		if (!bAnywhere && FVector2D::DotProduct(Direction, ViewerForward) > ViewConeCosine && Distance < VisibleSpawnDistanceM)
		{
			continue;
		}
		if (Streamer.IsValid() && !Streamer->IsNearTileShownAt(Position * 100.0))
		{
			continue;
		}
		if (!Simulation.IsAreaFree(Position, SpawnClearanceM))
		{
			continue;
		}
		const int32 ModelIndex = PickModelIndex();
		FTrafficVehicleModel& Model = *Models[ModelIndex];
		const bool bWasLoaded = Model.bLoaded;
		if (!Model.Load())
		{
			Model.Weight = 0.f;   // missing assets: never pick it again
			continue;
		}
		if (!bWasLoaded)
		{
			Model.CollectAssets(ModelAssets);
		}
		FSimCar Template;
		Template.LengthM = Model.LengthM;
		Template.WidthM = Model.WidthM;
		Template.WheelbaseM = Model.WheelbaseM;
		Template.FrontOverhangM = Model.FrontOverhangM;
		Template.RearOverhangM = Model.RearOverhangM;
		Template.WheelRadiusM = Model.WheelRadiusCm * 0.01f;
		FSimCar* Car = Simulation.AddCar(Template, Lane.Id, S, 0.f);
		Car->SpeedMs = Lane.LimitMs() * Car->DesiredSpeedFactor * SpawnSpeedFraction;
		FActorSpawnParameters Parameters;
		Parameters.ObjectFlags |= RF_Transient;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AAITrafficCar* Actor = GetWorld()->SpawnActor<AAITrafficCar>(Position * 100.0, FRotator::ZeroRotator, Parameters);
		if (!Actor)
		{
			Simulation.RemoveCar(Car->Id);
			return false;
		}
		Actor->Initialize(Model, PickPaint(Model));
		CarActors.Add(Car->Id, Actor);
		CarModel.Add(Car->Id, ModelIndex);
		return true;
	}
	return false;
}

int32 UAITrafficSubsystem::CountAllowedCars(const FVector& ViewerCm) const
{
	// Traffic density follows the road length near the viewer: a side street holds few cars, a main road many.
	const FVector2D Viewer = FVector2D(ViewerCm) * 0.01;
	float RoadMeters = 0.f;
	for (const int32 LaneId : Lanes.GetSpawnLanes())
	{
		const FTrafficLane& Lane = Lanes.GetLane(LaneId);
		const float Distance = float(FVector2D::Distance(FVector2D(Lane.Points[Lane.Points.Num() / 2]), Viewer));
		if (Distance < MaximumSpawnDistanceM)
		{
			RoadMeters += Lane.Length() * (0.4f + 0.2f * float(FMath::Clamp(Lane.Tier, 0, 4)));
		}
	}
	return FMath::Min(TargetCarCount, FMath::FloorToInt(RoadMeters / MetersOfRoadPerCar));
}

void UAITrafficSubsystem::SpawnCars(const FVector& ViewerCm, const FVector2D& ViewerForward)
{
	if (SecondsSinceSpawn < SpawnIntervalSeconds || Simulation.GetCars().Num() >= CountAllowedCars(ViewerCm))
	{
		return;
	}
	SecondsSinceSpawn = 0.f;
	TrySpawnOne(ViewerCm, ViewerForward);
}

void UAITrafficSubsystem::RemoveCar(int32 CarId)
{
	if (TObjectPtr<AAITrafficCar>* Actor = CarActors.Find(CarId))
	{
		if (UTrafficRuleComponent* Rules = (*Actor)->GetRuleChecker())
		{
			RemovedTotals.RedLight += Rules->GetViolationCount(ETrafficViolationType::RedLight);
			RemovedTotals.Speeding += Rules->GetViolationCount(ETrafficViolationType::Speeding);
			for (const FTrafficViolation& Event : Rules->GetHistory())
			{
				RemovedTotals.AmberRun += Event.Type == ETrafficViolationType::AmberRun ? 1 : 0;
			}
		}
		(*Actor)->Destroy();
		CarActors.Remove(CarId);
	}
	CarModel.Remove(CarId);
	ReportedViolations.Remove(CarId);
	Simulation.RemoveCar(CarId);
}

void UAITrafficSubsystem::RemoveFarCars(const FVector& ViewerCm, const FVector2D& ViewerForward)
{
	const FVector2D Viewer = FVector2D(ViewerCm) * 0.01;
	TArray<int32> ToRemove;
	for (const TSharedPtr<FSimCar>& Car : Simulation.GetCars())
	{
		const FVector2D Offset = Car->BodyCenter() - Viewer;
		const float Distance = float(Offset.Size());
		const bool bBehind = FVector2D::DotProduct(Offset.GetSafeNormal(), ViewerForward) < -ViewConeCosine;
		const bool bStuckFarAway = Car->StoppedSeconds > StuckRemoveSeconds && bBehind && Distance > 150.f;
		if (Distance > RemoveDistanceM || bStuckFarAway)
		{
			ToRemove.Add(Car->Id);
		}
	}
	for (const int32 CarId : ToRemove)
	{
		RemoveCar(CarId);
	}
}

void UAITrafficSubsystem::UpdateActors()
{
	const double Time = GetWorld()->GetTimeSeconds();
	const bool bBlinkOn = FMath::Fmod(Time, double(BlinkPeriodSeconds)) < double(BlinkPeriodSeconds) * 0.5;
	UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	const bool bHeadlights = Traffic && Traffic->GetNightFactor() > NightHeadlightFactor;
	for (const TSharedPtr<FSimCar>& CarPtr : Simulation.GetCars())
	{
		const FSimCar& Car = *CarPtr;
		AAITrafficCar* Actor = CarActors.FindRef(Car.Id);
		if (!Actor)
		{
			continue;
		}
		FVector Middle = (Car.FrontAxleM + Car.RearAxleM) * 50.0;
		Middle.Z += RoadLiftCm;
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(float(Car.Heading.Y), float(Car.Heading.X)));
		Actor->ApplyPose(Middle, FRotator(FMath::RadiansToDegrees(Car.PitchRadians), Yaw, 0.f), Car.SteerRadians, Car.WheelRollRadians);
		Actor->SetLights(Car.bBrakeLight, bHeadlights, Car.TurnSignal, bBlinkOn);
		if (Car.SecondsAlive > RuleCheckDelaySeconds)
		{
			Actor->EnableRuleChecking();
		}
	}
}

void UAITrafficSubsystem::UpdateHeadlightPool(const FVector& ViewerCm, bool bNight)
{
	const int32 Wanted = bNight ? CVarHeadlightLights.GetValueOnGameThread() : 0;
	if (Wanted <= 0 && HeadlightPool.IsEmpty())
	{
		return;
	}
	if (!LightHolder && Wanted > 0)
	{
		LightHolder = GetWorld()->SpawnActor<AActor>();
		USceneComponent* Root = NewObject<USceneComponent>(LightHolder, TEXT("Root"));
		LightHolder->SetRootComponent(Root);
		Root->RegisterComponent();
	}
	while (LightHolder && HeadlightPool.Num() < Wanted)
	{
		USpotLightComponent* Light = NewObject<USpotLightComponent>(LightHolder);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetupAttachment(LightHolder->GetRootComponent());
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(HeadlightIntensityCandela);
		Light->SetLightColor(FLinearColor(1.f, 0.93f, 0.82f));
		Light->SetAttenuationRadius(HeadlightAttenuationCm);
		Light->SetOuterConeAngle(38.f);
		Light->SetInnerConeAngle(10.f);
		Light->SetSourceRadius(4.f);
		Light->SetCastShadows(false);
		Light->RegisterComponent();
		Light->SetVisibility(false);
		HeadlightPool.Add(Light);
	}
	// The cars nearest the viewer within range get the lights, nearest first.
	TArray<TPair<float, const FSimCar*>> Candidates;
	const FVector2D Viewer = FVector2D(ViewerCm) * 0.01;
	for (const TSharedPtr<FSimCar>& Car : Simulation.GetCars())
	{
		const float Distance = float(FVector2D::Distance(Car->BodyCenter(), Viewer));
		if (bNight && Distance < HeadlightRangeM)
		{
			Candidates.Emplace(Distance, Car.Get());
		}
	}
	Candidates.Sort([](const TPair<float, const FSimCar*>& A, const TPair<float, const FSimCar*>& B) { return A.Key < B.Key; });
	for (int32 Index = 0; Index < HeadlightPool.Num(); ++Index)
	{
		USpotLightComponent* Light = HeadlightPool[Index];
		if (!Candidates.IsValidIndex(Index) || Index >= Wanted)
		{
			Light->SetVisibility(false);
			continue;
		}
		const FSimCar& Car = *Candidates[Index].Value;
		const FVector Forward(Car.Heading.X, Car.Heading.Y, 0.0);
		const FVector Position = Car.FrontAxleM * 100.0 + Forward * (Car.FrontOverhangM * 100.0 + HeadlightForwardCm) + FVector(0.0, 0.0, HeadlightHeightCm);
		Light->SetWorldLocationAndRotation(Position, FRotator(-1.5f + FMath::RadiansToDegrees(Car.PitchRadians), FMath::RadiansToDegrees(FMath::Atan2(float(Car.Heading.Y), float(Car.Heading.X))), 0.f));
		Light->SetVisibility(true);
	}
}

void UAITrafficSubsystem::ReportNewViolations()
{
	for (const TPair<int32, TObjectPtr<AAITrafficCar>>& Entry : CarActors)
	{
		const UTrafficRuleComponent* Rules = Entry.Value ? Entry.Value->GetRuleChecker() : nullptr;
		if (!Rules)
		{
			continue;
		}
		int32& Reported = ReportedViolations.FindOrAdd(Entry.Key);
		const TArray<FTrafficViolation>& History = Rules->GetHistory();
		for (; Reported < History.Num(); ++Reported)
		{
			const FSimCar* Car = Simulation.FindCar(Entry.Key);
			UE_LOG(LogAITraffic, Warning, TEXT("AITRAFFIC violation (%s) approach %d: %s; %s"), History[Reported].bCounts ? TEXT("counts") : TEXT("reported only"),
				History[Reported].ApproachId, *History[Reported].Message, Car ? *Simulation.Describe(*Car) : TEXT("car gone"));
		}
	}
}

FAITrafficViolationTotals UAITrafficSubsystem::GetViolationTotals() const
{
	FAITrafficViolationTotals Totals = RemovedTotals;
	for (const TPair<int32, TObjectPtr<AAITrafficCar>>& Entry : CarActors)
	{
		const UTrafficRuleComponent* Rules = Entry.Value ? Entry.Value->GetRuleChecker() : nullptr;
		if (!Rules)
		{
			continue;
		}
		Totals.RedLight += Rules->GetViolationCount(ETrafficViolationType::RedLight);
		Totals.Speeding += Rules->GetViolationCount(ETrafficViolationType::Speeding);
		for (const FTrafficViolation& Event : Rules->GetHistory())
		{
			Totals.AmberRun += Event.Type == ETrafficViolationType::AmberRun ? 1 : 0;
		}
	}
	return Totals;
}

void UAITrafficSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bStartTried)
	{
		TryStart();
	}
	UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>();
	if (!bRunning || !Traffic)
	{
		return;
	}
	FVector ViewerCm;
	FVector2D ViewerForward;
	if (!GetViewer(ViewerCm, ViewerForward))
	{
		return;
	}
	SecondsSinceSpawn += DeltaTime;
	TArray<FSimAgent> Agents;
	CollectAgents(Agents);
	RemoveFarCars(ViewerCm, ViewerForward);
	SpawnCars(ViewerCm, ViewerForward);
	Simulation.Step(FMath::Min(DeltaTime, 0.1f), Traffic->GetTrafficTime(), Agents);
	UpdateActors();
	UpdateHeadlightPool(ViewerCm, Traffic->GetNightFactor() > NightHeadlightFactor);
	ReportNewViolations();
}
