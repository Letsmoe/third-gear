#include "GrassField.h"

#include "Async/Async.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "WorldTileData.h"
#include "WorldTileMesher.h"

DEFINE_LOG_CATEGORY_STATIC(LogGrassField, Log, All);

namespace GrassFieldDetail
{
/** Cells are squares of this size, world cm. */
constexpr int32 CellCm = 1200;
/** A cell is dropped when it is further than the largest grass radius plus this. */
constexpr float DropMarginCm = 1500.f;
/** Cells are looked at again when the viewer moved this far, or after this many seconds (new tiles may have loaded). */
constexpr float RefreshDistanceCm = 400.f;
constexpr double RefreshSeconds = 0.5;
constexpr int32 MaxCellsInFlight = 2;
/** How far under the terrain the tufts' roots sit, cm: the 1 m terrain mesh is linear between vertices. */
constexpr float RootSinkCm = 2.f;

const TCHAR* TuftFolder = TEXT("/Game/Grass/Imported");

/** Hash of three integers to 32 random bits (PCG). */
uint32 HashCell(int32 X, int32 Y, uint32 Salt)
{
	uint32 Value = uint32(X) * 73856093u ^ uint32(Y) * 19349663u ^ Salt * 83492791u;
	Value = Value * 747796405u + 2891336453u;
	Value = ((Value >> ((Value >> 28u) + 4u)) ^ Value) * 277803737u;
	return (Value >> 22u) ^ Value;
}

/** Uniform number in [0, 1) from a hash and a stream index, so one point can draw several independent numbers. */
float Random01(uint32 Hash, uint32 Stream)
{
	uint32 Value = (Hash + Stream * 0x9E3779B9u) * 2654435761u;
	Value ^= Value >> 15;
	Value *= 2246822519u;
	Value ^= Value >> 13;
	return float(Value & 0xFFFFFF) / float(0x1000000);
}

/** Marks the raster cells whose centre lies inside the polygon (even-odd over all its rings). */
void RasterizePolygon(const FWorldPolygon& Polygon, int32 NumX, int32 NumY, TArray<uint8>& Excluded)
{
	const float Cell = FGrassMask::CellMetres;
	TArray<TArray<float>> Crossings;
	Crossings.SetNum(NumY);
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		for (int32 Index = 0; Index < Ring.Num(); ++Index)
		{
			const FVector2f& A = Ring[Index];
			const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
			if (A.Y == B.Y)
			{
				continue;
			}
			const float MinY = FMath::Min(A.Y, B.Y);
			const float MaxY = FMath::Max(A.Y, B.Y);
			// Rows whose centre y satisfies MinY <= y < MaxY.
			const int32 FirstRow = FMath::Max(0, FMath::CeilToInt(MinY / Cell - 0.5f));
			const int32 LastRow = FMath::Min(NumY - 1, FMath::CeilToInt(MaxY / Cell - 0.5f) - 1);
			for (int32 Row = FirstRow; Row <= LastRow; ++Row)
			{
				const float CentreY = (Row + 0.5f) * Cell;
				Crossings[Row].Add(A.X + (CentreY - A.Y) * (B.X - A.X) / (B.Y - A.Y));
			}
		}
	}
	for (int32 Row = 0; Row < NumY; ++Row)
	{
		TArray<float>& RowCrossings = Crossings[Row];
		RowCrossings.Sort();
		for (int32 Pair = 0; Pair + 1 < RowCrossings.Num(); Pair += 2)
		{
			const int32 FirstColumn = FMath::Max(0, FMath::CeilToInt(RowCrossings[Pair] / Cell - 0.5f));
			const int32 LastColumn = FMath::Min(NumX - 1, FMath::CeilToInt(RowCrossings[Pair + 1] / Cell - 0.5f) - 1);
			for (int32 Column = FirstColumn; Column <= LastColumn; ++Column)
			{
				Excluded[Row * NumX + Column] = 1;
			}
		}
	}
}

