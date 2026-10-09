#include "WorldSnow.h"

#include "HAL/PlatformTime.h"

#include "WorldMeshBuilder.h"
#include "WorldTileData.h"
#include "WorldTileMesher.h"

using namespace UE::Geometry;

namespace
{
constexpr float SnowCellMetres = 1.0f;
constexpr float SnowMetresToCm = 100.f;
/** Greatest distance to a road or footway edge that is measured, metres. */
constexpr float SnowEdgeReach = 5.1f;
/** Walls influence the snow within this distance, metres. */
constexpr float SnowWallReach = 7.f;
/** The least snow on any covered surface, metres. */
constexpr float SnowMinimumDepth = 0.035f;
constexpr float SnowMaximumDepth = 1.2f;
/** How far below the ground the skirt at the edge of the snow reaches, metres. */
constexpr float SnowSkirtDrop = 0.03f;
/** Snow depth above the base depth that smoothing may bring in from higher neighbours, metres. */
constexpr float SnowOverhangAllowance = 0.03f;
/** The ground is sampled this many times per snow cell along each axis when checking that it stays covered. */
constexpr int32 SnowFineSteps = 4;
/** The snow surface keeps at least this far above the ground everywhere between the vertices, metres. */
constexpr float SnowClearance = 0.03f;
/** Vertex colour scales. */
constexpr float SnowDepthPerColorStep = 0.005f;
constexpr float SnowEdgeDistancePerColorStep = 0.02f;

/** Direction the prevailing wind blows towards over Hamburg (from west-southwest), x east and y south. */
const FVector2f SnowWindDirection(0.94f, -0.34f);

enum class ESnowClass : uint8
{
	None = 0,
	Lawn,
	Road,
	Pavement,
	Path,
	Water,
};

/** What a ground material is for snow: how thick it lies and how it looks. */
ESnowClass SnowClassOfMaterial(const FString& Name)
{
	if (Name.StartsWith(TEXT("Road_")) || Name.StartsWith(TEXT("Bridge_")))
	{
		return ESnowClass::Road;
	}
	if (Name == TEXT("Pavement"))
	{
		return ESnowClass::Pavement;
	}
	if (Name.StartsWith(TEXT("Path_")))
	{
		return ESnowClass::Path;
	}
	if (Name == TEXT("Water"))
	{
		return ESnowClass::Water;
	}
	return ESnowClass::Lawn;
}

/** Snow depth in metres at full snow cover on a level surface of this class, before smoothing. */
float SnowBaseDepth(ESnowClass Class)
{
	switch (Class)
	{
	case ESnowClass::Lawn: return 0.16f;
	case ESnowClass::Road: return 0.035f;
	case ESnowClass::Pavement: return 0.07f;
	case ESnowClass::Path: return 0.10f;
	default: return 0.f;
	}
}

float SnowHash(int32 X, int32 Y)
{
	uint32 Value = uint32(X) * 73856093u ^ uint32(Y) * 19349663u;
	Value = (Value ^ (Value >> 13)) * 1274126177u;
	return float((Value ^ (Value >> 16)) & 0xFFFFu) / 65535.f;
}

/** Smooth value noise in 0..1; one lattice cell per unit. */
float SnowValueNoise(float X, float Y)
{
	const int32 CellX = FMath::FloorToInt(X);
	const int32 CellY = FMath::FloorToInt(Y);
	const float FractionX = FMath::SmoothStep(0.f, 1.f, X - CellX);
	const float FractionY = FMath::SmoothStep(0.f, 1.f, Y - CellY);
	const float Low = FMath::Lerp(SnowHash(CellX, CellY), SnowHash(CellX + 1, CellY), FractionX);
	const float High = FMath::Lerp(SnowHash(CellX, CellY + 1), SnowHash(CellX + 1, CellY + 1), FractionX);
	return FMath::Lerp(Low, High, FractionY);
}

/** Even-odd test against all rings. */
bool SnowIsInside(const FWorldPolygon& Polygon, const FVector2f& Point)
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

/** The ground and snow quantities on a grid of vertices every SnowCellMetres. */
struct FSnowSamples
{
	int32 CountX = 0;
	int32 CountY = 0;
	TArray<float> Ground;
	/** Height of the terrain mesh alone, which can lie above a road or pavement surface along its edges. */
	TArray<float> Terrain;
	TArray<ESnowClass> Class;
	/** Distance to the nearest edge of a road surface or of a footway surface, metres, capped at SnowEdgeReach. */
	TArray<float> RoadEdge;
	TArray<float> FootEdge;
	/** Distance to the nearest wall: positive outside, negative inside a building. */
	TArray<float> WallDistance;
	TArray<FVector2f> WallOutward;
	/** The snow surface and its depth over the ground. */
	TArray<float> Surface;
	TArray<float> Depth;
	/** Samples that get no snow: no ground, water, or deep inside a building. */
	TArray<bool> bSkip;

