#include "WorldStreamer.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
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
#include "Materials/MaterialInstanceDynamic.h"
#include "TrafficSubsystem.h"
#include "GrassField.h"
#include "ParkedCars.h"
#include "AITrafficCar.h"
#include "WorldFurniture.h"
#include "WorldSnow.h"
#include "WorldTileActor.h"
#include "WorldTileData.h"
#include "WorldTileMesher.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldStreamer, Log, All);

namespace
{
constexpr float EyeHeightCm = 120.f;
constexpr float UpdateIntervalSeconds = 0.25f;
constexpr int32 ShrubCullDistanceCm = 40000;
/** Plants stop swaying beyond this camera distance; the wind materials fade their sway out towards it. */
constexpr int32 PlantWindDistanceCm = 9000;
/** Height the car is dropped from with -StartPose; the ground trace in the game mode reaches 80 m. */
constexpr float StartPoseHeightCm = 4000.f;
/** The snow layer is meshed for near tiles once the snow cover is above this. */
constexpr float SnowMeshWantedAbove = 0.0005f;
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

/** A finished build being turned into components a few steps per frame (see AWorldStreamer::RunSpawnStep). */
struct FTileSpawnJob
{
	TSharedPtr<FWorldTileBuild> Build;
	TWeakObjectPtr<AWorldTileActor> Actor;
	/** Materials by slot; kept alive by AWorldStreamer::Materials. */
	TArray<UMaterialInterface*> Materials;
	int32 NextStep = 0;
	double SpawnSeconds = 0.0;
	double LongestStepSeconds = 0.0;
	/** Next part of the street furniture to add (see AWorldTileActor::AddFurnitureStep). */
	int32 FurnitureStep = 0;
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
	TSharedPtr<const FWorldTileData> Data, const FWorldMeshingContext& Context, bool bBuildSnow)
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
	Build->Meshes = MakeUnique<FWorldTileMeshes>(BuildWorldTileMeshes(*Data, static_cast<EWorldTileDetail>(Detail), Context, bBuildSnow));
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
	// The start menu loads the level with ?Region=<name>; that choice wins over the command line.
	for (const FString& Option : GetWorld()->URL.Op)
	{
		if (Option.StartsWith(TEXT("Region=")))
		{
			Region = Option.Mid(FCString::Strlen(TEXT("Region=")));
		}
	}
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
	AddHorizonTiles();
	Start = Root->GetObjectField(TEXT("start"));
	if (UTrafficSubsystem* Traffic = GetWorld()->GetSubsystem<UTrafficSubsystem>())
	{
		Traffic->LoadRegion(WorldDir);
	}
	Shared = MakeShared<FWorldStreamerShared>();
	bIndexLoaded = true;
	UE_LOG(LogWorldStreamer, Log, TEXT("World %s: %d tiles from %s"), *Region, Tiles.Num(), *WorldDir);
	return true;
}

void AWorldStreamer::AddHorizonTiles()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoHorizon")))
	{
		return;
	}
	const FString HorizonDir = FPaths::Combine(WorldDir, TEXT("horizon"));
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(HorizonDir, TEXT("horizon.json")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		UE_LOG(LogWorldStreamer, Log, TEXT("No horizon for %s (build it with Tools/osmimport/build_horizon_world.py %s)"), *Region, *Region);
		return;
	}
	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("tiles")))
	{
		const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>& Bounds = Entry->GetArrayField(TEXT("bounds_m"));
		FTileState& Tile = Tiles.AddDefaulted_GetRef();
		Tile.Path = FPaths::Combine(HorizonDir, Entry->GetStringField(TEXT("file")));
		Tile.Bounds = FBox2D(FVector2D(Bounds[0]->AsNumber(), Bounds[1]->AsNumber()) * 100.0,
			FVector2D(Bounds[2]->AsNumber(), Bounds[3]->AsNumber()) * 100.0);
		Tile.bHorizon = true;
		++Count;
	}
	UE_LOG(LogWorldStreamer, Log, TEXT("Horizon: %d tiles"), Count);
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
		LoadPlantWindMaterials(Model.Key, *Mesh);
		Context->PlantModelSizes.Add(Model.Key, FVector2f(float(FMath::Max(Size.X, Size.Y)), float(Size.Z)));
	}
	MeshingContext = Context;
	PreloadMaterials();
	PrepareFurniture();
}