/** Bilinear land cover weights (meadow, field, forest; 0..1) at a tile-local position in metres. */
FVector3f CoverAt(const FWorldTileGrid& Grid, float LocalX, float LocalY)
{
	const float GridX = FMath::Clamp(LocalX / Grid.CellSize, 0.f, Grid.NumX - 1.0001f);
	const float GridY = FMath::Clamp(LocalY / Grid.CellSize, 0.f, Grid.NumY - 1.0001f);
	const int32 X0 = FMath::FloorToInt(GridX);
	const int32 Y0 = FMath::FloorToInt(GridY);
	const float FractionX = GridX - X0;
	const float FractionY = GridY - Y0;
	const auto Vertex = [&](int32 X, int32 Y)
	{
		const FColor Color = Grid.CoverAtVertex(X, Y);
		return FVector3f(Color.R, Color.G, Color.B) / 255.f;
	};
	const FVector3f Top = FMath::Lerp(Vertex(X0, Y0), Vertex(X0 + 1, Y0), FractionX);
	const FVector3f Bottom = FMath::Lerp(Vertex(X0, Y0 + 1), Vertex(X0 + 1, Y0 + 1), FractionX);
	return FMath::Lerp(Top, Bottom, FractionY);
}

/** Builds the grass mask of a tile: land cover weights, minus everything that is built on or paved. */
TSharedPtr<const FGrassMask> BuildMask(const TSharedPtr<const FWorldTileData>& Data)
{
	TSharedRef<FGrassMask> Mask = MakeShared<FGrassMask>();
	const float Cell = FGrassMask::CellMetres;
	Mask->NumX = FMath::CeilToInt(Data->Size.X / Cell);
	Mask->NumY = FMath::CeilToInt(Data->Size.Y / Cell);
	TArray<uint8> Excluded;
	Excluded.SetNumZeroed(Mask->NumX * Mask->NumY);
	for (const FWorldSurface& Surface : Data->Surfaces)
	{
		RasterizePolygon(Surface.Polygon, Mask->NumX, Mask->NumY, Excluded);
	}
	for (const FWorldBuilding& Building : Data->Buildings)
	{
		RasterizePolygon(Building.Footprint, Mask->NumX, Mask->NumY, Excluded);
	}
	Mask->Lawn.SetNumZeroed(Excluded.Num());
	Mask->Meadow.SetNumZeroed(Excluded.Num());
	for (int32 Row = 0; Row < Mask->NumY; ++Row)
	{
		for (int32 Column = 0; Column < Mask->NumX; ++Column)
		{
			const int32 Index = Row * Mask->NumX + Column;
			if (Excluded[Index])
			{
				continue;
			}
			const FVector3f Cover = CoverAt(Data->Grid, (Column + 0.5f) * Cell, (Row + 0.5f) * Cell);
			// Leaf litter under tree crowns also shows as forest weight (up to 0.7), and a lawn under trees is still grass.
			const float Other = Cover.Y + Cover.Z;
			const float Shade = FMath::Clamp((1.f - Cover.Z) * 1.6f, 0.f, 1.f);
			Mask->Lawn[Index] = uint8(FMath::Clamp(1.f - Cover.X - Cover.Y, 0.f, 1.f) * Shade * 255.f);
			Mask->Meadow[Index] = uint8(FMath::Clamp(Cover.X, 0.f, 1.f) * FMath::Clamp(1.f - Other, 0.f, 1.f) * 255.f);
		}
	}
	return Mask;
}

/** A tile with everything a worker needs to place grass on it. */
struct FTileView
{
	FBox2D BoundsCm = FBox2D(ForceInit);
	TSharedPtr<const FWorldTileData> Data;
	TSharedPtr<const FGrassMask> Mask;
};

