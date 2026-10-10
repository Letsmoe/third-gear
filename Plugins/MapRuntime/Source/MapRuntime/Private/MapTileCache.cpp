#include "MapTileCache.h"

#include "Async/Async.h"
#include "ConstrainedDelaunay2.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MinimapIndex.h"
#include "Polygon2.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateBrush.h"
#include "WorldTileData.h"

using namespace UE::Geometry;

DEFINE_LOG_CATEGORY_STATIC(LogMapTileCache, Log, All);

namespace
{
/** Pixels along one side of a tile's land cover image (about 4 m per pixel). */
constexpr int32 LandcoverResolution = 64;

/** Tiles loading at the same time. */
constexpr int32 MaxTilesInFlight = 6;

/** The cache drops its least recently used tiles above this count, but never ones used in the last seconds. */
constexpr int32 MaxCachedTiles = 1400;
constexpr double KeepSeconds = 5.0;

/** Land cover images recoloured per Update after a palette change, so the change spreads over a few frames. */
constexpr int32 MaxRecoloursPerUpdate = 80;

/** Most vertices of one layer, so one odd tile cannot use unbounded memory. */
constexpr int32 MaxLayerVertices = 400000;

FColor MixColors(const FColor& From, const FColor& To, float Amount)
{
	const auto Channel = [Amount](uint8 A, uint8 B) { return static_cast<uint8>(FMath::RoundToInt(FMath::Lerp(float(A), float(B), Amount))); };
	return FColor(Channel(From.R, To.R), Channel(From.G, To.G), Channel(From.B, To.B), 255);
}

/** Colour of one land cover pixel from its meadow, field and forest weights. */
FColor LandcoverColor(const FMapPalette& Palette, const uint8* Weights)
{
	FColor Color = MixColors(Palette.Land, Palette.Meadow, Weights[0] / 255.f);
	Color = MixColors(Color, Palette.Field, Weights[1] / 255.f);
	return MixColors(Color, Palette.Forest, Weights[2] / 255.f);
}

/** The tile's land cover grid reduced to LandcoverResolution squared pixels of three weights. */
void BuildCoverWeights(const FWorldTileGrid& Grid, TArray<uint8>& OutWeights)
{
	OutWeights.SetNumUninitialized(LandcoverResolution * LandcoverResolution * 3);
	for (int32 PixelY = 0; PixelY < LandcoverResolution; ++PixelY)
	{
		const int32 GridY = FMath::Clamp(FMath::RoundToInt((PixelY + 0.5f) / LandcoverResolution * (Grid.NumY - 1)), 0, Grid.NumY - 1);
		for (int32 PixelX = 0; PixelX < LandcoverResolution; ++PixelX)
		{
			const int32 GridX = FMath::Clamp(FMath::RoundToInt((PixelX + 0.5f) / LandcoverResolution * (Grid.NumX - 1)), 0, Grid.NumX - 1);
			FMemory::Memcpy(&OutWeights[(PixelY * LandcoverResolution + PixelX) * 3], &Grid.Cover[(GridY * Grid.NumX + GridX) * 3], 3);
		}
	}
}

/** Even-odd test against all rings: inside the outline and outside every hole. */
bool IsInside(const FWorldPolygon& Polygon, const FVector2d& Point)
{
	bool bInside = false;
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		for (int32 Index = 0, Previous = Ring.Num() - 1; Index < Ring.Num(); Previous = Index++)
		{
			const FVector2f& A = Ring[Index];
			const FVector2f& B = Ring[Previous];
			if ((A.Y > Point.Y) != (B.Y > Point.Y) && Point.X < (B.X - A.X) * (Point.Y - A.Y) / (B.Y - A.Y) + A.X)
			{
				bInside = !bInside;
			}
		}
	}
	return bInside;
}

