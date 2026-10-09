#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WorldStreamer.generated.h"

class AWorldTileActor;
class UMaterialInterface;
class UStaticMesh;
struct FWorldStreamerShared;
struct FWorldTileBuild;

/**
 * Generates the world around the viewer from compiled world data tiles (Tools/osmimport/build_world.py) while
 * playing: tiles are read and meshed on worker threads at a detail level that depends on their distance, then
 * spawned as AWorldTileActors a few per frame, and dropped again when they fall out of range.
 *
 * The data is read from <data root>/world/<Region>/ (data root: $THIRD_GEAR_DATA, else the project's External link).
 * -Region=<name> on the command line overrides the region.
 */
UCLASS()
class MAPRUNTIME_API AWorldStreamer : public AActor
{
	GENERATED_BODY()

public:
	AWorldStreamer();

	/** Region folder under <data root>/world. */
	UPROPERTY(EditAnywhere, Category = "World")
	FString Region = TEXT("bergedorf_test");

	/** Tiles closer than this (to their nearest point) get full detail and collision, cm. */
	UPROPERTY(EditAnywhere, Category = "World")
	float NearDistance = 40000.f;

	/** Tiles closer than this get the middle detail level, cm. */
	UPROPERTY(EditAnywhere, Category = "World")
	float MiddleDistance = 120000.f;

	/** Tiles closer than this get the far detail level; anything further is not loaded, cm. */
	UPROPERTY(EditAnywhere, Category = "World")
	float FarDistance = 300000.f;

	/** Folder of the surface materials, named M_<section>. */
	UPROPERTY(EditAnywhere, Category = "World")
	FString MaterialFolder = TEXT("/Game/World/Materials");

	/** Plant model meshes by the model names in the world data. */
	UPROPERTY(EditAnywhere, Category = "World")
	TMap<FString, TSoftObjectPtr<UStaticMesh>> PlantModels;

	/** Tiles meshed in parallel at most. */
	UPROPERTY(EditAnywhere, Category = "World")
	int32 MaxBuildsInFlight = 4;

	/** Finished tiles spawned per frame at most, to keep frame times even. */
	UPROPERTY(EditAnywhere, Category = "World")
	int32 MaxSpawnsPerFrame = 1;

	/** Where the world data says the drive starts (on a road, facing along it), at eye height. False without data. */
	bool GetStartTransform(FTransform& OutTransform);

	/** Loads every tile needed around Location right now, with collision ready, before returning. */
	void LoadAroundBlocking(const FVector& Location);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

private:
	struct FTileState
	{
		FString Path;
		/** Tile rectangle in world cm. */
		FBox2D Bounds;
		/** Parsed data, kept after the first load so detail changes only re-mesh. */
		TSharedPtr<const struct FWorldTileData> Data;
		/** Detail level currently shown, or -1. */
		int32 ShownDetail = INDEX_NONE;
		/** Detail level being built, or -1. */
		int32 PendingDetail = INDEX_NONE;
		/** Detail level of the last failed attempt, so a broken tile isn't retried every frame. */
		int32 FailedDetail = INDEX_NONE;
		TWeakObjectPtr<AWorldTileActor> Actor;
	};

	/** Reads world.json the first time it is needed; false when it is missing. */
	bool EnsureIndex();

	/** Resolves materials and plant models and measures the models; called once before the first build. */
	void PrepareAssets();

	/** Detail level the tile should have for a viewer at Location (cm), or -1 to unload it. */
	int32 WantedDetail(const FTileState& Tile, const FVector2D& Location) const;

	/** Starts meshing a tile on a worker thread. */
	void StartBuild(int32 TileIndex, int32 Detail);

	/** Replaces a tile's actor with the finished build. */
	void ApplyBuild(FWorldTileBuild& Build, bool bCookNow);

	void Unload(FTileState& Tile);

	/** Location the world is generated around: the player's camera, else the first player start. */
	bool GetViewerLocation(FVector& OutLocation) const;

	/** Brings every tile to the detail it should have for Location: starts builds and unloads, no spawning. */
	void UpdateWanted(const FVector& Location);

	UMaterialInterface* FindMaterial(const FString& Section);

	TArray<FTileState> Tiles;
	FString WorldDir;
	TSharedPtr<class FJsonObject> Start;
	TSharedPtr<FWorldStreamerShared> Shared;
	TSharedPtr<const struct FWorldMeshingContext> MeshingContext;
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInterface>> Materials;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UStaticMesh>> LoadedPlantModels;

	/** Finished builds waiting to be spawned, nearest first. */
	TArray<TSharedPtr<FWorldTileBuild>> ReadyBuilds;
	float SecondsSinceUpdate = 0.f;
	int32 BuildsInFlight = 0;
	bool bIndexLoaded = false;
	bool bIndexFailed = false;
};