	int32 Index(int32 X, int32 Y) const { return Y * CountX + X; }
};

void InitSamples(const FWorldTileData& Tile, FSnowSamples& Samples)
{
	Samples.CountX = FMath::CeilToInt(Tile.Size.X / SnowCellMetres) + 1;
	Samples.CountY = FMath::CeilToInt(Tile.Size.Y / SnowCellMetres) + 1;
	const int32 Total = Samples.CountX * Samples.CountY;
	Samples.Ground.Init(0.f, Total);
	Samples.Terrain.Init(-1e9f, Total);
	Samples.Class.Init(ESnowClass::None, Total);
	Samples.RoadEdge.Init(SnowEdgeReach, Total);
	Samples.FootEdge.Init(SnowEdgeReach, Total);
	Samples.WallDistance.Init(99.f, Total);
	Samples.WallOutward.Init(FVector2f::ZeroVector, Total);
	Samples.Surface.Init(0.f, Total);
	Samples.Depth.Init(0.f, Total);
	Samples.bSkip.Init(true, Total);
}

/** Writes the triangle's height and class into the samples that fall inside it. */
void RasterizeTriangle(const FVector3f& A, const FVector3f& B, const FVector3f& C, ESnowClass Class, bool bTerrain, FSnowSamples& Samples)
{
	const FVector3f Face = FVector3f::CrossProduct(B - A, C - A);
	const float FaceLength = Face.Size();
	if (FaceLength < 1e-3f || FMath::Abs(Face.Z) / FaceLength < 0.2f)
	{
		return; // vertical kerb faces and skirts carry no snow
	}
	const FVector2f PointA = FVector2f(A.X, A.Y) / SnowMetresToCm;
	const FVector2f PointB = FVector2f(B.X, B.Y) / SnowMetresToCm;
	const FVector2f PointC = FVector2f(C.X, C.Y) / SnowMetresToCm;
	const float Determinant = (PointB.X - PointA.X) * (PointC.Y - PointA.Y) - (PointC.X - PointA.X) * (PointB.Y - PointA.Y);
	if (FMath::Abs(Determinant) < 1e-9f)
	{
		return;
	}
	const int32 MinX = FMath::Max(0, FMath::CeilToInt(FMath::Min3(PointA.X, PointB.X, PointC.X) / SnowCellMetres));
	const int32 MaxX = FMath::Min(Samples.CountX - 1, FMath::FloorToInt(FMath::Max3(PointA.X, PointB.X, PointC.X) / SnowCellMetres));
	const int32 MinY = FMath::Max(0, FMath::CeilToInt(FMath::Min3(PointA.Y, PointB.Y, PointC.Y) / SnowCellMetres));
	const int32 MaxY = FMath::Min(Samples.CountY - 1, FMath::FloorToInt(FMath::Max3(PointA.Y, PointB.Y, PointC.Y) / SnowCellMetres));
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const FVector2f Point(X * SnowCellMetres, Y * SnowCellMetres);
			const float WeightB = ((Point.X - PointA.X) * (PointC.Y - PointA.Y) - (PointC.X - PointA.X) * (Point.Y - PointA.Y)) / Determinant;
			const float WeightC = ((PointB.X - PointA.X) * (Point.Y - PointA.Y) - (Point.X - PointA.X) * (PointB.Y - PointA.Y)) / Determinant;
			const float WeightA = 1.f - WeightB - WeightC;
			const float Epsilon = -1e-4f;
			if (WeightA < Epsilon || WeightB < Epsilon || WeightC < Epsilon)
			{
				continue;
			}
			const float Height = (A.Z * WeightA + B.Z * WeightB + C.Z * WeightC) / SnowMetresToCm;
			const int32 Index = Samples.Index(X, Y);
			if (bTerrain)
			{
				Samples.Terrain[Index] = FMath::Max(Samples.Terrain[Index], Height);
			}
			const bool bCovered = Samples.Class[Index] != ESnowClass::None;
			const bool bTerrainBelow = Samples.Class[Index] == ESnowClass::Lawn;
			const bool bWritesOver = Class != ESnowClass::Lawn && (!bCovered || bTerrainBelow || Height > Samples.Ground[Index] - 0.02f);
			if (!bCovered || bWritesOver)
			{
				Samples.Ground[Index] = Height;
				Samples.Class[Index] = Class;
			}
		}
	}
}

/** Heights and classes of the ground (terrain, roads, pavements, paths) from the built ground triangles. */
void RasterizeGround(const FWorldTileMeshes& Meshes, FSnowSamples& Samples)
{
	const FWorldMeshBuilder& Ground = Meshes.Ground;
	for (int32 Triangle = 0; Triangle < Ground.NumTriangles(); ++Triangle)
	{
		const FString& Name = Meshes.MaterialNames.IsValidIndex(Ground.GetTriangleMaterial(Triangle)) ? Meshes.MaterialNames[Ground.GetTriangleMaterial(Triangle)] : FString();
		if (Name == TEXT("Kerb"))
		{
			continue;
		}
		const FIntVector3& Corners = Ground.GetTriangle(Triangle);
		RasterizeTriangle(Ground.GetPosition(Corners.X), Ground.GetPosition(Corners.Y), Ground.GetPosition(Corners.Z), SnowClassOfMaterial(Name), Name == TEXT("Terrain_Grass"), Samples);
	}
}

/**
 * Gives samples that no ground triangle reached (slivers between surfaces, the tile edge) the ground of their
 * covered neighbours, up to a few cells deep, so the snow never reaches down to an unset height.
 */
void FillUncoveredSamples(FSnowSamples& Samples)
{
	for (int32 Pass = 0; Pass < 4; ++Pass)
	{
		const TArray<ESnowClass> ClassBefore = Samples.Class;
		for (int32 Y = 0; Y < Samples.CountY; ++Y)
		{
			for (int32 X = 0; X < Samples.CountX; ++X)
			{
				const int32 Index = Samples.Index(X, Y);
				if (ClassBefore[Index] != ESnowClass::None)
				{
					continue;
				}
				float GroundSum = 0.f;
				int32 Found = 0;
				ESnowClass FoundClass = ESnowClass::None;
				const int32 Offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
				for (const int32* Offset : Offsets)
				{
					const int32 NeighbourX = X + Offset[0];
					const int32 NeighbourY = Y + Offset[1];
					if (NeighbourX < 0 || NeighbourY < 0 || NeighbourX >= Samples.CountX || NeighbourY >= Samples.CountY)
					{
						continue;
					}
					const int32 Neighbour = Samples.Index(NeighbourX, NeighbourY);
					if (ClassBefore[Neighbour] == ESnowClass::None)
					{
						continue;
					}
					GroundSum += Samples.Ground[Neighbour];
					FoundClass = FMath::Max(FoundClass, ClassBefore[Neighbour]);
					++Found;
				}
				if (Found > 0)
				{
					Samples.Ground[Index] = GroundSum / Found;
					Samples.Class[Index] = FoundClass;
				}
			}
		}
	}
}