/** Grass weight (0..1) and ground height (cm) at a world position, or false when no tile covers it. */
bool SampleGround(const TArray<FTileView>& Tiles, const FVector2D& WorldCm, float& OutLawn, float& OutMeadow, float& OutHeightCm)
{
	for (const FTileView& Tile : Tiles)
	{
		if (!Tile.BoundsCm.IsInside(WorldCm))
		{
			continue;
		}
		const FVector2D LocalCm = WorldCm - Tile.BoundsCm.Min;
		const float LocalX = float(LocalCm.X) * 0.01f;
		const float LocalY = float(LocalCm.Y) * 0.01f;
		const int32 Column = FMath::Clamp(FMath::FloorToInt(LocalX / FGrassMask::CellMetres), 0, Tile.Mask->NumX - 1);
		const int32 Row = FMath::Clamp(FMath::FloorToInt(LocalY / FGrassMask::CellMetres), 0, Tile.Mask->NumY - 1);
		const int32 Index = Row * Tile.Mask->NumX + Column;
		OutLawn = Tile.Mask->Lawn[Index] / 255.f;
		OutMeadow = Tile.Mask->Meadow[Index] / 255.f;
		OutHeightCm = Tile.Data->Grid.TerrainAt(LocalX, LocalY) * 100.f;
		return true;
	}
	return false;
}


/** Offset of a seam tuft from the pavement edge: the kerb stone's width plus half its seam strip, metres. */
constexpr float SeamOffsetMetres = 0.145f;
constexpr float SeamSpacingMetres = 0.06f;
/** Share of the 3 m patches along a kerb where weeds grow; the rest of the seam stays clean. */
constexpr float SeamPatchShare = 0.75f;

/** Even-odd point in polygon test over all rings. */
bool GrassPolygonContains(const FWorldPolygon& Polygon, const FVector2f& Point)
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


/** Tufts along one pavement edge A-B (tile-local metres) that fall inside the cell. */
void PlaceSeamTuftsOnEdge(const FTileView& Tile, const FWorldSurface& Surface, const FVector2f& A, const FVector2f& B, const FBox2D& CellBounds,
	const FVector2f& ScaleXY, const FVector2f& ScaleZ, TArray<TArray<FTransform>>& ByMesh)
{
	const FWorldTileData& Data = *Tile.Data;
	const float Length = FVector2f::Distance(A, B);
	const FVector2f Direction = (B - A) / Length;
	const FVector2f Side(-Direction.Y, Direction.X);
	const FVector2f Middle = (A + B) * 0.5f;
	// The pavement side of the edge is the one inside the polygon.
	const bool bSideInside = GrassPolygonContains(Surface.Polygon, Middle + Side * 0.05f);
	const FVector2f Inward = bSideInside ? Side : -Side;
	const FVector2D TileOriginCm = Tile.BoundsCm.Min;
	bool bRoadBeside = false;
	for (const FWorldSurface& Other : Data.Surfaces)
	{
		const bool bRoad = Data.Names.IsValidIndex(Other.Material) && Data.Names[Other.Material].StartsWith(TEXT("Road_"));
		if (bRoad && GrassPolygonContains(Other.Polygon, Middle - Inward * 0.4f))
		{
			bRoadBeside = true;
			break;
		}
	}
	const int32 Steps = FMath::FloorToInt(Length / SeamSpacingMetres);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const FVector2f Along = A + Direction * ((Step + 0.5f) * SeamSpacingMetres);
		const FVector2D WorldCm = TileOriginCm + FVector2D(Along.X, Along.Y) * 100.0;
		// Patches of weeds, about 3 m long, decided by the world cell they fall in.
		const uint32 PatchHash = HashCell(FMath::FloorToInt(WorldCm.X / 300.0), FMath::FloorToInt(WorldCm.Y / 300.0), 51u);
		if (Random01(PatchHash, 0) > SeamPatchShare)
		{
			continue;
		}
		const uint32 Hash = HashCell(FMath::RoundToInt(WorldCm.X), FMath::RoundToInt(WorldCm.Y), 52u);
		if (Random01(Hash, 0) > 0.7f)
		{
			continue;
		}
		// A few tufts grow in the joints between the gutter's rows of setts, on the road side of the kerb.
		const bool bGutterTuft = bRoadBeside && Random01(Hash, 7) < 0.12f;
		const float GutterOffset = (Random01(Hash, 8) < 0.5f ? 0.1f : 0.2f) + 0.01f * (Random01(Hash, 9) - 0.5f);
		const FVector2f Position = bGutterTuft ? Along - Inward * (0.02f + GutterOffset) : Along + Inward * (SeamOffsetMetres + 0.01f * (Random01(Hash, 1) - 0.5f));
		const FVector2D PositionCm = TileOriginCm + FVector2D(Position.X, Position.Y) * 100.0;
		if (!CellBounds.IsInside(PositionCm))
		{
			continue;
		}
		const float HeightCm = (Data.Grid.RoadAt(Position.X, Position.Y) + (bGutterTuft ? 0.008f : Surface.Params[0])) * 100.f;
		const float ScaleAcross = FMath::Lerp(ScaleXY.X, ScaleXY.Y, Random01(Hash, 3));
		const float ScaleUp = FMath::Lerp(ScaleZ.X, ScaleZ.Y, Random01(Hash, 4));
		const FQuat Yaw(FVector::UpVector, Random01(Hash, 5) * UE_TWO_PI);
		const int32 MeshIndex = FMath::Min(int32(Random01(Hash, 6) * ByMesh.Num()), ByMesh.Num() - 1);
		ByMesh[MeshIndex].Emplace(Yaw, FVector(PositionCm.X, PositionCm.Y, HeightCm - RootSinkCm), FVector(ScaleAcross, ScaleAcross, ScaleUp));
	}
}

