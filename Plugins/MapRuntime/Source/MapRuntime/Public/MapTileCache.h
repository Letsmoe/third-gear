#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "MapPalette.h"
#include "UObject/GCObject.h"
#include "WorldTileData.h"

struct FSlateBrush;
class UTexture2D;

/** Filled triangles of one map layer, in metres relative to the tile corner. */
struct FMapTileLayer
{
	TArray<FVector2f> Vertices;
	/** Triangle corners; 32 bit like Slate's indices on desktop platforms. */
	TArray<uint32> Indices;
};

/**
 * Thin lines that keep the same width on screen at any zoom: each point carries a unit normal, and the painter offsets
 * every point by plus and minus half the wanted width along it. Points are tile-relative metres; Indices triangulate
 * the strip when the painter's vertices are laid out as (point + offset, point - offset) per point.
 */
struct FMapTileStrips
{
	TArray<FVector2f> Points;
	TArray<FVector2f> Normals;
	TArray<uint32> Indices;
};

/**
 * One world tile reduced to what a map needs. Positions are metres relative to the tile corner (OriginM, world metres,
 * x east, y south). The 2D maps draw the land cover image, the water, footpath outlines and building fills; the 3D map
 * builds its blocks from BuildingRecords.
 */
struct MAPRUNTIME_API FMapTile
{
	FIntPoint Key = FIntPoint::ZeroValue;
	FVector2f OriginM = FVector2f::ZeroVector;
	float SizeM = 250.f;

	/** Land cover as a low resolution image in the palette's colours, drawn bilinear over the tile; null until the tile has data. */
	TObjectPtr<UTexture2D> Landcover;
	TSharedPtr<FSlateBrush> LandcoverBrush;
	/** Meadow, field and forest weights (0..255) per image pixel, kept so the image can be recoloured for another palette. */
	TArray<uint8> CoverWeights;
	/** The cache's palette version the image was last coloured for. */
	int32 ColorVersion = -1;

	FMapTileLayer Water;
	/** Building fills by EMapBuildingStyle. */
	FMapTileLayer Buildings[3];
	/** Outlines of pavements and footpaths as thin lines. */
	FMapTileStrips Footpaths;
	/** True once the building fills were built. */
	bool bHasBuildings = false;

	/**
	 * Every building of the tile as read from the file, for the 3D map: footprint rings (tile-relative metres, outline
	 * first, then holes), BaseZ and EaveHeight (absolute and above base, metres), RoofShape and the BLDG roof rectangle,
	 * the BTYP typing (ClassId, TypedRoofShape, PitchDegrees, Storeys, TypedEaveHeight, RidgeYaw...) and the skeleton roof
	 * faces and caps of the ROOF section. Only filled for tiles requested with bBuildingRecords.
	 */
	TArray<FWorldBuilding> BuildingRecords;
	bool bHasBuildingRecords = false;

	double LastUsedSeconds = 0.0;
};

/** What a map wants loaded: a circle around a centre (world metres) and which building data it needs. */
struct FMapTileRequest
{
	FVector2f CentreM = FVector2f::ZeroVector;
	float RadiusM = 0.f;
	/** Building fills for the 2D maps. */
	bool bBuildings = false;
	/** The full building records for the 3D map (implies bBuildings). */
	bool bBuildingRecords = false;
	/** Colours of the land cover images; a change recolours the visible tiles. */
	const FMapPalette* Palette = nullptr;
};

/**
 * Map data for any part of the region, independent of the streamer: tiles are read from the .tgtile files on worker
 * threads (only the sections a map needs), reduced to FMapTile, and dropped again when unused for a while. Buildings
 * are the expensive part and are only built for tiles requested with them.
 */
class MAPRUNTIME_API FMapTileCache : public FGCObject
{
public:
	virtual ~FMapTileCache() override;

	/** Starts reading the region's tile index (world.json in WorldDir). */
	void Initialize(const FString& WorldDir);

	/**
	 * Game thread, call about once per frame while a map is shown: takes finished tiles, starts loading the missing
	 * ones nearest to the centre first, recolours land cover images for a changed palette, and evicts tiles unused for
	 * a long time. A tile loaded without buildings is loaded again when a request needs them.
	 */
	void Update(const FMapTileRequest& Request);

	/** The loaded tiles that touch the circle around CentreM (world metres), in no particular order. */
	void GetTilesInView(const FVector2f& CentreM, float RadiusM, TArray<const FMapTile*>& OutTiles) const;

	int32 GetLoadedTileCount() const { return Tiles.Num(); }
	int32 GetPendingTileCount() const { return Pending.Num(); }

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FMapTileCache"); }

private:
	/** The result of a worker's read of one tile, before it gets its texture on the game thread. */
	struct FBuiltTile
	{
		TUniquePtr<FMapTile> Tile;
		bool bFailed = false;
	};

	struct FPendingTile
	{
		TFuture<TSharedPtr<FBuiltTile>> Result;
	};

	/** Reads the tile index once its worker has finished. */
	void PollIndex();

	/** Moves finished tile loads into the cache. */
	void CollectFinished();

	/** Whether a tile needs a (new) load for the request. */
	bool NeedsLoad(const FIntPoint& Key, const FMapTileRequest& Request) const;

	void StartLoad(const FIntPoint& Key, const FMapTileRequest& Request);

	/** Colours the tile's land cover image for the current palette, creating the texture on first use; game thread. */
	void ColorLandcover(FMapTile& Tile);

	void Evict();

	static constexpr float TileSizeM = 250.f;

	TFuture<TMap<FIntPoint, FString>> IndexLoad;
	bool bIndexLoadStarted = false;
	TMap<FIntPoint, FString> TilePaths;

	/** The palette the land cover images are coloured with; its version counts up with every change. */
	const FMapPalette* Palette = nullptr;
	int32 PaletteVersion = 0;

	TMap<FIntPoint, TUniquePtr<FMapTile>> Tiles;
	TMap<FIntPoint, FPendingTile> Pending;
};