/** Adds the filled polygon, holes left open, to a layer. */
void AddPolygon(const FWorldPolygon& Polygon, FMapTileLayer& Layer)
{
	TConstrainedDelaunay2<double> Triangulator;
	Triangulator.bOrientedEdges = false;
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		if (Ring.Num() < 3)
		{
			continue;
		}
		TArray<FVector2d> Points;
		Points.Reserve(Ring.Num());
		for (const FVector2f& Point : Ring)
		{
			Points.Add(FVector2d(Point));
		}
		Triangulator.Add(TPolygon2<double>(Points));
	}
	const bool bOk = Triangulator.Triangulate([&Polygon](const TArray<FVector2d>& Vertices, const FIndex3i& Triangle)
	{
		return IsInside(Polygon, (Vertices[Triangle.A] + Vertices[Triangle.B] + Vertices[Triangle.C]) / 3.0);
	});
	if (!bOk || Layer.Vertices.Num() + Triangulator.Vertices.Num() > MaxLayerVertices)
	{
		return;
	}
	const int32 First = Layer.Vertices.Num();
	for (const FVector2d& Vertex : Triangulator.Vertices)
	{
		Layer.Vertices.Add(FVector2f(Vertex));
	}
	for (const FIndex3i& Triangle : Triangulator.Triangles)
	{
		Layer.Indices.Add(static_cast<uint32>(First + Triangle.A));
		Layer.Indices.Add(static_cast<uint32>(First + Triangle.B));
		Layer.Indices.Add(static_cast<uint32>(First + Triangle.C));
	}
}

/** How far a footpath outline may stray when simplified, and the smallest ring (bounding box edge) that is kept, metres. */
constexpr float FootpathToleranceM = 0.4f;
constexpr float FootpathMinimumExtentM = 3.f;

/** Unit normal of a segment, pointing to the left of its direction. */
FVector2f SegmentNormal(const FVector2f& From, const FVector2f& To)
{
	return FVector2f(-(To.Y - From.Y), To.X - From.X).GetSafeNormal();
}

/** Adds a closed ring as a strip, with mitred normals at the corners (limited, so sharp corners do not spike). */
void AddStripRing(const TArray<FVector2f>& Ring, FMapTileStrips& Strips)
{
	constexpr float MaximumMitre = 2.f;
	const int32 Count = Ring.Num();
	const int32 First = Strips.Points.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2f& Previous = Ring[(Index + Count - 1) % Count];
		const FVector2f& Next = Ring[(Index + 1) % Count];
		const FVector2f Before = SegmentNormal(Previous, Ring[Index]);
		const FVector2f After = SegmentNormal(Ring[Index], Next);
		FVector2f Normal = (Before + After).GetSafeNormal();
		const float Cosine = FVector2f::DotProduct(Normal, After);
		Normal *= FMath::Min(MaximumMitre, 1.f / FMath::Max(Cosine, 0.01f));
		Strips.Points.Add(Ring[Index]);
		Strips.Normals.Add(Normal);
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const uint32 A = 2 * (First + Index);
		const uint32 B = 2 * (First + (Index + 1) % Count);
		Strips.Indices.Append({A, A + 1, B, A + 1, B + 1, B});
	}
}

/** Adds a polygon's rings as thin closed lines, simplified, skipping tiny ones. */
void AddOutlines(const FWorldPolygon& Polygon, FMapTileStrips& Strips)
{
	TArray<FVector2f> Simplified;
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		TArray<FVector2f> Closed = Ring;
		Closed.Add(Ring[0]);
		MinimapGeometry::SimplifyLine(Closed, Simplified, FootpathToleranceM);
		Simplified.Pop();
		const FBox2f Bounds(Simplified);
		if (Simplified.Num() >= 3 && FMath::Max(Bounds.GetSize().X, Bounds.GetSize().Y) >= FootpathMinimumExtentM)
		{
			AddStripRing(Simplified, Strips);
		}
	}
}

bool IsFootpathMaterial(const FString& Name)
{
	return Name == TEXT("Pavement") || Name == TEXT("Path_Paved") || Name == TEXT("Path_Gravel");
}