/** Seam tufts for every pavement edge near a cell. */
void PlaceSeamTufts(const TArray<FTileView>& Tiles, const FIntPoint& CellKey, const FVector2f& ScaleXY, const FVector2f& ScaleZ,
	TArray<TArray<FTransform>>& ByMesh)
{
	const FBox2D CellBounds(FVector2D(CellKey.X * CellCm, CellKey.Y * CellCm), FVector2D((CellKey.X + 1) * CellCm, (CellKey.Y + 1) * CellCm));
	for (const FTileView& Tile : Tiles)
	{
		const FWorldTileData& Data = *Tile.Data;
		for (const FWorldSurface& Surface : Data.Surfaces)
		{
			const bool bPavement = Data.Names.IsValidIndex(Surface.Material) && Data.Names[Surface.Material] == TEXT("Pavement");
			if (!bPavement)
			{
				continue;
			}
			for (const TArray<FVector2f>& Ring : Surface.Polygon.Rings)
			{
				for (int32 Index = 0; Index < Ring.Num(); ++Index)
				{
					const FVector2f& A = Ring[Index];
					const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
					const FVector2D MinCm = Tile.BoundsCm.Min + FVector2D(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y)) * 100.0;
					const FVector2D MaxCm = Tile.BoundsCm.Min + FVector2D(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y)) * 100.0;
					const bool bTouchesCell = MaxCm.X >= CellBounds.Min.X - 30.0 && MinCm.X <= CellBounds.Max.X + 30.0
						&& MaxCm.Y >= CellBounds.Min.Y - 30.0 && MinCm.Y <= CellBounds.Max.Y + 30.0;
					if (bTouchesCell && FVector2f::Distance(A, B) > 0.05f)
					{
						PlaceSeamTuftsOnEdge(Tile, Surface, A, B, CellBounds, ScaleXY, ScaleZ, ByMesh);
					}
				}
			}
		}
	}
}

/** Distance from a point to a cell's rectangle, cm. */
float DistanceToCell(const FIntPoint& Key, const FVector& Point)
{
	const FBox2D Rectangle(FVector2D(Key.X * CellCm, Key.Y * CellCm), FVector2D((Key.X + 1) * CellCm, (Key.Y + 1) * CellCm));
	return float(DistanceToBox2D(Rectangle, FVector2D(Point)));
}
}

using namespace GrassFieldDetail;

