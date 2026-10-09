#include "TrafficSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "WorldTileActor.h"

DEFINE_LOG_CATEGORY_STATIC(LogTraffic, Log, All);

static TAutoConsoleVariable<float> CVarNight(TEXT("tg.Night"), -1.f,
	TEXT("Street lamps and signal brightness: 0 day, 1 night, negative = from the sun's elevation."));
static TAutoConsoleVariable<int32> CVarNightScene(TEXT("tg.NightScene"), 0,
	TEXT("1 turns the scene into a night test: sun below the horizon, exposure for night."));
static TAutoConsoleVariable<float> CVarNightExposure(TEXT("tg.NightEV"), 4.f, TEXT("EV100 used by tg.NightScene."));
static TAutoConsoleVariable<int32> CVarFakeHeadlights(TEXT("tg.FakeHeadlights"), 0,
	TEXT("1 puts a pair of headlights on the camera, for night test shots when the car has none."));

namespace
{
constexpr int32 LampLightCount = 20;
constexpr int32 LensLightCount = 10;
constexpr float LampLightRangeCm = 11000.f;
constexpr float LensLightRangeCm = 7000.f;
constexpr float LampLumens = 11000.f;
constexpr float SlowUpdateSeconds = 0.25f;
constexpr float SignalUpdateSeconds = 0.1f;

/** Pitch that puts the sun far enough below the horizon for night. */
constexpr float NightSunPitchDegrees = 22.f;

FLinearColor LensLightColor(ESignalAspect Aspect)
{
	switch (Aspect)
	{
	case ESignalAspect::Green: return FLinearColor(0.1f, 1.f, 0.35f);
	case ESignalAspect::Amber: return FLinearColor(1.f, 0.55f, 0.05f);
	default: return FLinearColor(1.f, 0.08f, 0.04f);
	}
}

int32 LensIndexFor(ESignalAspect Aspect)
{
	switch (Aspect)
	{
	case ESignalAspect::Green: return 2;
	case ESignalAspect::Amber: return 1;
	default: return 0;
	}
}
}

bool UTrafficSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

TStatId UTrafficSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTrafficSubsystem, STATGROUP_Tickables);
}

void UTrafficSubsystem::Deinitialize()
{
	LightSources.Reset();
	SignalTiles.Reset();
	Super::Deinitialize();
}

bool UTrafficSubsystem::LoadRegion(const FString& WorldDir)
{
	FParse::Value(FCommandLine::Get(), TEXT("TrafficTimeOffset="), TimeOffsetSeconds);
	FString Error;
	if (!Network.Load(FPaths::Combine(WorldDir, TEXT("traffic.json")), Error))
	{
		UE_LOG(LogTraffic, Warning, TEXT("No traffic data: %s"), *Error);
		return false;
	}
	RegionDirectory = WorldDir;
	UE_LOG(LogTraffic, Log, TEXT("Traffic: %d signal junctions, %d approaches"), Network.GetJunctions().Num(), Network.GetApproaches().Num());
	return true;
}

double UTrafficSubsystem::GetTrafficTime() const
{
	return GetWorld()->GetTimeSeconds() + TimeOffsetSeconds;
}

bool UTrafficSubsystem::FindSignalAhead(const FVector& Location, const FVector& Forward, FApproachQuery& Out, float MaxDistanceCm,
	float BehindCm, int32 PreferApproachId) const
{
	return Network.FindApproachAhead(FVector2D(Location), FVector2D(Forward).GetSafeNormal(), MaxDistanceCm, BehindCm, GetTrafficTime(), Out,
		PreferApproachId);
}

FSpeedLimitResult UTrafficSubsystem::GetSpeedLimit(const FVector& Location, const FVector& Forward) const
{
	return Network.GetSpeedLimit(FVector2D(Location), FVector2D(Forward).GetSafeNormal());
}

void UTrafficSubsystem::RegisterSignalTile(AWorldTileActor* Tile)
{
	SignalTiles.AddUnique(Tile);
}

void UTrafficSubsystem::UnregisterSignalTile(AWorldTileActor* Tile)
{
	SignalTiles.RemoveSingleSwap(Tile);
}

void UTrafficSubsystem::AddLightSources(const AActor* Owner, TArray<FStreetLightSource>&& Sources)
{
	LightSources.Add(Owner, MoveTemp(Sources));
}

