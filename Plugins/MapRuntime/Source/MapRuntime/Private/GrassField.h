#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
struct FWorldTileData;

/**
 * Where grass may grow in one world tile: weights for lawn and for meadow on a 0.5 m raster, zero on roads,
 * pavements, paths, water and under buildings, and in fields and forests.
 */
struct FGrassMask
{
	static constexpr float CellMetres = 0.5f;
	int32 NumX = 0;
	int32 NumY = 0;
	TArray<uint8> Lawn;
	TArray<uint8> Meadow;
};

/** A tile as the grass sees it: its rectangle in world cm and its data. */
struct FGrassTileSource
{
	FBox2D BoundsCm = FBox2D(ForceInit);
	TSharedPtr<const FWorldTileData> Data;
};

/**
 * Real 3D grass tufts scattered around the viewer on lawns and meadows (the ground texture alone looks flat close up).
 * The world is cut into square cells; cells near the viewer get instanced tufts, computed on worker threads from the
 * tiles' land cover and heights, and cells that fall out of range are removed again. Near the edge of its radius each
 * tuft shrinks away in the material, so nothing pops. Owned by AWorldStreamer, which feeds it tile data.
 */
class FGrassField
{
public:
	/** Finds the tiles overlapping a world rectangle; tiles without data are left out. */
	using FTileFinder = TFunction<void(const FBox2D& RectangleCm, TArray<FGrassTileSource>& OutTiles)>;

	explicit FGrassField(AActor* InOwner);
	~FGrassField();

	/** Loads the tuft meshes and materials; false (grass stays off) when they are missing. */
	bool LoadAssets();

	/** Follows the viewer: starts and finishes cells, removes the far ones. Call every frame. */
	void Update(const FVector& ViewerCm, const FTileFinder& FindTiles);

	/** Removes everything. */
	void Clear();

private:
	/** Which land cover weight a kind grows on. */
	enum class ESource : uint8
	{
		Lawn,
		Meadow,
		Seam,
	};

	/** One kind of grass: how dense, how far, how big, and which tuft meshes it draws from. */
	struct FKind
	{
		FName Name;
		ESource Source = ESource::Lawn;
		/** Above 0: the kind only grows in patches (flowers), this share of the area; 0 = everywhere. */
		float PatchShare = 0.f;
		/** How strongly slow noise varies the height of the tufts (0 = even, 0.5 = by up to +-50 %). */
		float Unevenness = 0.f;
		float SpacingCm = 30.f;
		float RadiusCm = 2000.f;
		FVector2f ScaleXY = FVector2f(1.f, 1.f);
		FVector2f ScaleZ = FVector2f(1.f, 1.f);
		TArray<TObjectPtr<UStaticMesh>> Meshes;
		TObjectPtr<UMaterialInterface> Material;
	};

	/** Transforms for each mesh of each kind in one cell, produced by a worker. */
	struct FCellResult
	{
		TArray<TArray<TArray<FTransform>>> TransformsByKindAndMesh;
	};

	struct FCell
	{
		TArray<TObjectPtr<UInstancedStaticMeshComponent>> Components;
		bool bPending = false;
		/** Bit per kind that the built components include; a cell is rebuilt when the viewer comes within a kind's radius. */
		uint32 BuiltKinds = 0;
	};

	struct FPendingCell
	{
		FIntPoint Key;
		uint32 Kinds = 0;
		TFuture<TSharedPtr<FCellResult>> Result;
	};

	/** Mask for a tile, started on a worker the first time it is asked for. Null while still being built. */
	TSharedPtr<const FGrassMask> FindMask(const FGrassTileSource& Tile);

	/** Sends a cell to the workers, with the kinds in the bit set KindBits, when all tiles it touches have data and masks. */
	void StartCell(const FIntPoint& Key, uint32 KindBits, const FTileFinder& FindTiles);

	/** Turns a finished worker result into components. */
	void FinishCell(const FIntPoint& Key, uint32 KindBits, const FCellResult& Result);

	void RemoveCell(FCell& Cell);

	TWeakObjectPtr<AActor> Owner;
	TArray<FKind> Kinds;
	TMap<FIntPoint, FCell> Cells;
	TArray<FPendingCell> Pending;
	/** Masks by tile corner in metres; the data pointer tells a reloaded tile from the one the mask was made for. */
	struct FMaskEntry
	{
		const FWorldTileData* DataKey = nullptr;
		TSharedFuture<TSharedPtr<const FGrassMask>> Mask;
	};
	TMap<FIntPoint, FMaskEntry> Masks;
	FVector LastViewerCm = FVector(1e12);
	double LastRefreshSeconds = 0.0;
	/** Set while wanted cells are left to start, so the next frame looks again instead of waiting for the interval. */
	bool bCellsWaiting = false;
	bool bAssetsLoaded = false;
};