FGrassField::FGrassField(AActor* InOwner)
	: Owner(InOwner)
{
}

FGrassField::~FGrassField() = default;

bool FGrassField::LoadAssets()
{
	const auto Load = [](const FString& Path) -> UObject*
	{
		return StaticLoadObject(UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_Quiet | LOAD_NoWarn);
	};
	const auto TuftPath = [](const TCHAR* Pack, const TCHAR* Name)
	{
		return FString::Printf(TEXT("%s/%s/%s_2k/StaticMeshes/%s.%s"), TuftFolder, Pack, Pack, Name, Name);
	};

	FKind Lawn;
	Lawn.Name = TEXT("Lawn");
	Lawn.SpacingCm = 15.f;
	Lawn.RadiusCm = 1500.f;
	Lawn.ScaleXY = FVector2f(1.3f, 1.9f);
	Lawn.ScaleZ = FVector2f(0.5f, 0.75f);
	Lawn.Material = Cast<UMaterialInterface>(Load(TEXT("/Game/Grass/Materials/MI_GrassLawn.MI_GrassLawn")));
	for (const TCHAR* Name : {TEXT("grass_medium_01_small_a_LOD0"), TEXT("grass_medium_01_small_b_LOD0"), TEXT("grass_medium_01_mid_c_LOD0")})
	{
		Lawn.Meshes.Add(Cast<UStaticMesh>(Load(TuftPath(TEXT("grass_medium_01"), Name))));
	}

	FKind Meadow;
	Meadow.Name = TEXT("Meadow");
	Meadow.SpacingCm = 55.f;
	Meadow.RadiusCm = 4500.f;
	Meadow.ScaleXY = FVector2f(0.9f, 1.5f);
	Meadow.ScaleZ = FVector2f(0.9f, 1.5f);
	Meadow.Material = Cast<UMaterialInterface>(Load(TEXT("/Game/Grass/Materials/MI_GrassMeadow.MI_GrassMeadow")));
	for (const TCHAR* Name : {TEXT("grass_medium_02_c"), TEXT("grass_medium_02_d"), TEXT("grass_medium_02_e")})
	{
		Meadow.Meshes.Add(Cast<UStaticMesh>(Load(TuftPath(TEXT("grass_medium_02"), Name))));
	}
	Meadow.Meshes.Add(Cast<UStaticMesh>(Load(TuftPath(TEXT("grass_medium_02"), TEXT("grass_medium_02_b")))));

	// Weeds in the seam between kerb and pavement: the lawn's tufts and material, small, only placed along pavement edges.
	FKind Seam = Lawn;
	Seam.Name = TEXT("Seam");
	Seam.RadiusCm = 3500.f;
	Seam.ScaleXY = FVector2f(0.3f, 0.9f);
	Seam.ScaleZ = FVector2f(0.35f, 1.0f);

	for (FKind* Kind : {&Lawn, &Meadow, &Seam})
	{
		bool bComplete = Kind->Material != nullptr;
		for (const TObjectPtr<UStaticMesh>& Mesh : Kind->Meshes)
		{
			bComplete &= Mesh != nullptr;
		}
		if (!bComplete)
		{
			UE_LOG(LogGrassField, Warning, TEXT("Grass assets for %s missing; run Scripts/import_grass_models.py and create_grass_materials.py"), *Kind->Name.ToString());
			return false;
		}
		Kinds.Add(*Kind);
	}
	bAssetsLoaded = true;
	return true;
}

TSharedPtr<const FGrassMask> FGrassField::FindMask(const FGrassTileSource& Tile)
{
	const FIntPoint Corner(FMath::RoundToInt(Tile.BoundsCm.Min.X), FMath::RoundToInt(Tile.BoundsCm.Min.Y));
	FMaskEntry* Entry = Masks.Find(Corner);
	if (!Entry || Entry->DataKey != Tile.Data.Get())
	{
		FMaskEntry NewEntry;
		NewEntry.DataKey = Tile.Data.Get();
		const TSharedPtr<const FWorldTileData> Data = Tile.Data;
		NewEntry.Mask = Async(EAsyncExecution::ThreadPool, [Data]() { return BuildMask(Data); }).Share();
		Entry = &Masks.Add(Corner, NewEntry);
	}
	if (!Entry->Mask.IsReady())
	{
		return nullptr;
	}
	return Entry->Mask.Get();
}