/** True for a ring edge that only exists because the polygon was cut at the tile border. */
bool SnowIsOnTileBorder(const FWorldTileData& Tile, const FVector2f& A, const FVector2f& B)
{
	const auto Near = [](float Value, float Target) { return FMath::Abs(Value - Target) < 0.01f; };
	return (Near(A.X, 0.f) && Near(B.X, 0.f)) || (Near(A.Y, 0.f) && Near(B.Y, 0.f))
		|| (Near(A.X, Tile.Size.X) && Near(B.X, Tile.Size.X)) || (Near(A.Y, Tile.Size.Y) && Near(B.Y, Tile.Size.Y));
}

/** Distance from a point to a segment. */
float SnowDistanceToSegment(const FVector2f& Point, const FVector2f& A, const FVector2f& B)
{
	const FVector2f Edge = B - A;
	const float T = FMath::Clamp(FVector2f::DotProduct(Point - A, Edge) / FMath::Max(Edge.SizeSquared(), 1e-12f), 0.f, 1.f);
	return FVector2f::Distance(Point, A + Edge * T);
}

/** Lowers Distances at every sample near the polygon's outline to the distance to it. */
void StampPolygonEdgeDistance(const FWorldTileData& Tile, const FWorldPolygon& Polygon, FSnowSamples& Samples, TArray<float>& Distances)
{
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		for (int32 Index = 0; Index < Ring.Num(); ++Index)
		{
			const FVector2f& A = Ring[Index];
			const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
			if (SnowIsOnTileBorder(Tile, A, B))
			{
				continue;
			}
			const int32 MinX = FMath::Max(0, FMath::FloorToInt((FMath::Min(A.X, B.X) - SnowEdgeReach) / SnowCellMetres));
			const int32 MaxX = FMath::Min(Samples.CountX - 1, FMath::CeilToInt((FMath::Max(A.X, B.X) + SnowEdgeReach) / SnowCellMetres));
			const int32 MinY = FMath::Max(0, FMath::FloorToInt((FMath::Min(A.Y, B.Y) - SnowEdgeReach) / SnowCellMetres));
			const int32 MaxY = FMath::Min(Samples.CountY - 1, FMath::CeilToInt((FMath::Max(A.Y, B.Y) + SnowEdgeReach) / SnowCellMetres));
			for (int32 Y = MinY; Y <= MaxY; ++Y)
			{
				for (int32 X = MinX; X <= MaxX; ++X)
				{
					float& Distance = Distances[Samples.Index(X, Y)];
					Distance = FMath::Min(Distance, SnowDistanceToSegment(FVector2f(X * SnowCellMetres, Y * SnowCellMetres), A, B));
				}
			}
		}
	}
}

void StampRoadAndFootwayEdges(const FWorldTileData& Tile, FSnowSamples& Samples)
{
	for (const FWorldSurface& Surface : Tile.Surfaces)
	{
		const FString& Name = Tile.Names.IsValidIndex(Surface.Material) ? Tile.Names[Surface.Material] : FString();
		const ESnowClass Class = SnowClassOfMaterial(Name);
		if (Class == ESnowClass::Road)
		{
			StampPolygonEdgeDistance(Tile, Surface.Polygon, Samples, Samples.RoadEdge);
		}
		else if (Class == ESnowClass::Pavement || Class == ESnowClass::Path)
		{
			StampPolygonEdgeDistance(Tile, Surface.Polygon, Samples, Samples.FootEdge);
		}
	}
}

/** Direction pointing out of the polygon at the middle of edge A-B. */
FVector2f SnowOutward(const FWorldPolygon& Polygon, const FVector2f& A, const FVector2f& B)
{
	const FVector2f Side = FVector2f(-(B.Y - A.Y), B.X - A.X).GetSafeNormal();
	const FVector2f Probe = (A + B) * 0.5f + Side * 0.05f;
	return SnowIsInside(Polygon, Probe) ? -Side : Side;
}

/** Signed distance to the nearest wall and its outward direction, around one building. */
void StampBuildingWalls(const FWorldBuilding& Building, FSnowSamples& Samples)
{
	if (Building.Footprint.Rings.IsEmpty() || Building.Footprint.Rings[0].Num() < 3)
	{
		return;
	}
	const FBox2f Bounds(Building.Footprint.Rings[0]);
	const int32 MinX = FMath::Max(0, FMath::FloorToInt((Bounds.Min.X - SnowWallReach) / SnowCellMetres));
	const int32 MaxX = FMath::Min(Samples.CountX - 1, FMath::CeilToInt((Bounds.Max.X + SnowWallReach) / SnowCellMetres));
	const int32 MinY = FMath::Max(0, FMath::FloorToInt((Bounds.Min.Y - SnowWallReach) / SnowCellMetres));
	const int32 MaxY = FMath::Min(Samples.CountY - 1, FMath::CeilToInt((Bounds.Max.Y + SnowWallReach) / SnowCellMetres));
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const FVector2f Point(X * SnowCellMetres, Y * SnowCellMetres);
			float Nearest = MAX_flt;
			const FVector2f* NearestA = nullptr;
			const FVector2f* NearestB = nullptr;
			for (const TArray<FVector2f>& Ring : Building.Footprint.Rings)
			{
				for (int32 Index = 0; Index < Ring.Num(); ++Index)
				{
					const FVector2f& A = Ring[Index];
					const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
					const float Distance = SnowDistanceToSegment(Point, A, B);
					if (Distance < Nearest)
					{
						Nearest = Distance;
						NearestA = &A;
						NearestB = &B;
					}
				}
			}
			if (Nearest > SnowWallReach || !NearestA)
			{
				continue;
			}
			const bool bInside = SnowIsInside(Building.Footprint, Point);
			const float Signed = bInside ? -Nearest : Nearest;
			const int32 Index = Samples.Index(X, Y);
			if (FMath::Abs(Signed) < FMath::Abs(Samples.WallDistance[Index]) || (bInside && Samples.WallDistance[Index] > 0.f && Nearest < SnowWallReach))
			{
				Samples.WallDistance[Index] = Signed;
				Samples.WallOutward[Index] = SnowOutward(Building.Footprint, *NearestA, *NearestB);
			}
		}
	}
}

