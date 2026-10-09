#include "WorldStreamer.h"

#include "Async/ParallelFor.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/Queue.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tasks/Task.h"
#include "WorldTileActor.h"
#include "WorldTileData.h"
#include "WorldTileMesher.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldStreamer, Log, All);

namespace
{
constexpr float EyeHeightCm = 120.f;
constexpr float UpdateIntervalSeconds = 0.25f;
}

/** One tile meshed at one detail level by a worker. */
struct FWorldTileBuild
{
	int32 TileIndex = INDEX_NONE;
	int32 Detail = INDEX_NONE;
	TSharedPtr<const FWorldTileData> Data;
	TUniquePtr<FWorldTileMeshes> Meshes;
	FString Error;
	double BuildSeconds = 0.0;
};

/** State shared with worker tasks, which may still finish after the streamer is gone. */
struct FWorldStreamerShared
{
	TQueue<TSharedPtr<FWorldTileBuild>, EQueueMode::Mpsc> Finished;
	std::atomic<bool> bCancelled = false;
};

namespace
{
/** Reads (if not cached) and meshes one tile. Runs on a worker thread. */
TSharedPtr<FWorldTileBuild> RunBuild(int32 TileIndex, int32 Detail, const FString& Path,
	TSharedPtr<const FWorldTileData> Data, const FWorldMeshingContext& Context)
{
	TSharedPtr<FWorldTileBuild> Build = MakeShared<FWorldTileBuild>();
	Build->TileIndex = TileIndex;
	Build->Detail = Detail;
	const double StartTime = FPlatformTime::Seconds();
	if (!Data)
	{
		TSharedPtr<FWorldTileData> Loaded = MakeShared<FWorldTileData>();
		if (!FWorldTileData::Load(Path, *Loaded, Build->Error))
		{
			return Build;
		}
		Data = Loaded;
	}
	Build->Data = Data;
	Build->Meshes = MakeUnique<FWorldTileMeshes>(BuildWorldTileMeshes(*Data, static_cast<EWorldTileDetail>(Detail), Context));
	Build->BuildSeconds = FPlatformTime::Seconds() - StartTime;
	return Build;
}

/** <data root>/world/<Region>: $THIRD_GEAR_DATA, else the project's External link. */
FString FindWorldDir(const FString& Region)
{
	FString DataRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("THIRD_GEAR_DATA"));
	if (DataRoot.IsEmpty())
	{
		DataRoot = FPaths::Combine(FPaths::ProjectDir(), TEXT("External"));
	}
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(DataRoot, TEXT("world"), Region));
}

/** Shortest distance from a point to a rectangle (0 inside). */
double DistanceToBox(const FBox2D& Box, const FVector2D& Point)
{
	const FVector2D Clamped(FMath::Clamp(Point.X, Box.Min.X, Box.Max.X), FMath::Clamp(Point.Y, Box.Min.Y, Box.Max.Y));
	return FVector2D::Distance(Clamped, Point);
}
}