void UTrafficSubsystem::RemoveLightSources(const AActor* Owner)
{
	LightSources.Remove(Owner);
}

void UTrafficSubsystem::SetHeadlight(const FVector& LocationCm, const FVector& Direction, float Candela)
{
	HeadlightLocation = LocationCm;
	HeadlightDirection = Direction.GetSafeNormal();
	HeadlightCandela = Candela;
}

bool UTrafficSubsystem::GetViewerLocation(FVector& OutLocation) const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (Controller && Controller->PlayerCameraManager)
	{
		OutLocation = Controller->PlayerCameraManager->GetCameraLocation();
		return true;
	}
	return false;
}

void UTrafficSubsystem::UpdateNightFactor()
{
	const float Override = CVarNight.GetValueOnGameThread();
	if (Override >= 0.f)
	{
		NightFactor = FMath::Clamp(Override, 0.f, 1.f);
		return;
	}
	TActorIterator<ADirectionalLight> Sun(GetWorld());
	if (!Sun)
	{
		return;
	}
	const float ElevationDegrees = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(-Sun->GetActorForwardVector().Z, -1.f, 1.f)));
	NightFactor = 1.f - FMath::SmoothStep(-4.f, 2.f, ElevationDegrees);
}

void UTrafficSubsystem::ApplyNightScene()
{
	const bool bWanted = CVarNightScene.GetValueOnGameThread() > 0;
	if (!bWanted || bNightSceneApplied)
	{
		return;
	}
	bNightSceneApplied = true;
	for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
	{
		FRotator Rotation = It->GetActorRotation();
		Rotation.Pitch = NightSunPitchDegrees;
		It->SetActorRotation(Rotation);
	}
	for (TActorIterator<APostProcessVolume> It(GetWorld()); It; ++It)
	{
		const float Exposure = CVarNightExposure.GetValueOnGameThread();
		It->Settings.bOverride_AutoExposureMinBrightness = true;
		It->Settings.bOverride_AutoExposureMaxBrightness = true;
		It->Settings.AutoExposureMinBrightness = Exposure;
		It->Settings.AutoExposureMaxBrightness = Exposure;
	}
	if (CVarNight.GetValueOnGameThread() < 0.f)
	{
		CVarNight->Set(1.f);
	}
}

void UTrafficSubsystem::EnsureLightPool()
{
	if (LightHolder)
	{
		return;
	}
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	LightHolder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Parameters);
	USceneComponent* Root = NewObject<USceneComponent>(LightHolder, TEXT("Root"));
	LightHolder->SetRootComponent(Root);
	Root->RegisterComponent();

	const auto SetupLight = [](ULocalLightComponent* Light, float AttenuationCm)
	{
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetCastShadows(false);
		Light->SetAttenuationRadius(AttenuationCm);
		Light->SetVisibility(false);
	};
	for (int32 Index = 0; Index < LampLightCount; ++Index)
	{
		USpotLightComponent* Light = NewObject<USpotLightComponent>(LightHolder);
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetIntensityUnits(ELightUnits::Lumens);
		Light->SetIntensity(LampLumens);
		Light->SetInnerConeAngle(25.f);
		Light->SetOuterConeAngle(70.f);
		Light->SetTemperature(3300.f);
		Light->SetUseTemperature(true);
		Light->SetSourceRadius(12.f);
		SetupLight(Light, 3800.f);
		LampLights.Add(Light);
		LampAssignments.Add(TPair<const AActor*, int32>(nullptr, INDEX_NONE));
	}
	for (int32 Index = 0; Index < LensLightCount; ++Index)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(LightHolder);
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(8.f);
		Light->SetSourceRadius(5.f);
		SetupLight(Light, 900.f);
		LensLights.Add(Light);
	}
	for (int32 Index = 0; Index < 2; ++Index)
	{
		USpotLightComponent* Light = NewObject<USpotLightComponent>(LightHolder);
		Light->SetupAttachment(Root);
		Light->RegisterComponent();
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(9000.f);
		Light->SetInnerConeAngle(8.f);
		Light->SetOuterConeAngle(34.f);
		Light->SetTemperature(4300.f);
		Light->SetUseTemperature(true);
		Light->SetSourceRadius(6.f);
		SetupLight(Light, 12000.f);
		HeadlightLights.Add(Light);
	}
}