/** One 3-wide box blur pass of a grid along X then Y; edges repeat their border value. */
void BoxBlur(const FSnowSamples& Samples, TArray<float>& Values)
{
	TArray<float> Scratch = Values;
	for (int32 Y = 0; Y < Samples.CountY; ++Y)
	{
		for (int32 X = 0; X < Samples.CountX; ++X)
		{
			const float Left = Values[Samples.Index(FMath::Max(X - 1, 0), Y)];
			const float Right = Values[Samples.Index(FMath::Min(X + 1, Samples.CountX - 1), Y)];
			Scratch[Samples.Index(X, Y)] = (Left + Values[Samples.Index(X, Y)] + Right) / 3.f;
		}
	}
	for (int32 Y = 0; Y < Samples.CountY; ++Y)
	{
		for (int32 X = 0; X < Samples.CountX; ++X)
		{
			const float Down = Scratch[Samples.Index(X, FMath::Max(Y - 1, 0))];
			const float Up = Scratch[Samples.Index(X, FMath::Min(Y + 1, Samples.CountY - 1))];
			Values[Samples.Index(X, Y)] = (Down + Scratch[Samples.Index(X, Y)] + Up) / 3.f;
		}
	}
}

/** Extra snow ploughed up against the kerb along a road, metres. */
float PloughRidgeDepth(float DistanceToRoadEdge)
{
	const float Offset = (DistanceToRoadEdge - 0.45f) / 0.32f;
	return 0.04f * FMath::Exp(-Offset * Offset);
}

/** Extra snow blown against a wall: a lot on the lee side, a little on the windward side, metres. */
float WallDriftDepth(float Distance, const FVector2f& Outward)
{
	if (Distance <= 0.f || Distance > SnowWallReach)
	{
		return 0.f;
	}
	const float Lee = FMath::SmoothStep(-0.3f, 0.8f, FVector2f::DotProduct(Outward, SnowWindDirection));
	return (0.35f * Lee + 0.04f) * FMath::Exp(-Distance / 1.5f);
}

/** The snow surface and depth: class dependent depth, smoothed so edges round off, plus ridges and drifts. */
void ComputeSnowSurface(const FWorldTileData& Tile, FSnowSamples& Samples)
{
	const int32 Total = Samples.Ground.Num();
	TArray<float> Smoothed;
	Smoothed.SetNumUninitialized(Total);
	for (int32 Y = 0; Y < Samples.CountY; ++Y)
	{
		for (int32 X = 0; X < Samples.CountX; ++X)
		{
			const int32 Index = Samples.Index(X, Y);
			const float WorldX = float(Tile.Origin.X) + X * SnowCellMetres;
			const float WorldY = float(Tile.Origin.Y) + Y * SnowCellMetres;
			const float Variation = 0.82f + 0.36f * (SnowValueNoise(WorldX / 7.f, WorldY / 7.f) * 0.7f + SnowValueNoise(WorldX / 2.3f, WorldY / 2.3f) * 0.3f);
			Smoothed[Index] = Samples.Ground[Index] + SnowBaseDepth(Samples.Class[Index]) * Variation;
		}
	}
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		BoxBlur(Samples, Smoothed);
	}
	// Lawn snow is smoothed much further: it hides the ground's small bumps and merges into big flat triangles.
	TArray<float> SmoothedLawn = Smoothed;
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		BoxBlur(Samples, SmoothedLawn);
	}
	for (int32 Index = 0; Index < Total; ++Index)
	{
		if (Samples.Class[Index] == ESnowClass::Lawn)
		{
			Smoothed[Index] = SmoothedLawn[Index];
		}
	}
	for (int32 Y = 0; Y < Samples.CountY; ++Y)
	{
		for (int32 X = 0; X < Samples.CountX; ++X)
		{
			// Depth over ground that drops away (blurred in from a higher neighbour) is capped, so the snow never hangs over a drop.
			const int32 Index = Samples.Index(X, Y);
			const float Limit = Samples.Ground[Index] + SnowBaseDepth(Samples.Class[Index]) * 1.2f + SnowOverhangAllowance;
			Smoothed[Index] = FMath::Min(Smoothed[Index], Limit);
		}
	}
	for (int32 Index = 0; Index < Total; ++Index)
	{
		const ESnowClass Class = Samples.Class[Index];
		float Depth = FMath::Max(Smoothed[Index] - Samples.Ground[Index], SnowMinimumDepth);
		if (Class == ESnowClass::Road)
		{
			Depth += PloughRidgeDepth(Samples.RoadEdge[Index]);
		}
		const float DriftScale = Class == ESnowClass::Road ? 0.3f : 1.f;
		Depth += DriftScale * WallDriftDepth(Samples.WallDistance[Index], Samples.WallOutward[Index]);
		Samples.Depth[Index] = FMath::Min(Depth, SnowMaximumDepth);
		Samples.Surface[Index] = Samples.Ground[Index] + Samples.Depth[Index];
		const bool bNoSnow = Class == ESnowClass::None || Class == ESnowClass::Water;
		Samples.bSkip[Index] = bNoSnow || Samples.WallDistance[Index] < -2.5f;
	}
}

/** The ground height on a grid SnowFineSteps times finer than the snow cells, kerb tops included. */
struct FSnowFineGround
{
	int32 CountX = 0;
	int32 CountY = 0;
	TArray<float> Height;
};