void AWorldStreamer::LoadPlantWindMaterials(const FString& ModelKey, const UStaticMesh& PlantMesh)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NoPlantWind")))
	{
		return; // for comparing frame times with and without sway
	}
	TArray<TObjectPtr<UMaterialInterface>> SlotMaterials;
	for (int32 Slot = 0; Slot < PlantMesh.GetStaticMaterials().Num(); ++Slot)
	{
		const FString Path = FString::Printf(TEXT("/Game/Vegetation/Wind/MI_%s_%d.MI_%s_%d"), *PlantMesh.GetName(), Slot, *PlantMesh.GetName(), Slot);
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
		if (!Material)
		{
			return; // all or nothing: a half-swapped tree would sway its bark away from its leaves
		}
		SlotMaterials.Add(Material);
	}
	LoadedPlantWindMaterials.Add(ModelKey, MoveTemp(SlotMaterials));
}

void AWorldStreamer::PrepareParkedCars()
{
	if (ParkedCars::IsDisabled())
	{
		return;
	}
	for (const TSharedPtr<FTrafficVehicleModel>& Model : ParkedCars::GetModels())
	{
		if (Model->Load())
		{
			Model->CollectAssets(ParkedCarAssets);
		}
	}
	ParkedCarCollider = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
}

void AWorldStreamer::PrepareFurniture()
{
	const FString Folder = TEXT("/Game/World/Furniture/Meshes");
	TArray<FString> Names = {FurnitureAssets::Lamp, FurnitureAssets::SignalPole, FurnitureAssets::SignalHead, FurnitureAssets::SignPlate,
		FurnitureAssets::SignClamp};
	for (const int32 Height : GetSignPoleHeightsCm())
	{
		Names.Add(FString::Printf(TEXT("SM_SignPole_%d"), Height));
	}
	for (const FString& Name : Names)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), *Folder, *Name, *Name), nullptr, LOAD_NoWarn);
		if (Mesh)
		{
			FurnitureMeshes.Add(Name, Mesh);
		}
		else
		{
			UE_LOG(LogWorldStreamer, Warning, TEXT("Street furniture mesh %s missing; run Scripts/create_furniture_assets.py"), *Name);
		}
	}
	PrepareParkedCars();
	SignMasterMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/World/Furniture/M_SignFace.M_SignFace"), nullptr, LOAD_NoWarn);
	// Loading a sign texture the first time a tile needs it stalls that frame, so load them all up front.
	TArray<FAssetData> Textures;
	FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(TEXT("/Game/World/Furniture/Signs")), Textures);
	for (const FAssetData& Asset : Textures)
	{
		if (UTexture* Texture = Cast<UTexture>(Asset.GetAsset()))
		{
			SignTextures.Add(Texture);
		}
	}
}

UMaterialInterface* AWorldStreamer::FindSignMaterial(const FString& GraphicName)
{
	if (TObjectPtr<UMaterialInterface>* Found = SignMaterials.Find(GraphicName))
	{
		return *Found;
	}
	UTexture* Texture = LoadObject<UTexture>(nullptr, *SignTexturePath(GraphicName), nullptr, LOAD_NoWarn);
	UMaterialInterface* Material = nullptr;
	if (Texture && SignMasterMaterial)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(SignMasterMaterial, this);
		Instance->SetTextureParameterValue(TEXT("Graphic"), Texture);
		Material = Instance;
	}
	else
	{
		UE_LOG(LogWorldStreamer, Warning, TEXT("Sign graphic %s missing"), *GraphicName);
	}
	SignMaterials.Add(GraphicName, Material);
	return Material;
}

void AWorldStreamer::PreloadMaterials()
{
	// Loading a material the first time a tile needs it stalls that frame for tens of milliseconds.
	TArray<FAssetData> Assets;
	FAssetRegistryModule::GetRegistry().GetAssetsByPath(FName(*MaterialFolder), Assets);
	for (const FAssetData& Asset : Assets)
	{
		const FString Name = Asset.AssetName.ToString();
		if (Name.StartsWith(TEXT("M_")))
		{
			FindMaterial(Name.RightChop(2));
		}
	}
}