AWorldStreamer::AWorldStreamer()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	PlantModels.Add(TEXT("broadleaf"), TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Vegetation/SM_Broadleaf_01.SM_Broadleaf_01"))));
	PlantModels.Add(TEXT("conifer"), TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Vegetation/SM_Conifer_01.SM_Conifer_01"))));
	PlantModels.Add(TEXT("shrub"), TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/Vegetation/SM_Shrub_01.SM_Shrub_01"))));
}

bool AWorldStreamer::EnsureIndex()
{
	if (bIndexLoaded || bIndexFailed)
	{
		return bIndexLoaded;
	}
	FParse::Value(FCommandLine::Get(), TEXT("Region="), Region);
	WorldDir = FindWorldDir(Region);
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(WorldDir, TEXT("world.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		UE_LOG(LogWorldStreamer, Error, TEXT("No world data in %s (build it with Tools/osmimport/build_world.py %s)"), *WorldDir, *Region);
		bIndexFailed = true;
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("tiles")))
	{
		const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>& Bounds = Entry->GetArrayField(TEXT("bounds_m"));
		FTileState& Tile = Tiles.AddDefaulted_GetRef();
		Tile.Path = FPaths::Combine(WorldDir, Entry->GetStringField(TEXT("file")));
		Tile.Bounds = FBox2D(FVector2D(Bounds[0]->AsNumber(), Bounds[1]->AsNumber()) * 100.0,
			FVector2D(Bounds[2]->AsNumber(), Bounds[3]->AsNumber()) * 100.0);
	}
	Start = Root->GetObjectField(TEXT("start"));
	Shared = MakeShared<FWorldStreamerShared>();
	bIndexLoaded = true;
	UE_LOG(LogWorldStreamer, Log, TEXT("World %s: %d tiles from %s"), *Region, Tiles.Num(), *WorldDir);
	return true;
}

void AWorldStreamer::PrepareAssets()
{
	if (MeshingContext)
	{
		return;
	}
	TSharedPtr<FWorldMeshingContext> Context = MakeShared<FWorldMeshingContext>();
	for (const TPair<FString, TSoftObjectPtr<UStaticMesh>>& Model : PlantModels)
	{
		UStaticMesh* Mesh = Model.Value.LoadSynchronous();
		if (!Mesh)
		{
			UE_LOG(LogWorldStreamer, Warning, TEXT("Plant model %s missing (%s); run Scripts/bake_vegetation.py"), *Model.Key, *Model.Value.ToString());
			continue;
		}
		const FBox Box = Mesh->GetBoundingBox();
		const FVector Size = Box.GetSize() / 100.0;
		LoadedPlantModels.Add(Model.Key, Mesh);
		Context->PlantModelSizes.Add(Model.Key, FVector2f(float(FMath::Max(Size.X, Size.Y)), float(Size.Z)));
	}
	MeshingContext = Context;
}

UMaterialInterface* AWorldStreamer::FindMaterial(const FString& Section)
{
	if (TObjectPtr<UMaterialInterface>* Found = Materials.Find(Section))
	{
		return *Found;
	}
	const FString Path = FString::Printf(TEXT("%s/M_%s.M_%s"), *MaterialFolder, *Section, *Section);
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn);
	if (!Material)
	{
		UE_LOG(LogWorldStreamer, Warning, TEXT("Material %s missing; run Scripts/create_materials.py"), *Path);
	}
	Materials.Add(Section, Material);
	return Material;
}

bool AWorldStreamer::GetStartTransform(FTransform& OutTransform)
{
	if (!EnsureIndex() || !Start.IsValid())
	{
		return false;
	}
	const FVector Location(Start->GetNumberField(TEXT("x")) * 100.0, Start->GetNumberField(TEXT("y")) * 100.0,
		Start->GetNumberField(TEXT("z")) * 100.0 + EyeHeightCm);
	OutTransform = FTransform(FRotator(0.0, Start->GetNumberField(TEXT("yaw")), 0.0), Location);
	return true;
}

int32 AWorldStreamer::WantedDetail(const FTileState& Tile, const FVector2D& Location) const
{
	const double Distance = DistanceToBox(Tile.Bounds, Location);
	if (Distance < NearDistance)
	{
		return int32(EWorldTileDetail::Near);
	}
	if (Distance < MiddleDistance)
	{
		return int32(EWorldTileDetail::Middle);
	}
	if (Distance < FarDistance)
	{
		return int32(EWorldTileDetail::Far);
	}
	return INDEX_NONE;
}

void AWorldStreamer::StartBuild(int32 TileIndex, int32 Detail)
{
	FTileState& Tile = Tiles[TileIndex];
	Tile.PendingDetail = Detail;
	++BuildsInFlight;
	UE::Tasks::Launch(UE_SOURCE_LOCATION,
		[TileIndex, Detail, Path = Tile.Path, Data = Tile.Data, Context = MeshingContext, SharedState = Shared]()
		{
			TSharedPtr<FWorldTileBuild> Build;
			if (!SharedState->bCancelled)
			{
				Build = RunBuild(TileIndex, Detail, Path, Data, *Context);
			}
			else
			{
				Build = MakeShared<FWorldTileBuild>();
				Build->TileIndex = TileIndex;
				Build->Detail = Detail;
				Build->Error = TEXT("cancelled");
			}
			SharedState->Finished.Enqueue(Build);
		});
}

void AWorldStreamer::Unload(FTileState& Tile)
{
	if (AWorldTileActor* Actor = Tile.Actor.Get())
	{
		Actor->Destroy();
	}
	Tile.Actor = nullptr;
	Tile.ShownDetail = INDEX_NONE;
	Tile.PendingDetail = INDEX_NONE;
}

void AWorldStreamer::ApplyBuild(FWorldTileBuild& Build, bool bCookNow)
{
	FTileState& Tile = Tiles[Build.TileIndex];
	if (Build.Detail != Tile.PendingDetail)
	{
		return; // superseded while it was being built
	}
	Tile.PendingDetail = INDEX_NONE;
	if (!Build.Meshes)
	{
		UE_LOG(LogWorldStreamer, Error, TEXT("Tile %s: %s"), *Tile.Path, *Build.Error);
		Tile.FailedDetail = Build.Detail;
		return;
	}
	Tile.Data = Build.Data;
	const double StartTime = FPlatformTime::Seconds();
	TArray<UMaterialInterface*> SlotMaterials;
	for (const FString& Name : Build.Meshes->MaterialNames)
	{
		SlotMaterials.Add(FindMaterial(Name));
	}
	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	const FVector Origin(Tile.Data->Origin.X * 100.0, Tile.Data->Origin.Y * 100.0, 0.0);
	AWorldTileActor* Actor = GetWorld()->SpawnActor<AWorldTileActor>(Origin, FRotator::ZeroRotator, Parameters);
	const bool bCollision = Build.Detail == int32(EWorldTileDetail::Near);
	Actor->Populate(*Build.Meshes, SlotMaterials, LoadedPlantModels, bCollision, bCookNow);
	if (AWorldTileActor* Previous = Tile.Actor.Get())
	{
		Previous->Destroy();
	}
	Tile.Actor = Actor;
	Tile.ShownDetail = Build.Detail;
	UE_LOG(LogWorldStreamer, Log, TEXT("Tile %s detail %d: built in %.0f ms, spawned in %.1f ms"),
		*FPaths::GetBaseFilename(Tile.Path), Build.Detail, Build.BuildSeconds * 1000.0, (FPlatformTime::Seconds() - StartTime) * 1000.0);
}

bool AWorldStreamer::GetViewerLocation(FVector& OutLocation) const
{
	const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
	if (Controller && Controller->PlayerCameraManager)
	{
		OutLocation = Controller->PlayerCameraManager->GetCameraLocation();
		return true;
	}
	if (Controller && Controller->GetPawn())
	{
		OutLocation = Controller->GetPawn()->GetActorLocation();
		return true;
	}
	return false;
}

void AWorldStreamer::UpdateWanted(const FVector& Location)
{
	const FVector2D Viewer(Location);
	TArray<int32> Order;
	for (int32 Index = 0; Index < Tiles.Num(); ++Index)
	{
		FTileState& Tile = Tiles[Index];
		const int32 Wanted = WantedDetail(Tile, Viewer);
		if (Wanted == INDEX_NONE)
		{
			if (Tile.ShownDetail != INDEX_NONE || Tile.PendingDetail != INDEX_NONE)
			{
				Unload(Tile);
			}
			continue;
		}
		if (Wanted != Tile.ShownDetail && Wanted != Tile.PendingDetail && Wanted != Tile.FailedDetail)
		{
			Order.Add(Index);
		}
	}
	Order.Sort([&](int32 A, int32 B) { return DistanceToBox(Tiles[A].Bounds, Viewer) < DistanceToBox(Tiles[B].Bounds, Viewer); });
	for (const int32 Index : Order)
	{
		if (BuildsInFlight >= MaxBuildsInFlight)
		{
			break;
		}
		StartBuild(Index, WantedDetail(Tiles[Index], Viewer));
	}
}

void AWorldStreamer::LoadAroundBlocking(const FVector& Location)
{
	if (!EnsureIndex())
	{
		return;
	}
	PrepareAssets();
	const double StartTime = FPlatformTime::Seconds();
	const FVector2D Viewer(Location);
	TArray<int32> Needed;
	for (int32 Index = 0; Index < Tiles.Num(); ++Index)
	{
		const int32 Wanted = WantedDetail(Tiles[Index], Viewer);
		if (Wanted == INDEX_NONE)
		{
			Unload(Tiles[Index]);
		}
		else if (Wanted != Tiles[Index].ShownDetail)
		{
			Needed.Add(Index);
			Tiles[Index].PendingDetail = Wanted;
		}
	}
	TArray<TSharedPtr<FWorldTileBuild>> Builds;
	Builds.SetNum(Needed.Num());
	const FWorldMeshingContext& Context = *MeshingContext;
	ParallelFor(Needed.Num(), [&](int32 Slot)
	{
		const FTileState& Tile = Tiles[Needed[Slot]];
		Builds[Slot] = RunBuild(Needed[Slot], Tile.PendingDetail, Tile.Path, Tile.Data, Context);
	});
	for (const TSharedPtr<FWorldTileBuild>& Build : Builds)
	{
		ApplyBuild(*Build, /*bCookNow=*/true);
	}
	UE_LOG(LogWorldStreamer, Log, TEXT("Loaded %d tiles around %s in %.2f s"), Builds.Num(), *Location.ToString(),
		FPlatformTime::Seconds() - StartTime);
}

void AWorldStreamer::BeginPlay()
{
	Super::BeginPlay();
	if (!EnsureIndex())
	{
		return;
	}
	PrepareAssets();
	FVector Location;
	FTransform StartTransform;
	if (!GetViewerLocation(Location) && GetStartTransform(StartTransform))
	{
		Location = StartTransform.GetLocation();
	}
	UpdateWanted(Location);
}

void AWorldStreamer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Shared)
	{
		Shared->bCancelled = true;
	}
	Super::EndPlay(EndPlayReason);
}