/** Raises Heights wherever a triangle of the ground covers a fine grid point above it. */
void RasterizeFineTriangle(const FVector3f& A, const FVector3f& B, const FVector3f& C, FSnowFineGround& Fine)
{
	const FVector3f Face = FVector3f::CrossProduct(B - A, C - A);
	const float FaceLength = Face.Size();
	if (FaceLength < 1e-3f || FMath::Abs(Face.Z) / FaceLength < 0.2f)
	{
		return;
	}
	const float Spacing = SnowCellMetres / float(SnowFineSteps);
	const FVector2f PointA = FVector2f(A.X, A.Y) / SnowMetresToCm;
	const FVector2f PointB = FVector2f(B.X, B.Y) / SnowMetresToCm;
	const FVector2f PointC = FVector2f(C.X, C.Y) / SnowMetresToCm;
	const float Determinant = (PointB.X - PointA.X) * (PointC.Y - PointA.Y) - (PointC.X - PointA.X) * (PointB.Y - PointA.Y);
	if (FMath::Abs(Determinant) < 1e-9f)
	{
		return;
	}
	const int32 MinX = FMath::Max(0, FMath::CeilToInt(FMath::Min3(PointA.X, PointB.X, PointC.X) / Spacing));
	const int32 MaxX = FMath::Min(Fine.CountX - 1, FMath::FloorToInt(FMath::Max3(PointA.X, PointB.X, PointC.X) / Spacing));
	const int32 MinY = FMath::Max(0, FMath::CeilToInt(FMath::Min3(PointA.Y, PointB.Y, PointC.Y) / Spacing));
	const int32 MaxY = FMath::Min(Fine.CountY - 1, FMath::FloorToInt(FMath::Max3(PointA.Y, PointB.Y, PointC.Y) / Spacing));
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const FVector2f Point(X * Spacing, Y * Spacing);
			const float WeightB = ((Point.X - PointA.X) * (PointC.Y - PointA.Y) - (PointC.X - PointA.X) * (Point.Y - PointA.Y)) / Determinant;
			const float WeightC = ((PointB.X - PointA.X) * (Point.Y - PointA.Y) - (Point.X - PointA.X) * (PointB.Y - PointA.Y)) / Determinant;
			const float WeightA = 1.f - WeightB - WeightC;
			const float Epsilon = -1e-4f;
			if (WeightA < Epsilon || WeightB < Epsilon || WeightC < Epsilon)
			{
				continue;
			}
			float& Height = Fine.Height[Y * Fine.CountX + X];
			Height = FMath::Max(Height, (A.Z * WeightA + B.Z * WeightB + C.Z * WeightC) / SnowMetresToCm);
		}
	}
}

/** Raises the fine grid point nearest to a ground position (cm) to its height. */
void RaiseFineGroundPoint(const FVector3f& Position, FSnowFineGround& Fine)
{
	const float Spacing = SnowCellMetres / float(SnowFineSteps);
	const int32 X = FMath::RoundToInt(Position.X / SnowMetresToCm / Spacing);
	const int32 Y = FMath::RoundToInt(Position.Y / SnowMetresToCm / Spacing);
	if (X < 0 || Y < 0 || X >= Fine.CountX || Y >= Fine.CountY)
	{
		return;
	}
	float& Height = Fine.Height[Y * Fine.CountX + X];
	Height = FMath::Max(Height, Position.Z / SnowMetresToCm);
}

/**
 * Raises the fine grid along the edges of a triangle. A kerb top is narrower than a fine grid step and a kerb face
 * is vertical, so neither is caught by the fill of RasterizeFineTriangle; their edges are.
 */
void RaiseFineGroundAlongEdges(const FVector3f& A, const FVector3f& B, const FVector3f& C, FSnowFineGround& Fine)
{
	const FVector3f Corners[3] = {A, B, C};
	const float StepCm = SnowCellMetres * SnowMetresToCm / float(SnowFineSteps) * 0.5f;
	for (int32 Edge = 0; Edge < 3; ++Edge)
	{
		const FVector3f& From = Corners[Edge];
		const FVector3f& To = Corners[(Edge + 1) % 3];
		const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2f::Distance(FVector2f(From.X, From.Y), FVector2f(To.X, To.Y)) / StepCm));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			RaiseFineGroundPoint(FMath::Lerp(From, To, float(Step) / Steps), Fine);
		}
	}
}

/** Fine ground heights from all ground triangles, kerbs included; -1e9 where there is no ground. */
void RasterizeFineGround(const FWorldTileMeshes& Meshes, const FSnowSamples& Samples, FSnowFineGround& Fine)
{
	Fine.CountX = (Samples.CountX - 1) * SnowFineSteps + 1;
	Fine.CountY = (Samples.CountY - 1) * SnowFineSteps + 1;
	Fine.Height.Init(-1e9f, Fine.CountX * Fine.CountY);
	const FWorldMeshBuilder& Ground = Meshes.Ground;
	for (int32 Triangle = 0; Triangle < Ground.NumTriangles(); ++Triangle)
	{
		const FIntVector3& Corners = Ground.GetTriangle(Triangle);
		RasterizeFineTriangle(Ground.GetPosition(Corners.X), Ground.GetPosition(Corners.Y), Ground.GetPosition(Corners.Z), Fine);
		RaiseFineGroundAlongEdges(Ground.GetPosition(Corners.X), Ground.GetPosition(Corners.Y), Ground.GetPosition(Corners.Z), Fine);
	}
}

/** Barycentric weights of a point (U, V in 0..1) in a cell on its two triangles, as the mesher splits the cell. */
void SnowCellWeights(int32 CellX, int32 CellY, float U, float V, float OutWeights[4])
{
	// Corners in the order 00, 10, 11, 01.
	if (((CellX + CellY) & 1) == 0)
	{
		if (U >= V)
		{
			OutWeights[0] = 1.f - U; OutWeights[1] = U - V; OutWeights[2] = V; OutWeights[3] = 0.f;
		}
		else
		{
			OutWeights[0] = 1.f - V; OutWeights[1] = 0.f; OutWeights[2] = U; OutWeights[3] = V - U;
		}
	}
	else if (U + V <= 1.f)
	{
		OutWeights[0] = 1.f - U - V; OutWeights[1] = U; OutWeights[2] = 0.f; OutWeights[3] = V;
	}
	else
	{
		OutWeights[0] = 0.f; OutWeights[1] = 1.f - V; OutWeights[2] = U + V - 1.f; OutWeights[3] = 1.f - U;
	}
}

/**
 * Raises the snow surface where the ground between its vertices would poke through: a kerb top, a bump the 1 m grid
 * misses. Every ground point on a finer grid is compared with the surface interpolated from the cell's vertices; a
 * shortfall is made up by moving the cell's vertices by the least amount, weighted by their share in that point.
 * The kerb ends up as a ramp instead of a step that is filled level. Only raises, so the points already
 * checked stay covered, and one pass is enough.
 */