void UTrafficSubsystem::UpdateLightPool()
{
	EnsureLightPool();
	FVector Viewer;
	const bool bHasViewer = GetViewerLocation(Viewer);
	const bool bNight = NightFactor > 0.02f && bHasViewer;

	// Street lamps: keep lights on lamps that stay in range, give the free lights to the nearest new ones.
	struct FCandidate { const AActor* Owner; int32 Index; float DistanceSquared; };
	TArray<FCandidate> Candidates;
	if (bNight)
	{
		for (const TPair<const AActor*, TArray<FStreetLightSource>>& Entry : LightSources)
		{
			for (int32 Index = 0; Index < Entry.Value.Num(); ++Index)
			{
				const FStreetLightSource& Source = Entry.Value[Index];
				if (Source.ApproachId != INDEX_NONE)
				{
					continue;
				}
				const float DistanceSquared = float(FVector::DistSquared(Source.LocationCm, Viewer));
				if (DistanceSquared < FMath::Square(LampLightRangeCm))
				{
					Candidates.Add({Entry.Key, Index, DistanceSquared});
				}
			}
		}
		Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.DistanceSquared < B.DistanceSquared; });
		Candidates.SetNum(FMath::Min(Candidates.Num(), LampLightCount));
	}
	TArray<bool> CandidateTaken;
	CandidateTaken.Init(false, Candidates.Num());
	for (int32 Slot = 0; Slot < LampLights.Num(); ++Slot)
	{
		TPair<const AActor*, int32>& Assignment = LampAssignments[Slot];
		const int32 Kept = Candidates.IndexOfByPredicate([&](const FCandidate& C) { return C.Owner == Assignment.Key && C.Index == Assignment.Value; });
		if (Kept != INDEX_NONE)
		{
			CandidateTaken[Kept] = true;
		}
		else
		{
			Assignment = TPair<const AActor*, int32>(nullptr, INDEX_NONE);
		}
	}
	for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
	{
		if (CandidateTaken[CandidateIndex])
		{
			continue;
		}
		for (int32 Slot = 0; Slot < LampLights.Num(); ++Slot)
		{
			if (LampAssignments[Slot].Key == nullptr)
			{
				LampAssignments[Slot] = TPair<const AActor*, int32>(Candidates[CandidateIndex].Owner, Candidates[CandidateIndex].Index);
				break;
			}
		}
	}
	for (int32 Slot = 0; Slot < LampLights.Num(); ++Slot)
	{
		USpotLightComponent* Light = LampLights[Slot];
		const TPair<const AActor*, int32>& Assignment = LampAssignments[Slot];
		const TArray<FStreetLightSource>* Sources = Assignment.Key ? LightSources.Find(Assignment.Key) : nullptr;
		if (!bNight || !Sources || !Sources->IsValidIndex(Assignment.Value))
		{
			Light->SetVisibility(false);
			continue;
		}
		const FStreetLightSource& Source = (*Sources)[Assignment.Value];
		const float Distance = float(FVector::Dist(Source.LocationCm, Viewer));
		const float Fade = 1.f - FMath::SmoothStep(LampLightRangeCm * 0.75f, LampLightRangeCm, Distance);
		Light->SetWorldLocationAndRotation(Source.LocationCm, FRotator(-90.f, 0.f, 0.f));
		Light->SetIntensity(LampLumens * NightFactor * Fade);
		Light->SetVisibility(true);
	}

	// Signal lenses: the nearest few get a small coloured light at the lit lens.
	TArray<FCandidate> Lenses;
	if (bNight)
	{
		for (const TPair<const AActor*, TArray<FStreetLightSource>>& Entry : LightSources)
		{
			for (int32 Index = 0; Index < Entry.Value.Num(); ++Index)
			{
				const FStreetLightSource& Source = Entry.Value[Index];
				const float DistanceSquared = float(FVector::DistSquared(Source.LocationCm, Viewer));
				if (Source.ApproachId != INDEX_NONE && DistanceSquared < FMath::Square(LensLightRangeCm))
				{
					Lenses.Add({Entry.Key, Index, DistanceSquared});
				}
			}
		}
		Lenses.Sort([](const FCandidate& A, const FCandidate& B) { return A.DistanceSquared < B.DistanceSquared; });
	}
	for (int32 Slot = 0; Slot < LensLights.Num(); ++Slot)
	{
		UPointLightComponent* Light = LensLights[Slot];
		if (!Lenses.IsValidIndex(Slot))
		{
			Light->SetVisibility(false);
			continue;
		}
		const FStreetLightSource& Source = LightSources[Lenses[Slot].Owner][Lenses[Slot].Index];
		const ESignalAspect Aspect = Network.GetApproachState(Source.ApproachId, GetTrafficTime()).Aspect;
		Light->SetWorldLocation(Source.LocationCm + FVector(0.f, 0.f, Source.LensOffsetsCm[LensIndexFor(Aspect)]) + Source.Direction * 25.f);
		Light->SetLightColor(LensLightColor(Aspect));
		Light->SetVisibility(true);
	}
}