void AWorldStreamer::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bIndexLoaded)
	{
		return;
	}
	TSharedPtr<FWorldTileBuild> Finished;
	while (Shared->Finished.Dequeue(Finished))
	{
		--BuildsInFlight;
		ReadyBuilds.Add(Finished);
	}
	FVector Location;
	const bool bHasViewer = GetViewerLocation(Location);
	if (bHasViewer && !ReadyBuilds.IsEmpty())
	{
		const FVector2D Viewer(Location);
		ReadyBuilds.Sort([&](const TSharedPtr<FWorldTileBuild>& A, const TSharedPtr<FWorldTileBuild>& B)
		{
			return DistanceToBox(Tiles[A->TileIndex].Bounds, Viewer) < DistanceToBox(Tiles[B->TileIndex].Bounds, Viewer);
		});
	}
	for (int32 Spawned = 0; Spawned < MaxSpawnsPerFrame && !ReadyBuilds.IsEmpty(); ++Spawned)
	{
		const TSharedPtr<FWorldTileBuild> Build = ReadyBuilds[0];
		ReadyBuilds.RemoveAt(0);
		ApplyBuild(*Build, /*bCookNow=*/false);
	}
	SecondsSinceUpdate += DeltaSeconds;
	if (bHasViewer && SecondsSinceUpdate >= UpdateIntervalSeconds)
	{
		SecondsSinceUpdate = 0.f;
		UpdateWanted(Location);
	}
}