void KeepSnowAboveGround(const FSnowFineGround& Fine, FSnowSamples& Samples)
{
	for (int32 CellY = 0; CellY < Samples.CountY - 1; ++CellY)
	{
		for (int32 CellX = 0; CellX < Samples.CountX - 1; ++CellX)
		{
			const int32 Corners[4] = {Samples.Index(CellX, CellY), Samples.Index(CellX + 1, CellY), Samples.Index(CellX + 1, CellY + 1), Samples.Index(CellX, CellY + 1)};
			if (Samples.bSkip[Corners[0]] && Samples.bSkip[Corners[1]] && Samples.bSkip[Corners[2]] && Samples.bSkip[Corners[3]])
			{
				continue;
			}
			for (int32 FineY = 0; FineY <= SnowFineSteps; ++FineY)
			{
				for (int32 FineX = 0; FineX <= SnowFineSteps; ++FineX)
				{
					const float Ground = Fine.Height[(CellY * SnowFineSteps + FineY) * Fine.CountX + CellX * SnowFineSteps + FineX];
					if (Ground < -1e8f)
					{
						continue;
					}
					float Weights[4];
					SnowCellWeights(CellX, CellY, float(FineX) / SnowFineSteps, float(FineY) / SnowFineSteps, Weights);
					float Interpolated = 0.f;
					float WeightSquares = 0.f;
					for (int32 Corner = 0; Corner < 4; ++Corner)
					{
						Interpolated += Weights[Corner] * Samples.Surface[Corners[Corner]];
						WeightSquares += Weights[Corner] * Weights[Corner];
					}
					const float Shortfall = Ground + SnowClearance - Interpolated;
					if (Shortfall <= 0.f)
					{
						continue;
					}
					for (int32 Corner = 0; Corner < 4; ++Corner)
					{
						Samples.Surface[Corners[Corner]] += Shortfall * Weights[Corner] / WeightSquares;
					}
				}
			}
		}
	}
	for (int32 Index = 0; Index < Samples.Surface.Num(); ++Index)
	{
		Samples.Depth[Index] = FMath::Min(Samples.Surface[Index] - Samples.Ground[Index], SnowMaximumDepth);
	}
}

FColor SnowVertexColor(const FSnowSamples& Samples, int32 Index)
{
	const ESnowClass Class = Samples.Class[Index];
	const float EdgeDistance = Class == ESnowClass::Road ? Samples.RoadEdge[Index] : Samples.FootEdge[Index];
	return FColor(
		uint8(FMath::Clamp(FMath::RoundToInt(Samples.Depth[Index] / SnowDepthPerColorStep), 0, 255)),
		Class == ESnowClass::Road ? 255 : 0,
		(Class == ESnowClass::Pavement || Class == ESnowClass::Path) ? 255 : 0,
		uint8(FMath::Clamp(FMath::RoundToInt(EdgeDistance / SnowEdgeDistancePerColorStep), 0, 255)));
}

/** Writes the snow surface as two triangles per grid cell. */
class FSnowGridMesher
{
public:
	FSnowGridMesher(const FWorldTileData& InTile, const FSnowSamples& InSamples, FWorldMeshBuilder& InBuilder)
		: Tile(InTile), Samples(InSamples), Builder(InBuilder)
	{
		VertexOfSample.Init(INDEX_NONE, Samples.Ground.Num());
	}

	void Build()
	{
		const int32 CellsX = Samples.CountX - 1;
		const int32 CellsY = Samples.CountY - 1;
		for (int32 Y = 0; Y < CellsY; ++Y)
		{
			for (int32 X = 0; X < CellsX; ++X)
			{
				EmitCell(X, Y);
			}
		}
	}

private:
	/** The mesh vertex of a sample, created on first use. */
	int32 VertexAt(int32 X, int32 Y)
	{
		const int32 Index = Samples.Index(X, Y);
		if (VertexOfSample[Index] == INDEX_NONE)
		{
			const FVector3f Position(X * SnowCellMetres * SnowMetresToCm, Y * SnowCellMetres * SnowMetresToCm, Samples.Surface[Index] * SnowMetresToCm);
			const FVector2f UV(float(Tile.Origin.X) + X * SnowCellMetres, float(Tile.Origin.Y) + Y * SnowCellMetres);
			VertexOfSample[Index] = Builder.AddVertex(Position, UV, SnowVertexColor(Samples, Index));
		}
		return VertexOfSample[Index];
	}

	/** True when every sample in the block (the block's cells and their corners) gets no snow. */
	bool IsBlockEmpty(int32 X0, int32 Y0, int32 Size) const
	{
		for (int32 Y = Y0; Y <= FMath::Min(Y0 + Size, Samples.CountY - 1); ++Y)
		{
			for (int32 X = X0; X <= FMath::Min(X0 + Size, Samples.CountX - 1); ++X)
			{
				if (!Samples.bSkip[Samples.Index(X, Y)])
				{
					return false;
				}
			}
		}
		return true;
	}

	/** The two triangles of one grid cell; cells with no snow are left out. */
	void EmitCell(int32 X0, int32 Y0)
	{
		if (X0 >= Samples.CountX - 1 || Y0 >= Samples.CountY - 1 || IsBlockEmpty(X0, Y0, 1))
		{
			return;
		}
		const FVector3f Up(0.f, 0.f, 1.f);
		const int32 X1 = X0 + 1;
		const int32 Y1 = Y0 + 1;
		const int32 V00 = VertexAt(X0, Y0);
		const int32 V10 = VertexAt(X1, Y0);
		const int32 V11 = VertexAt(X1, Y1);
		const int32 V01 = VertexAt(X0, Y1);
		if (((X0 + Y0) & 1) == 0)
		{
			Builder.AddTriangle(V00, V10, V11, 0, Up);
			Builder.AddTriangle(V00, V11, V01, 0, Up);
		}
		else
		{
			Builder.AddTriangle(V00, V10, V01, 0, Up);
			Builder.AddTriangle(V10, V11, V01, 0, Up);
		}
		EmitSkirts(X0, Y0);
	}