void UTrafficSubsystem::UpdateFakeHeadlights()
{
	const bool bWanted = CVarFakeHeadlights.GetValueOnGameThread() > 0;
	FVector Viewer;
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (!bWanted || !Controller || !Controller->PlayerCameraManager || !GetViewerLocation(Viewer))
	{
		for (USpotLightComponent* Light : HeadlightLights)
		{
			Light->SetVisibility(false);
		}
		return;
	}
	const FVector Forward = Controller->PlayerCameraManager->GetCameraRotation().Vector();
	const FVector Flat = FVector(Forward.X, Forward.Y, 0.f).GetSafeNormal();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Flat) * -1.f;
	const FVector Down = FVector(Flat.X, Flat.Y, -0.04f);
	for (int32 Index = 0; Index < HeadlightLights.Num(); ++Index)
	{
		const float Side = Index == 0 ? -65.f : 65.f;
		HeadlightLights[Index]->SetWorldLocationAndRotation(Viewer + Flat * 180.f + Right * Side + FVector(0, 0, -45.f), Down.Rotation());
		HeadlightLights[Index]->SetVisibility(true);
	}
	SetHeadlight(Viewer + Flat * 180.f + FVector(0, 0, -45.f), Down, 9000.f);
}

void UTrafficSubsystem::PushMaterialParameters()
{
	if (!Collection)
	{
		Collection = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/World/Furniture/MPC_Furniture.MPC_Furniture"), nullptr, LOAD_NoWarn);
	}
	if (!Collection)
	{
		return;
	}
	UMaterialParameterCollectionInstance* Instance = GetWorld()->GetParameterCollectionInstance(Collection);
	Instance->SetScalarParameterValue(TEXT("Night"), NightFactor);
	Instance->SetScalarParameterValue(TEXT("HeadlightOn"), HeadlightCandela > 0.f ? 1.f : 0.f);
	Instance->SetScalarParameterValue(TEXT("HeadlightCandela"), FMath::Max(HeadlightCandela, 1.f));
	Instance->SetVectorParameterValue(TEXT("HeadlightPosition"), FLinearColor(HeadlightLocation.X, HeadlightLocation.Y, HeadlightLocation.Z));
	Instance->SetVectorParameterValue(TEXT("HeadlightDirection"), FLinearColor(HeadlightDirection.X, HeadlightDirection.Y, HeadlightDirection.Z));
}

void UTrafficSubsystem::UpdateSignalLenses()
{
	const double Time = GetTrafficTime();
	for (int32 Index = SignalTiles.Num() - 1; Index >= 0; --Index)
	{
		AWorldTileActor* Tile = SignalTiles[Index].Get();
		if (!Tile)
		{
			SignalTiles.RemoveAtSwap(Index);
			continue;
		}
		Tile->UpdateSignalHeads(Network, Time);
	}
}

void UTrafficSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	SecondsSinceSignalUpdate += DeltaTime;
	if (SecondsSinceSignalUpdate >= SignalUpdateSeconds)
	{
		SecondsSinceSignalUpdate = 0.f;
		UpdateSignalLenses();
	}
	UpdateFakeHeadlights();
	SecondsSinceSlowUpdate += DeltaTime;
	if (SecondsSinceSlowUpdate >= SlowUpdateSeconds)
	{
		SecondsSinceSlowUpdate = 0.f;
		ApplyNightScene();
		UpdateNightFactor();
		UpdateLightPool();
	}
	PushMaterialParameters();
}