UMaterialInterface* AWorldStreamer::FindBuildingMaterial(const FString& Section) const
{
	// The scanned facade and roof materials (Scripts/create_facade_materials.py) replace the plain ones where they exist.
	const bool bBuildingSection = Section.StartsWith(TEXT("Facade_")) || Section.StartsWith(TEXT("Roof_"));
	if (!bBuildingSection || FParse::Param(FCommandLine::Get(), TEXT("NoFacadeMaterials")))
	{
		return nullptr;
	}
	const FString Path = FString::Printf(TEXT("%s/M_%s.M_%s"), *BuildingMaterialFolder, *Section, *Section);
	return LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

UMaterialInterface* AWorldStreamer::FindMaterial(const FString& Section)
{
	if (TObjectPtr<UMaterialInterface>* Found = Materials.Find(Section))
	{
		return *Found;
	}
	UMaterialInterface* Material = FindBuildingMaterial(Section);
	const FString Path = FString::Printf(TEXT("%s/M_%s.M_%s"), *MaterialFolder, *Section, *Section);
	if (!Material)
	{
		Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn);
	}
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
	// -StartPose=x,y,yaw (metres, degrees) starts somewhere else, e.g. at a signal junction; the car is dropped onto the ground.
	FString Pose;
	TArray<FString> Parts;
	if (FParse::Value(FCommandLine::Get(), TEXT("StartPose="), Pose, /*bShouldStopOnSeparator=*/false) && Pose.ParseIntoArray(Parts, TEXT(",")) == 3)
	{
		OutTransform = FTransform(FRotator(0.0, FCString::Atod(*Parts[2]), 0.0),
			FVector(FCString::Atod(*Parts[0]) * 100.0, FCString::Atod(*Parts[1]) * 100.0, StartPoseHeightCm));
		return true;
	}
	const FVector Location(Start->GetNumberField(TEXT("x")) * 100.0, Start->GetNumberField(TEXT("y")) * 100.0,
		Start->GetNumberField(TEXT("z")) * 100.0 + EyeHeightCm);
	OutTransform = FTransform(FRotator(0.0, Start->GetNumberField(TEXT("yaw")), 0.0), Location);
	return true;
}

bool AWorldStreamer::IsNearTileShownAt(const FVector& Location) const
{
	const FVector2D Point(Location.X, Location.Y);
	for (const FTileState& Tile : Tiles)
	{
		if (!Tile.bHorizon && Tile.ShownDetail == int32(EWorldTileDetail::Near) && Tile.Bounds.IsInside(Point))
		{
			return true;
		}
	}
	return false;
}