void FGrassField::StartCell(const FIntPoint& Key, uint32 KindBits, const FTileFinder& FindTiles)
{
	TArray<FGrassTileSource> Sources;
	const FBox2D Rectangle(FVector2D(Key.X * CellCm, Key.Y * CellCm), FVector2D((Key.X + 1) * CellCm, (Key.Y + 1) * CellCm));
	FindTiles(Rectangle, Sources);
	TArray<FTileView> Views;
	for (const FGrassTileSource& Source : Sources)
	{
		FTileView View;
		View.BoundsCm = Source.BoundsCm;
		View.Data = Source.Data;
		View.Mask = FindMask(Source);
		if (!View.Mask)
		{
			return; // still being built; the next refresh tries again
		}
		Views.Add(View);
	}
	if (Views.IsEmpty())
	{
		return;
	}

	// Kinds' plain parameters for the worker; the meshes are chosen by index.
	struct FKindParameters
	{
		float SpacingCm;
		FVector2f ScaleXY;
		FVector2f ScaleZ;
		int32 MeshCount;
		bool bMeadow;
		bool bSeam;
		bool bIncluded;
	};
	TArray<FKindParameters> Parameters;
	for (int32 KindIndex = 0; KindIndex < Kinds.Num(); ++KindIndex)
	{
		const FKind& Kind = Kinds[KindIndex];
		Parameters.Add({Kind.SpacingCm, Kind.ScaleXY, Kind.ScaleZ, Kind.Meshes.Num(), Kind.Name == TEXT("Meadow"), Kind.Name == TEXT("Seam"), (KindBits & (1u << KindIndex)) != 0});
	}

	FCell& Cell = Cells.FindOrAdd(Key);
	Cell.bPending = true;
	FPendingCell Job;
	Job.Key = Key;
	Job.Kinds = KindBits;
	Job.Result = Async(EAsyncExecution::ThreadPool, [Key, Views, Parameters]()
	{
		TSharedPtr<FCellResult> Result = MakeShared<FCellResult>();
		for (int32 KindIndex = 0; KindIndex < Parameters.Num(); ++KindIndex)
		{
			const FKindParameters& Kind = Parameters[KindIndex];
			TArray<TArray<FTransform>>& ByMesh = Result->TransformsByKindAndMesh.AddDefaulted_GetRef();
			ByMesh.SetNum(Kind.MeshCount);
			if (!Kind.bIncluded)
			{
				continue;
			}
			if (Kind.bSeam)
			{
				PlaceSeamTufts(Views, Key, Kind.ScaleXY, Kind.ScaleZ, ByMesh);
				continue;
			}
			const int32 FirstX = FMath::FloorToInt(float(Key.X * CellCm) / Kind.SpacingCm);
			const int32 LastX = FMath::FloorToInt(float((Key.X + 1) * CellCm) / Kind.SpacingCm);
			const int32 FirstY = FMath::FloorToInt(float(Key.Y * CellCm) / Kind.SpacingCm);
			const int32 LastY = FMath::FloorToInt(float((Key.Y + 1) * CellCm) / Kind.SpacingCm);
			for (int32 LatticeY = FirstY; LatticeY <= LastY; ++LatticeY)
			{
				for (int32 LatticeX = FirstX; LatticeX <= LastX; ++LatticeX)
				{
					const uint32 Hash = HashCell(LatticeX, LatticeY, uint32(KindIndex + 1));
					const FVector2D Position((LatticeX + Random01(Hash, 0)) * Kind.SpacingCm, (LatticeY + Random01(Hash, 1)) * Kind.SpacingCm);
					if (Position.X < Key.X * CellCm || Position.X >= (Key.X + 1) * CellCm || Position.Y < Key.Y * CellCm || Position.Y >= (Key.Y + 1) * CellCm)
					{
						continue; // belongs to the neighbouring cell, whose lattice range covers it too
					}
					float Lawn = 0.f;
					float Meadow = 0.f;
					float HeightCm = 0.f;
					if (!SampleGround(Views, Position, Lawn, Meadow, HeightCm))
					{
						continue;
					}
					const float Weight = Kind.bMeadow ? Meadow : Lawn;
					if (Random01(Hash, 2) >= Weight)
					{
						continue;
					}
					const float ScaleAcross = FMath::Lerp(Kind.ScaleXY.X, Kind.ScaleXY.Y, Random01(Hash, 3));
					// Thin edges of a meadow are shorter.
					const float ScaleUp = FMath::Lerp(Kind.ScaleZ.X, Kind.ScaleZ.Y, Random01(Hash, 4)) * (Kind.bMeadow ? 0.6f + 0.4f * Weight : 1.f);
					const FQuat Yaw(FVector::UpVector, Random01(Hash, 5) * UE_TWO_PI);
					const int32 MeshIndex = FMath::Min(int32(Random01(Hash, 6) * Kind.MeshCount), Kind.MeshCount - 1);
					ByMesh[MeshIndex].Emplace(Yaw, FVector(Position.X, Position.Y, HeightCm - RootSinkCm), FVector(ScaleAcross, ScaleAcross, ScaleUp));
				}
			}
		}
		return Result;
	});
	Pending.Add(MoveTemp(Job));
}