/** Water fills and footpath outlines from the surface polygons. */
void AddSurfaces(const FWorldTileData& Data, FMapTile& Tile)
{
	for (const FWorldSurface& Surface : Data.Surfaces)
	{
		if (!Data.Names.IsValidIndex(Surface.Material))
		{
			continue;
		}
		const FString& Name = Data.Names[Surface.Material];
		if (Name == TEXT("Water"))
		{
			AddPolygon(Surface.Polygon, Tile.Water);
		}
		else if (IsFootpathMaterial(Name))
		{
			AddOutlines(Surface.Polygon, Tile.Footpaths);
		}
	}
}

/** Public and retail buildings get the commercial fill and industrial halls the industrial one (building_types.py ids); shop houses stay residential. */
EMapBuildingStyle StyleOfBuilding(const FWorldBuilding& Building)
{
	constexpr uint8 IndustrialHall = 12;
	constexpr uint8 Public = 13;
	constexpr uint8 RetailCentre = 14;
	if (!Building.bTyped)
	{
		return EMapBuildingStyle::Residential;
	}
	if (Building.ClassId == IndustrialHall)
	{
		return EMapBuildingStyle::Industrial;
	}
	const bool bCommercial = Building.ClassId == Public || Building.ClassId == RetailCentre;
	return bCommercial ? EMapBuildingStyle::Commercial : EMapBuildingStyle::Residential;
}

void AddBuildings(FWorldTileData& Data, FMapTile& Tile, bool bKeepRecords)
{
	for (const FWorldBuilding& Building : Data.Buildings)
	{
		AddPolygon(Building.Footprint, Tile.Buildings[static_cast<int32>(StyleOfBuilding(Building))]);
	}
	Tile.bHasBuildings = true;
	if (bKeepRecords)
	{
		Tile.BuildingRecords = MoveTemp(Data.Buildings);
		Tile.bHasBuildingRecords = true;
	}
}
}

FMapTileCache::~FMapTileCache()
{
	if (IndexLoad.IsValid())
	{
		IndexLoad.Wait();
	}
	for (TPair<FIntPoint, FPendingTile>& Entry : Pending)
	{
		Entry.Value.Result.Wait();
	}
}

void FMapTileCache::Initialize(const FString& WorldDir)
{
	if (bIndexLoadStarted)
	{
		return;
	}
	bIndexLoadStarted = true;
	IndexLoad = Async(EAsyncExecution::ThreadPool, [WorldDir]()
	{
		TMap<FIntPoint, FString> Paths;
		FString Json;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(WorldDir, TEXT("world.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			return Paths;
		}
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("tiles")))
		{
			const TSharedPtr<FJsonObject>& Entry = Value->AsObject();
			const TArray<TSharedPtr<FJsonValue>>& Bounds = Entry->GetArrayField(TEXT("bounds_m"));
			const FIntPoint Key(FMath::RoundToInt(Bounds[0]->AsNumber() / TileSizeM), FMath::RoundToInt(Bounds[1]->AsNumber() / TileSizeM));
			Paths.Add(Key, FPaths::Combine(WorldDir, Entry->GetStringField(TEXT("file"))));
		}
		return Paths;
	});
}

void FMapTileCache::PollIndex()
{
	if (IndexLoad.IsValid() && IndexLoad.IsReady())
	{
		TilePaths = IndexLoad.Get();
		IndexLoad = TFuture<TMap<FIntPoint, FString>>();
		UE_LOG(LogMapTileCache, Log, TEXT("Map tile index: %d tiles"), TilePaths.Num());
	}
}