	/** True when no snow is meshed in the cell: outside the tile, or a cell without snow. */
	bool IsCellOpen(int32 X, int32 Y) const
	{
		return X < 0 || Y < 0 || X >= Samples.CountX - 1 || Y >= Samples.CountY - 1 || IsBlockEmpty(X, Y, 1);
	}

	/**
	 * Closes the edge of a cell that borders no snow with a skirt down to the ground. Tiles are meshed apart, and
	 * their snow surfaces differ by a few centimetres at the shared border; without a skirt the ground shows
	 * through as a thin dark crack. The skirt's lower vertices have depth zero, so they stay on the ground.
	 */
	void EmitSkirt(int32 XA, int32 YA, int32 XB, int32 YB, const FVector3f& Outward)
	{
		const int32 TopA = VertexAt(XA, YA);
		const int32 TopB = VertexAt(XB, YB);
		const int32 BottomA = SkirtBottom(XA, YA);
		const int32 BottomB = SkirtBottom(XB, YB);
		Builder.AddQuad(TopA, TopB, BottomB, BottomA, 0, Outward);
	}

	/** A vertex under a sample, on the ground with no snow depth. */
	int32 SkirtBottom(int32 X, int32 Y)
	{
		const int32 Index = Samples.Index(X, Y);
		const FVector3f Position(X * SnowCellMetres * SnowMetresToCm, Y * SnowCellMetres * SnowMetresToCm, (Samples.Ground[Index] - SnowSkirtDrop) * SnowMetresToCm);
		const FVector2f UV(float(Tile.Origin.X) + X * SnowCellMetres, float(Tile.Origin.Y) + Y * SnowCellMetres);
		FColor Color = SnowVertexColor(Samples, Index);
		Color.R = 0;
		return Builder.AddVertex(Position, UV, Color);
	}

	/** The skirts of every side of a cell that has no snow next to it. */
	void EmitSkirts(int32 X0, int32 Y0)
	{
		const int32 X1 = X0 + 1;
		const int32 Y1 = Y0 + 1;
		if (IsCellOpen(X0 - 1, Y0))
		{
			EmitSkirt(X0, Y0, X0, Y1, FVector3f(-1.f, 0.f, 0.f));
		}
		if (IsCellOpen(X0 + 1, Y0))
		{
			EmitSkirt(X1, Y0, X1, Y1, FVector3f(1.f, 0.f, 0.f));
		}
		if (IsCellOpen(X0, Y0 - 1))
		{
			EmitSkirt(X0, Y0, X1, Y0, FVector3f(0.f, -1.f, 0.f));
		}
		if (IsCellOpen(X0, Y0 + 1))
		{
			EmitSkirt(X0, Y1, X1, Y1, FVector3f(0.f, 1.f, 0.f));
		}
	}

	const FWorldTileData& Tile;
	const FSnowSamples& Samples;
	FWorldMeshBuilder& Builder;
	TArray<int32> VertexOfSample;
};

// --- roofs ----------------------------------------------------------------------------------------------------

/** A roof position quantised to a centimetre, so triangles that share a corner share its vertex. */
FIntVector SnowRoofKey(const FVector3f& Position)
{
	return FIntVector(FMath::RoundToInt(Position.X), FMath::RoundToInt(Position.Y), FMath::RoundToInt(Position.Z));
}

uint64 SnowEdgeKey(int32 A, int32 B)
{
	return (uint64(FMath::Min(A, B)) << 32) | uint64(FMath::Max(A, B));
}

/** A roof edge with a single top triangle: an eave or a gable edge, where the snow rolls over. */
struct FSnowRoofRim
{
	int32 CornerA = INDEX_NONE;
	int32 CornerB = INDEX_NONE;
	FVector2f Outward = FVector2f::ZeroVector;
	float Depth = 0.f;
};