void FGrassField::FinishCell(const FIntPoint& Key, uint32 KindBits, const FCellResult& Result)
{
	AActor* OwnerActor = Owner.Get();
	FCell* Cell = Cells.Find(Key);
	if (!OwnerActor || !Cell)
	{
		return;
	}
	RemoveCell(*Cell); // a rebuild replaces the components of the earlier build
	Cell->bPending = false;
	Cell->BuiltKinds = KindBits;
	int32 Total = 0;
	for (int32 KindIndex = 0; KindIndex < Kinds.Num(); ++KindIndex)
	{
		const FKind& Kind = Kinds[KindIndex];
		for (int32 MeshIndex = 0; MeshIndex < Kind.Meshes.Num(); ++MeshIndex)
		{
			const TArray<FTransform>& Transforms = Result.TransformsByKindAndMesh[KindIndex][MeshIndex];
			if (Transforms.IsEmpty())
			{
				continue;
			}
			UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(OwnerActor);
			Component->SetStaticMesh(Kind.Meshes[MeshIndex]);
			Component->SetMaterial(0, Kind.Material);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			Component->bAffectDynamicIndirectLighting = false;
			Component->bAffectDistanceFieldLighting = false;
			Component->bVisibleInRayTracing = false;
			Component->SetCullDistances(0, FMath::RoundToInt(Kind.RadiusCm + CellCm));
			Component->SetWorldPositionOffsetDisableDistance(FMath::RoundToInt(Kind.RadiusCm));
			Component->SetupAttachment(OwnerActor->GetRootComponent());
			Component->RegisterComponent();
			Component->AddInstances(Transforms, /*bShouldReturnIndices=*/false, /*bWorldSpace=*/true);
			Cell->Components.Add(Component);
			Total += Transforms.Num();
		}
	}
	UE_LOG(LogGrassField, Log, TEXT("Cell %d,%d: %d tufts in %d components"), Key.X, Key.Y, Total, Cell->Components.Num());
}

void FGrassField::RemoveCell(FCell& Cell)
{
	for (UInstancedStaticMeshComponent* Component : Cell.Components)
	{
		if (Component)
		{
			Component->DestroyComponent();
		}
	}
	Cell.Components.Reset();
}