void FMapTileCache::Update(const FMapTileRequest& Request)
{
	PollIndex();
	CollectFinished();
	if (Request.Palette && Request.Palette != Palette)
	{
		Palette = Request.Palette;
		++PaletteVersion;
	}
	if (TilePaths.Num() == 0)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const FVector2f Centre = Request.CentreM;
	const FIntPoint MinKey(FMath::FloorToInt((Centre.X - Request.RadiusM) / TileSizeM), FMath::FloorToInt((Centre.Y - Request.RadiusM) / TileSizeM));
	const FIntPoint MaxKey(FMath::FloorToInt((Centre.X + Request.RadiusM) / TileSizeM), FMath::FloorToInt((Centre.Y + Request.RadiusM) / TileSizeM));
	TArray<TPair<float, FIntPoint>> Wanted;
	int32 Recoloured = 0;
	for (int32 KeyY = MinKey.Y; KeyY <= MaxKey.Y; ++KeyY)
	{
		for (int32 KeyX = MinKey.X; KeyX <= MaxKey.X; ++KeyX)
		{
			const FIntPoint Key(KeyX, KeyY);
			if (TUniquePtr<FMapTile>* Existing = Tiles.Find(Key))
			{
				(*Existing)->LastUsedSeconds = Now;
				if ((*Existing)->ColorVersion != PaletteVersion && Recoloured < MaxRecoloursPerUpdate)
				{
					ColorLandcover(**Existing);
					++Recoloured;
				}
			}
			if (TilePaths.Contains(Key) && NeedsLoad(Key, Request))
			{
				const FVector2f TileCentre((KeyX + 0.5f) * TileSizeM, (KeyY + 0.5f) * TileSizeM);
				Wanted.Emplace(FVector2f::DistSquared(TileCentre, Centre), Key);
			}
		}
	}
	Wanted.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	for (const TPair<float, FIntPoint>& Candidate : Wanted)
	{
		if (Pending.Num() >= MaxTilesInFlight)
		{
			break;
		}
		StartLoad(Candidate.Value, Request);
	}
	Evict();
}

bool FMapTileCache::NeedsLoad(const FIntPoint& Key, const FMapTileRequest& Request) const
{
	if (Pending.Contains(Key))
	{
		return false;
	}
	const TUniquePtr<FMapTile>* Existing = Tiles.Find(Key);
	if (!Existing)
	{
		return true;
	}
	const bool bNeedsFills = (Request.bBuildings || Request.bBuildingRecords) && !(*Existing)->bHasBuildings;
	const bool bNeedsRecords = Request.bBuildingRecords && !(*Existing)->bHasBuildingRecords;
	return bNeedsFills || bNeedsRecords;
}

void FMapTileCache::StartLoad(const FIntPoint& Key, const FMapTileRequest& Request)
{
	const FString Path = TilePaths[Key];
	const bool bWithBuildings = Request.bBuildings || Request.bBuildingRecords;
	const bool bWithRecords = Request.bBuildingRecords;
	FPendingTile& Entry = Pending.Add(Key);
	Entry.Result = Async(EAsyncExecution::ThreadPool, [Path, Key, bWithBuildings, bWithRecords]()
	{
		TSharedPtr<FBuiltTile> Built = MakeShared<FBuiltTile>();
		Built->Tile = MakeUnique<FMapTile>();
		FMapTile& Tile = *Built->Tile;
		Tile.Key = Key;
		Tile.OriginM = FVector2f(Key.X * TileSizeM, Key.Y * TileSizeM);
		Tile.SizeM = TileSizeM;
		EWorldTileSections Sections = EWorldTileSections::Names | EWorldTileSections::Grid | EWorldTileSections::Surfaces;
		if (bWithBuildings)
		{
			Sections |= EWorldTileSections::Buildings | EWorldTileSections::BuildingTypes;
		}
		if (bWithRecords)
		{
			Sections |= EWorldTileSections::Roofs;
		}
		FWorldTileData Data;
		FString Error;
		if (!FWorldTileData::Load(Path, Data, Error, Sections))
		{
			UE_LOG(LogMapTileCache, Warning, TEXT("%s"), *Error);
			Built->bFailed = true;
			return Built;
		}
		BuildCoverWeights(Data.Grid, Tile.CoverWeights);
		AddSurfaces(Data, Tile);
		if (bWithBuildings)
		{
			AddBuildings(Data, Tile, bWithRecords);
		}
		return Built;
	});
}