/** Snow cap over every roof top face of the buildings, with a rounded lip along the boundary edges. */
void BuildRoofSnow(const FWorldTileData& Tile, const FWorldTileMeshes& Meshes, FWorldMeshBuilder& Snow)
{
	const FWorldMeshBuilder& Buildings = Meshes.Buildings;
	const FVector2f TileOrigin(Tile.Origin);
	TMap<FIntVector, int32> CornerOfPosition;
	TArray<FVector3f> Corners;
	TArray<float> CornerDepths;
	TArray<FIntVector3> RoofTriangles;
	for (int32 Triangle = 0; Triangle < Buildings.NumTriangles(); ++Triangle)
	{
		const int32 Material = Buildings.GetTriangleMaterial(Triangle);
		if (!Meshes.MaterialNames.IsValidIndex(Material) || !Meshes.MaterialNames[Material].StartsWith(TEXT("Roof_")) || Meshes.MaterialNames[Material] == TEXT("Roof_Glass"))
		{
			continue;
		}
		const FIntVector3& Source = Buildings.GetTriangle(Triangle);
		const FVector3f& A = Buildings.GetPosition(Source.X);
		const FVector3f& B = Buildings.GetPosition(Source.Y);
		const FVector3f& C = Buildings.GetPosition(Source.Z);
		const FVector3f Face = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
		if (Face.Z < 0.2f)
		{
			continue; // undersides and steep faces
		}
		const float Depth = Meshes.MaterialNames[Material] == TEXT("Roof_Flat") ? 0.16f : 0.09f;
		FIntVector3 Mapped;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVector3f& Position = Buildings.GetPosition(Source[Corner]);
			int32& Found = CornerOfPosition.FindOrAdd(SnowRoofKey(Position), INDEX_NONE);
			if (Found == INDEX_NONE)
			{
				Found = Corners.Add(Position);
				CornerDepths.Add(Depth);
			}
			Mapped[Corner] = Found;
		}
		RoofTriangles.Add(Mapped);
	}
	// Edges used by one triangle are boundaries.
	TMap<uint64, int32> EdgeUses;
	for (const FIntVector3& Triangle : RoofTriangles)
	{
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			EdgeUses.FindOrAdd(SnowEdgeKey(Triangle[Edge], Triangle[(Edge + 1) % 3]), 0)++;
		}
	}
	TArray<int32> SnowVertexOfCorner;
	SnowVertexOfCorner.Init(INDEX_NONE, Corners.Num());
	const auto VertexOf = [&](int32 Corner)
	{
		if (SnowVertexOfCorner[Corner] == INDEX_NONE)
		{
			const FVector3f& Position = Corners[Corner];
			const FVector3f Lifted(Position.X, Position.Y, Position.Z + CornerDepths[Corner] * SnowMetresToCm);
			SnowVertexOfCorner[Corner] = Snow.AddVertex(Lifted, FVector2f(Position.X, Position.Y) / SnowMetresToCm + TileOrigin,
				FColor(uint8(FMath::Clamp(FMath::RoundToInt(CornerDepths[Corner] / SnowDepthPerColorStep), 0, 255)), 0, 0, 0));
		}
		return SnowVertexOfCorner[Corner];
	};
	const FVector3f Up(0.f, 0.f, 1.f);
	for (const FIntVector3& Triangle : RoofTriangles)
	{
		Snow.AddTriangle(VertexOf(Triangle.X), VertexOf(Triangle.Y), VertexOf(Triangle.Z), 0, Up);
	}
	// Rounded lip: rows going out and down from the boundary edge.
	struct FLipRow { float Out; float DepthShare; };
	const FLipRow LipRows[] = {{0.04f, 0.85f}, {0.08f, 0.35f}, {0.09f, -0.2f}, {0.03f, -0.15f}};
	for (const FIntVector3& Triangle : RoofTriangles)
	{
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			const int32 CornerA = Triangle[Edge];
			const int32 CornerB = Triangle[(Edge + 1) % 3];
			if (EdgeUses[SnowEdgeKey(CornerA, CornerB)] != 1)
			{
				continue;
			}
			const FVector3f& PositionA = Corners[CornerA];
			const FVector3f& PositionB = Corners[CornerB];
			const FVector2f EdgeDirection = FVector2f(PositionB.X - PositionA.X, PositionB.Y - PositionA.Y).GetSafeNormal();
			if (EdgeDirection.IsNearlyZero())
			{
				continue;
			}
			const FVector3f& Third = Corners[Triangle[(Edge + 2) % 3]];
			FVector2f Outward(-EdgeDirection.Y, EdgeDirection.X);
			const FVector2f Middle = FVector2f(PositionA.X + PositionB.X, PositionA.Y + PositionB.Y) * 0.5f;
			if (FVector2f::DotProduct(Outward, FVector2f(Third.X, Third.Y) - Middle) > 0.f)
			{
				Outward = -Outward;
			}
			const float Depth = CornerDepths[CornerA];
			int32 PreviousA = VertexOf(CornerA);
			int32 PreviousB = VertexOf(CornerB);
			for (const FLipRow& Row : LipRows)
			{
				const FVector3f Offset(Outward.X * Row.Out * SnowMetresToCm, Outward.Y * Row.Out * SnowMetresToCm, Row.DepthShare * Depth * SnowMetresToCm);
				const FColor Color(uint8(FMath::Clamp(FMath::RoundToInt(FMath::Max(Row.DepthShare, 0.f) * Depth / SnowDepthPerColorStep), 0, 255)), 0, 0, 0);
				const int32 NextA = Snow.AddVertex(PositionA + Offset, FVector2f(PositionA.X, PositionA.Y) / SnowMetresToCm + TileOrigin, Color);
				const int32 NextB = Snow.AddVertex(PositionB + Offset, FVector2f(PositionB.X, PositionB.Y) / SnowMetresToCm + TileOrigin, Color);
				const FVector3f Hint = Row.DepthShare < 0.f ? FVector3f(0.f, 0.f, -1.f) : FVector3f(Outward.X, Outward.Y, 1.f);
				Snow.AddQuad(PreviousA, PreviousB, NextB, NextA, 0, Hint);
				PreviousA = NextA;
				PreviousB = NextB;
			}
		}
	}
}
}

void BuildWorldSnow(const FWorldTileData& Tile, const FWorldTileMeshes& Meshes, FWorldSnowMeshes& Out)
{
	const double StartSeconds = FPlatformTime::Seconds();
	FSnowSamples Samples;
	InitSamples(Tile, Samples);
	RasterizeGround(Meshes, Samples);
	FillUncoveredSamples(Samples);
	StampRoadAndFootwayEdges(Tile, Samples);
	for (const FWorldBuilding& Building : Tile.Buildings)
	{
		StampBuildingWalls(Building, Samples);
	}
	ComputeSnowSurface(Tile, Samples);
	FSnowFineGround FineGround;
	RasterizeFineGround(Meshes, Samples, FineGround);
	KeepSnowAboveGround(FineGround, Samples);

	FWorldMeshBuilder Snow;
	FSnowGridMesher(Tile, Samples, Snow).Build();
	BuildRoofSnow(Tile, Meshes, Snow);


	// Near tiles have 4 x 4 ground chunks (WorldTileMesher.cpp); the snow follows them.
	const int32 PerSide = 4;
	const FVector2f ChunkSize = Tile.Size * SnowMetresToCm / float(PerSide);
	Out.ChunksPerSide = PerSide;
	Out.ChunkSizeCm = ChunkSize;
	Out.Chunks = Snow.ToDynamicMeshes(PerSide * PerSide, [&](const FVector3f& Centroid)
	{
		const int32 Column = FMath::Clamp(FMath::FloorToInt(Centroid.X / ChunkSize.X), 0, PerSide - 1);
		const int32 Row = FMath::Clamp(FMath::FloorToInt(Centroid.Y / ChunkSize.Y), 0, PerSide - 1);
		return Row * PerSide + Column;
	});

	int64 Triangles = 0;
	int64 Bytes = 0;
	for (const FDynamicMesh3& Chunk : Out.Chunks)
	{
		Triangles += Chunk.TriangleCount();
		Bytes += Chunk.GetByteCount();
	}
	UE_LOG(LogTemp, Log, TEXT("SNOWMESH tile origin %.0f,%.0f: %lld triangles, %.1f MB, built in %.0f ms"), Tile.Origin.X, Tile.Origin.Y,
		Triangles, double(Bytes) / (1024.0 * 1024.0), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}