void FGrassField::Update(const FVector& ViewerCm, const FTileFinder& FindTiles)
{
	if (!bAssetsLoaded)
	{
		return;
	}
	// Finished cells become components, one per call.
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		if (Pending[Index].Result.IsReady())
		{
			FinishCell(Pending[Index].Key, Pending[Index].Kinds, *Pending[Index].Result.Get());
			Pending.RemoveAtSwap(Index);
			break;
		}
	}

	const double Now = FPlatformTime::Seconds();
	const bool bMoved = FVector::DistSquared(ViewerCm, LastViewerCm) > FMath::Square(RefreshDistanceCm);
	if (!bMoved && !bCellsWaiting && Now - LastRefreshSeconds < RefreshSeconds)
	{
		return;
	}
	bCellsWaiting = false;
	LastRefreshSeconds = Now;
	LastViewerCm = ViewerCm;

	float LargestRadius = 0.f;
	for (const FKind& Kind : Kinds)
	{
		LargestRadius = FMath::Max(LargestRadius, Kind.RadiusCm);
	}
	// Cells to have, nearest first.
	const int32 Reach = FMath::CeilToInt(LargestRadius / CellCm) + 1;
	const FIntPoint Centre(FMath::FloorToInt(ViewerCm.X / CellCm), FMath::FloorToInt(ViewerCm.Y / CellCm));
	TArray<TPair<float, FIntPoint>> Candidates;
	for (int32 OffsetY = -Reach; OffsetY <= Reach; ++OffsetY)
	{
		for (int32 OffsetX = -Reach; OffsetX <= Reach; ++OffsetX)
		{
			const FIntPoint Key(Centre.X + OffsetX, Centre.Y + OffsetY);
			const float Distance = DistanceToCell(Key, ViewerCm);
			if (Distance < LargestRadius)
			{
				Candidates.Emplace(Distance, Key);
			}
		}
	}
	Candidates.Sort([](const TPair<float, FIntPoint>& A, const TPair<float, FIntPoint>& B) { return A.Key < B.Key; });
	for (const TPair<float, FIntPoint>& Candidate : Candidates)
	{
		if (Pending.Num() >= MaxCellsInFlight)
		{
			bCellsWaiting = true; // look again as soon as a worker is free
			break;
		}
		uint32 WantedKinds = 0;
		for (int32 KindIndex = 0; KindIndex < Kinds.Num(); ++KindIndex)
		{
			if (Candidate.Key < Kinds[KindIndex].RadiusCm)
			{
				WantedKinds |= 1u << KindIndex;
			}
		}
		const FCell* Existing = Cells.Find(Candidate.Value);
		const bool bMissingKind = !Existing || (WantedKinds & ~Existing->BuiltKinds) != 0;
		if (bMissingKind && (!Existing || !Existing->bPending))
		{
			StartCell(Candidate.Value, WantedKinds | (Existing ? Existing->BuiltKinds : 0u), FindTiles);
		}
	}
	// Drop what is out of reach, and masks of tiles far away.
	TArray<FIntPoint> ToRemove;
	for (TPair<FIntPoint, FCell>& Entry : Cells)
	{
		if (!Entry.Value.bPending && DistanceToCell(Entry.Key, ViewerCm) > LargestRadius + DropMarginCm)
		{
			ToRemove.Add(Entry.Key);
		}
	}
	for (const FIntPoint& Key : ToRemove)
	{
		RemoveCell(Cells[Key]);
		Cells.Remove(Key);
	}
	for (auto Iterator = Masks.CreateIterator(); Iterator; ++Iterator)
	{
		const FVector2D Corner(Iterator.Key().X, Iterator.Key().Y);
		if (FVector2D::Distance(Corner, FVector2D(ViewerCm)) > 40000.0 && Iterator.Value().Mask.IsReady())
		{
			Iterator.RemoveCurrent();
		}
	}
}

void FGrassField::Clear()
{
	for (TPair<FIntPoint, FCell>& Entry : Cells)
	{
		RemoveCell(Entry.Value);
	}
	Cells.Reset();
	Pending.Reset();
	Masks.Reset();
}