void FMapTileCache::CollectFinished()
{
	TArray<FIntPoint> Finished;
	for (TPair<FIntPoint, FPendingTile>& Entry : Pending)
	{
		if (Entry.Value.Result.IsReady())
		{
			Finished.Add(Entry.Key);
		}
	}
	for (const FIntPoint& Key : Finished)
	{
		const TSharedPtr<FBuiltTile> Built = Pending[Key].Result.Get();
		Pending.Remove(Key);
		FMapTile& Tile = *Built->Tile;
		Tile.LastUsedSeconds = FPlatformTime::Seconds();
		if (const TUniquePtr<FMapTile>* Previous = Tiles.Find(Key))
		{
			// A reload for buildings: keep the image the old tile already had.
			Tile.Landcover = (*Previous)->Landcover;
			Tile.LandcoverBrush = (*Previous)->LandcoverBrush;
			Tile.ColorVersion = (*Previous)->ColorVersion;
		}
		if (!Built->bFailed)
		{
			ColorLandcover(Tile);
		}
		Tiles.Add(Key, MoveTemp(Built->Tile));
	}
}

void FMapTileCache::ColorLandcover(FMapTile& Tile)
{
	if (!Palette || Tile.CoverWeights.Num() != LandcoverResolution * LandcoverResolution * 3)
	{
		return;
	}
	if (!Tile.Landcover)
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(LandcoverResolution, LandcoverResolution, PF_B8G8R8A8);
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->SRGB = true;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->NeverStream = true;
		Tile.Landcover = Texture;
		Tile.LandcoverBrush = MakeShared<FSlateBrush>();
		Tile.LandcoverBrush->DrawAs = ESlateBrushDrawType::Image;
		Tile.LandcoverBrush->ImageSize = FVector2f(LandcoverResolution, LandcoverResolution);
		Tile.LandcoverBrush->SetResourceObject(Texture);
	}
	FColor* Destination = static_cast<FColor*>(Tile.Landcover->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Pixel = 0; Pixel < LandcoverResolution * LandcoverResolution; ++Pixel)
	{
		Destination[Pixel] = LandcoverColor(*Palette, &Tile.CoverWeights[Pixel * 3]);
	}
	Tile.Landcover->GetPlatformData()->Mips[0].BulkData.Unlock();
	Tile.Landcover->UpdateResource();
	Tile.ColorVersion = PaletteVersion;
}

void FMapTileCache::Evict()
{
	if (Tiles.Num() <= MaxCachedTiles)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	TArray<TPair<double, FIntPoint>> ByAge;
	for (const TPair<FIntPoint, TUniquePtr<FMapTile>>& Entry : Tiles)
	{
		if (Now - Entry.Value->LastUsedSeconds > KeepSeconds)
		{
			ByAge.Emplace(Entry.Value->LastUsedSeconds, Entry.Key);
		}
	}
	ByAge.Sort([](const TPair<double, FIntPoint>& A, const TPair<double, FIntPoint>& B) { return A.Key < B.Key; });
	const int32 Excess = Tiles.Num() - MaxCachedTiles;
	for (int32 Index = 0; Index < FMath::Min(Excess, ByAge.Num()); ++Index)
	{
		Tiles.Remove(ByAge[Index].Value);
	}
}

void FMapTileCache::GetTilesInView(const FVector2f& CentreM, float RadiusM, TArray<const FMapTile*>& OutTiles) const
{
	OutTiles.Reset();
	const FIntPoint MinKey(FMath::FloorToInt((CentreM.X - RadiusM) / TileSizeM), FMath::FloorToInt((CentreM.Y - RadiusM) / TileSizeM));
	const FIntPoint MaxKey(FMath::FloorToInt((CentreM.X + RadiusM) / TileSizeM), FMath::FloorToInt((CentreM.Y + RadiusM) / TileSizeM));
	for (int32 KeyY = MinKey.Y; KeyY <= MaxKey.Y; ++KeyY)
	{
		for (int32 KeyX = MinKey.X; KeyX <= MaxKey.X; ++KeyX)
		{
			if (const TUniquePtr<FMapTile>* Tile = Tiles.Find(FIntPoint(KeyX, KeyY)))
			{
				OutTiles.Add(Tile->Get());
			}
		}
	}
}

void FMapTileCache::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (TPair<FIntPoint, TUniquePtr<FMapTile>>& Entry : Tiles)
	{
		Collector.AddReferencedObject(Entry.Value->Landcover);
	}
}