int32 AWorldStreamer::WantedDetail(const FTileState& Tile, const FVector2D& Location) const
{
	if (Tile.bHorizon)
	{
		return int32(EWorldTileDetail::Far);
	}
	const double Distance = DistanceToBox2D(Tile.Bounds, Location);
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
		[TileIndex, Detail, bBuildSnow = bSnowWanted, Path = Tile.Path, Data = Tile.Data, Context = MeshingContext, SharedState = Shared]()
		{
			TSharedPtr<FWorldTileBuild> Build;
			if (!SharedState->bCancelled)
			{
				Build = RunBuild(TileIndex, Detail, Path, Data, *Context, bBuildSnow);
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

bool AWorldStreamer::AcceptBuild(const TSharedPtr<FWorldTileBuild>& Build)
{
	FTileState& Tile = Tiles[Build->TileIndex];
	if (Build->Detail != Tile.PendingDetail)
	{
		return false; // superseded while it was being built
	}
	if (!Build->Meshes)
	{
		UE_LOG(LogWorldStreamer, Error, TEXT("Tile %s: %s"), *Tile.Path, *Build->Error);
		Tile.PendingDetail = INDEX_NONE;
		Tile.FailedDetail = Build->Detail;
		return false;
	}
	Tile.Data = Build->Data;
	TSharedPtr<FTileSpawnJob> Job = MakeShared<FTileSpawnJob>();
	Job->Build = Build;
	SpawnJobs.Add(Job);
	return true;
}

bool AWorldStreamer::RunSpawnStep(FTileSpawnJob& Job, bool bCookNow)
{
	FWorldTileBuild& Build = *Job.Build;
	FWorldTileMeshes& Meshes = *Build.Meshes;
	FTileState& Tile = Tiles[Build.TileIndex];
	AWorldTileActor* Actor = Job.Actor.Get();
	if (Build.Detail != Tile.PendingDetail || (Job.NextStep > 0 && !Actor))
	{
		if (Actor)
		{
			Actor->Destroy(); // the tile moved on to another detail level, or was unloaded
		}
		return true;
	}
	const double StartTime = FPlatformTime::Seconds();
	const bool bNear = Build.Detail == int32(EWorldTileDetail::Near);
	const int32 Step = Job.NextStep++;
	const int32 ChunkSteps = Meshes.GroundChunks.Num();
	const int32 PlantSteps = Meshes.Plants.Num();
	const int32 SnowSteps = Meshes.Snow ? Meshes.Snow->Chunks.Num() : 0;
	bool bDone = false;
	if (Step == 0)
	{
		for (const FString& Name : Meshes.MaterialNames)
		{
			Job.Materials.Add(FindMaterial(Name));
		}
		FActorSpawnParameters Parameters;
		Parameters.ObjectFlags |= RF_Transient;
		const FVector Origin(Tile.Data->Origin.X * 100.0, Tile.Data->Origin.Y * 100.0, 0.0);
		Job.Actor = GetWorld()->SpawnActor<AWorldTileActor>(Origin, FRotator::ZeroRotator, Parameters);
	}
	else if (Step <= ChunkSteps)
	{
		const int32 Chunk = Step - 1;
		const FVector2D ChunkSize(Meshes.ChunkSizeCm);
		const FVector2D ChunkMin = Tile.Bounds.Min + FVector2D(Chunk % Meshes.ChunksPerSide, Chunk / Meshes.ChunksPerSide) * ChunkSize;
		Actor->AddGroundChunk(MoveTemp(Meshes.GroundChunks[Chunk]), FBox2D(ChunkMin, ChunkMin + ChunkSize), Job.Materials);
	}
	else if (Step == ChunkSteps + 1)
	{
		Actor->AddMarkings(MoveTemp(Meshes.MarkingsMesh), Job.Materials);
	}
	else if (Step == ChunkSteps + 2)
	{
		Actor->AddBuildings(MoveTemp(Meshes.BuildingsMesh), Job.Materials, bNear, bCookNow);
	}
	else if (Step <= ChunkSteps + 2 + PlantSteps)
	{
		const FWorldPlantInstances& Group = Meshes.Plants[Step - ChunkSteps - 3];
		const TObjectPtr<UStaticMesh>* Model = LoadedPlantModels.Find(Group.Model);
		TArray<UMaterialInterface*> WindMaterials;
		if (const TArray<TObjectPtr<UMaterialInterface>>* Found = LoadedPlantWindMaterials.Find(Group.Model))
		{
			WindMaterials.Append(*Found);
		}
		Actor->AddPlants(Model ? Model->Get() : nullptr, Group.Transforms, Group.Model == TEXT("shrub") ? ShrubCullDistanceCm : 0,
			WindMaterials, PlantWindDistanceCm);
	}
	else if (Step == ChunkSteps + 3 + PlantSteps)
	{
		if (Meshes.Furniture)
		{
			FFurnitureMeshes Furniture;
			const auto Find = [this](const TCHAR* Name) { const TObjectPtr<UStaticMesh>* Mesh = FurnitureMeshes.Find(Name); return Mesh ? Mesh->Get() : nullptr; };
			Furniture.Lamp = Find(FurnitureAssets::Lamp);
			Furniture.SignalPole = Find(FurnitureAssets::SignalPole);
			Furniture.SignalHead = Find(FurnitureAssets::SignalHead);
			Furniture.SignPlate = Find(FurnitureAssets::SignPlate);
			Furniture.SignClamp = Find(FurnitureAssets::SignClamp);
			for (const TSharedPtr<FTrafficVehicleModel>& Model : ParkedCars::GetModels())
			{
				Furniture.ParkedModels.Add(Model->bLoaded ? Model.Get() : nullptr);
			}
			Furniture.ParkedCollider = ParkedCarCollider;
			for (const int32 Height : GetSignPoleHeightsCm())
			{
				Furniture.SignPoles.Add(Height, Find(*FString::Printf(TEXT("SM_SignPole_%d"), Height)));
			}
			for (const TPair<FString, TArray<FTransform>>& Plates : Meshes.Furniture->SignPlates)
			{
				Furniture.SignMaterials.Add(Plates.Key, FindSignMaterial(Plates.Key));
			}
			if (!Actor->AddFurnitureStep(*Meshes.Furniture, Furniture, Job.FurnitureStep++))
			{
				--Job.NextStep; // more parts to add: come back to this step
			}
		}
	}
	else if (Step <= ChunkSteps + 3 + PlantSteps + SnowSteps)
	{
		const int32 SnowChunk = Step - ChunkSteps - 4 - PlantSteps;
		const FVector2D ChunkSize(Meshes.Snow->ChunkSizeCm);
		const FVector2D ChunkMin = Tile.Bounds.Min + FVector2D(SnowChunk % Meshes.Snow->ChunksPerSide, SnowChunk / Meshes.Snow->ChunksPerSide) * ChunkSize;
		Actor->AddSnowChunk(MoveTemp(Meshes.Snow->Chunks[SnowChunk]), FBox2D(ChunkMin, ChunkMin + ChunkSize), FindMaterial(TEXT("Snow_Layer")));
	}
	else
	{
		if (bNear)
		{
			Actor->AddTrunkColliders(Meshes.TrunkBases, Meshes.TrunkDiameters);
		}
		if (AWorldTileActor* Previous = Tile.Actor.Get())
		{
			Previous->Destroy();
		}
		Tile.Actor = Actor;
		Tile.ShownDetail = Build.Detail;
		Tile.PendingDetail = INDEX_NONE;
		Tile.bHasSnowMesh = Meshes.Snow.IsValid();
		bDone = true;
	}
	const double StepSeconds = FPlatformTime::Seconds() - StartTime;
	if (StepSeconds > 0.008)
	{
		UE_LOG(LogWorldStreamer, Log, TEXT("Tile %s detail %d: step %d of %d took %.1f ms (furniture part %d)"), *FPaths::GetBaseFilename(Tile.Path), Build.Detail,
			Step, ChunkSteps + PlantSteps + SnowSteps + 5, StepSeconds * 1000.0, Job.FurnitureStep - 1);
	}
	Job.SpawnSeconds += StepSeconds;
	Job.LongestStepSeconds = FMath::Max(Job.LongestStepSeconds, StepSeconds);
	if (bDone)
	{
		UE_LOG(LogWorldStreamer, Log, TEXT("Tile %s detail %d: built in %.0f ms, spawned in %.1f ms over %d steps, longest step %.1f ms"),
			*FPaths::GetBaseFilename(Tile.Path), Build.Detail, Build.BuildSeconds * 1000.0, Job.SpawnSeconds * 1000.0, Job.NextStep,
			Job.LongestStepSeconds * 1000.0);
	}
	return bDone;
}

void AWorldStreamer::RunSpawnJobs(double BudgetSeconds, bool bCookNow)
{
	const double StartTime = FPlatformTime::Seconds();
	do
	{
		if (SpawnJobs.IsEmpty())
		{
			return;
		}
		if (RunSpawnStep(*SpawnJobs[0], bCookNow))
		{
			SpawnJobs.RemoveAt(0);
		}
	}
	while (FPlatformTime::Seconds() - StartTime < BudgetSeconds);
}

void AWorldStreamer::EnableCollisionNear(const FVector& Location, int32 MaxChunks, bool bCookNow)
{
	const FVector2D Viewer(Location);
	for (int32 Enabled = 0; Enabled < MaxChunks; ++Enabled)
	{
		AWorldTileActor* BestActor = nullptr;
		int32 BestChunk = INDEX_NONE;
		double BestDistance = CollisionDistance;
		for (const FTileState& Tile : Tiles)
		{
			AWorldTileActor* Actor = Tile.Actor.Get();
			if (!Actor || Tile.ShownDetail != int32(EWorldTileDetail::Near))
			{
				continue;
			}
			double Distance = 0.0;
			const int32 Chunk = Actor->FindChunkNeedingCollision(Viewer, BestDistance, Distance);
			if (Chunk != INDEX_NONE)
			{
				BestActor = Actor;
				BestChunk = Chunk;
				BestDistance = Distance;
			}
		}
		if (!BestActor)
		{
			return;
		}
		BestActor->EnableChunkCollision(BestChunk, bCookNow);
	}
}

void AWorldStreamer::EnableFurnitureCollisionNear(const FVector& Location, int32 MaxComponents)
{
	const FVector2D Viewer(Location);
	int32 Enabled = 0;
	for (const FTileState& Tile : Tiles)
	{
		AWorldTileActor* Actor = Tile.Actor.Get();
		if (!Actor || Tile.ShownDetail != int32(EWorldTileDetail::Near) || DistanceToBox2D(Tile.Bounds, Viewer) > CollisionDistance)
		{
			continue;
		}
		while (Enabled < MaxComponents && Actor->EnableNextFurnitureCollision())
		{
			++Enabled;
		}
		if (Enabled >= MaxComponents)
		{
			return;
		}
	}
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
		const bool bNeedsSnowMesh = bSnowWanted && Wanted == int32(EWorldTileDetail::Near) && Tile.ShownDetail == Wanted && !Tile.bHasSnowMesh;
		if (bNeedsSnowMesh && Tile.PendingDetail == INDEX_NONE && Tile.FailedDetail != Wanted)
		{
			Order.Add(Index); // snow started falling: mesh the tile again, this time with its snow layer
		}
		else if (Wanted != Tile.ShownDetail && Wanted != Tile.PendingDetail && Wanted != Tile.FailedDetail)
		{
			Order.Add(Index);
		}
	}
	Order.Sort([&](int32 A, int32 B) { return DistanceToBox2D(Tiles[A].Bounds, Viewer) < DistanceToBox2D(Tiles[B].Bounds, Viewer); });
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
		Builds[Slot] = RunBuild(Needed[Slot], Tile.PendingDetail, Tile.Path, Tile.Data, Context, bSnowWanted);
	});
	for (const TSharedPtr<FWorldTileBuild>& Build : Builds)
	{
		AcceptBuild(Build);
	}
	RunSpawnJobs(/*BudgetSeconds=*/MAX_dbl, /*bCookNow=*/true);
	EnableCollisionNear(Location, MAX_int32, /*bCookNow=*/true);
	EnableFurnitureCollisionNear(Location, MAX_int32);
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

void AWorldStreamer::UpdateGrass(const FVector& Location)
{
	if (bGrassFailed || FParse::Param(FCommandLine::Get(), TEXT("NoGrass")))
	{
		return;
	}
	if (!Grass)
	{
		Grass = MakeShared<FGrassField>(this);
		bGrassFailed = !Grass->LoadAssets();
		if (bGrassFailed)
		{
			Grass.Reset();
			return;
		}
	}
	Grass->Update(Location, [this](const FBox2D& RectangleCm, TArray<FGrassTileSource>& OutTiles)
	{
		for (const FTileState& Tile : Tiles)
		{
			if (Tile.bHorizon || !Tile.Data.IsValid() || !Tile.Bounds.Intersect(RectangleCm))
			{
				continue;
			}
			FGrassTileSource& Source = OutTiles.AddDefaulted_GetRef();
			Source.BoundsCm = Tile.Bounds;
			Source.Data = Tile.Data;
		}
	});
}

void AWorldStreamer::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Grass)
	{
		Grass->Clear();
	}
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
	bool bNewJobs = false;
	while (Shared->Finished.Dequeue(Finished))
	{
		--BuildsInFlight;
		bNewJobs |= AcceptBuild(Finished);
	}
	FVector Location;
	const bool bHasViewer = GetViewerLocation(Location);
	if (bHasViewer && bNewJobs)
	{
		// Nearest tiles first; a job that has started keeps its place so its actor isn't left half built.
		const FVector2D Viewer(Location);
		const int32 First = SpawnJobs.Num() > 0 && SpawnJobs[0]->NextStep > 0 ? 1 : 0;
		Algo::Sort(MakeArrayView(SpawnJobs).Slice(First, SpawnJobs.Num() - First), [&](const TSharedPtr<FTileSpawnJob>& A, const TSharedPtr<FTileSpawnJob>& B)
		{
			return DistanceToBox2D(Tiles[A->Build->TileIndex].Bounds, Viewer) < DistanceToBox2D(Tiles[B->Build->TileIndex].Bounds, Viewer);
		});
	}
	RunSpawnJobs(SpawnBudgetMs / 1000.0, /*bCookNow=*/false);
	if (bHasViewer)
	{
		EnableCollisionNear(Location, 1, /*bCookNow=*/false);
		EnableFurnitureCollisionNear(Location, 1);
	}
	if (bHasViewer)
	{
		UpdateGrass(Location);
	}
	SecondsSinceUpdate += DeltaSeconds;
	if (bHasViewer && SecondsSinceUpdate >= UpdateIntervalSeconds)
	{
		SecondsSinceUpdate = 0.f;
		bSnowWanted = ReadWorldSnowCover(GetWorld()) > SnowMeshWantedAbove || FParse::Param(FCommandLine::Get(), TEXT("ForceSnowMesh"));
		UpdateWanted(Location);
	}
}
