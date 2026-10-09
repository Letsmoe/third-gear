#include "WorldTileMesher.h"

#include "ConstrainedDelaunay2.h"
#include "Polygon2.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "WorldFurniture.h"
#include "WorldTileData.h"

using namespace UE::Geometry;

namespace
{
constexpr float MetresToCm = 100.f;
constexpr float KerbDepthBelowRoad = 0.1f;
constexpr float MarkingLift = 0.01f;
constexpr float MarkingSampleStep = 2.f;
constexpr float BuildingFoundationDepth = 0.6f;
constexpr float RoofPitchDegrees = 40.f;
constexpr float RoofMaxRise = 6.f;
constexpr float RoofOverhang = 0.4f;
constexpr float MaxPlantAnisotropy = 1.6f;
constexpr float BoundaryEpsilon = 0.01f;
const FVector3f Up(0.f, 0.f, 1.f);

/** Settings that change with the detail level. */
struct FDetailSettings
{
	/** Terrain vertex spacing in metres; coarse grids (horizon tiles) use every vertex. */
	float TerrainStepMetres;
	/** Spacing of the extra vertices inside surface polygons, metres; 0 means outline only. */
	float SurfaceGrid;
	/** Longest outline edge before it is split, so surfaces follow the ground. */
	float SurfaceEdge;
	/** Surfaces sit this much higher, so the coarser terrain doesn't poke through. */
	float SurfaceLift;
	/** How far the terrain skirts reach down along tile edges, hiding cracks to coarser neighbours. */
	float SkirtDepth;
	/** Ground chunks along each tile edge. */
	int32 ChunksPerSide;
};

FDetailSettings SettingsFor(EWorldTileDetail Detail)
{
	switch (Detail)
	{
	case EWorldTileDetail::Near: return {1.f, 2.f, 2.f, 0.f, 0.5f, 4};
	case EWorldTileDetail::Middle: return {4.f, 8.f, 8.f, 0.15f, 2.f, 2};
	case EWorldTileDetail::Far: return {8.f, 0.f, 16.f, 0.3f, 4.f, 1};
	}
	return {1.f, 2.f, 2.f, 0.f, 0.5f, 4};
}

FVector3f ToCm(float LocalX, float LocalY, float Z)
{
	return FVector3f(LocalX * MetresToCm, LocalY * MetresToCm, Z * MetresToCm);
}

/** World-metre UV, so textures run on seamlessly across tiles. */
FVector2f WorldUV(const FWorldTileData& Tile, float LocalX, float LocalY)
{
	return FVector2f(float(Tile.Origin.X + LocalX), float(Tile.Origin.Y + LocalY));
}

/** Even-odd test against all rings: inside the outline and outside every hole. */
bool IsInside(const FWorldPolygon& Polygon, const FVector2f& Point)
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

/** Squared distance from a point to the nearest edge of any ring. */
float DistanceToOutlineSquared(const FWorldPolygon& Polygon, const FVector2f& Point)
{
	float Best = MAX_flt;
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		for (int32 Index = 0, Previous = Ring.Num() - 1; Index < Ring.Num(); Previous = Index++)
		{
			const FVector2f& A = Ring[Previous];
			const FVector2f Edge = Ring[Index] - A;
			const float T = FMath::Clamp(FVector2f::DotProduct(Point - A, Edge) / FMath::Max(Edge.SizeSquared(), 1e-12f), 0.f, 1.f);
			Best = FMath::Min(Best, FVector2f::DistSquared(Point, A + Edge * T));
		}
	}
	return Best;
}

/** Ring with edges longer than MaxEdge split evenly. */
TArray<FVector2d> DensifyRing(const TArray<FVector2f>& Ring, float MaxEdge)
{
	TArray<FVector2d> Out;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2f& A = Ring[Index];
		const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
		const int32 Pieces = FMath::Max(1, FMath::CeilToInt(FVector2f::Distance(A, B) / MaxEdge));
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			Out.Add(FVector2d(FMath::Lerp(A, B, float(Piece) / Pieces)));
		}
	}
	return Out;
}

/** Regular grid points inside the polygon, kept away from its outline to avoid slivers. */
TArray<FVector2d> InteriorGridPoints(const FWorldPolygon& Polygon, float Spacing)
{
	TArray<FVector2d> Points;
	if (Spacing <= 0.f || Polygon.Rings.IsEmpty())
	{
		return Points;
	}
	FBox2f Bounds(Polygon.Rings[0]);
	const float MinDistanceSquared = FMath::Square(Spacing * 0.3f);
	for (float Y = FMath::CeilToFloat(Bounds.Min.Y / Spacing) * Spacing; Y < Bounds.Max.Y; Y += Spacing)
	{
		for (float X = FMath::CeilToFloat(Bounds.Min.X / Spacing) * Spacing; X < Bounds.Max.X; X += Spacing)
		{
			const FVector2f Point(X, Y);
			if (IsInside(Polygon, Point) && DistanceToOutlineSquared(Polygon, Point) > MinDistanceSquared)
			{
				Points.Add(FVector2d(Point));
			}
		}
	}
	return Points;
}

/** Constrained Delaunay triangulation of a polygon with holes plus extra inside points. */
bool TriangulatePolygon(const FWorldPolygon& Polygon, float MaxEdge, float InteriorSpacing,
	TArray<FVector2d>& OutVertices, TArray<FIndex3i>& OutTriangles)
{
	TConstrainedDelaunay2<double> Triangulator;
	Triangulator.bOrientedEdges = false;
	for (const TArray<FVector2f>& Ring : Polygon.Rings)
	{
		if (Ring.Num() >= 3)
		{
			Triangulator.Add(TPolygon2<double>(DensifyRing(Ring, MaxEdge)));
		}
	}
	Triangulator.Vertices.Append(InteriorGridPoints(Polygon, InteriorSpacing));
	const bool bOk = Triangulator.Triangulate([&Polygon](const TArray<FVector2d>& Vertices, const FIndex3i& Triangle)
	{
		const FVector2d Centroid = (Vertices[Triangle.A] + Vertices[Triangle.B] + Vertices[Triangle.C]) / 3.0;
		return IsInside(Polygon, FVector2f(Centroid));
	});
	OutVertices = MoveTemp(Triangulator.Vertices);
	OutTriangles = MoveTemp(Triangulator.Triangles);
	return bOk;
}

/** Material slot of a name, added to the tile's list when new. */
int32 MaterialSlot(FWorldTileMeshes& Meshes, const FString& Name)
{
	return Meshes.MaterialNames.AddUnique(Name);
}

// --- terrain --------------------------------------------------------------------------------------------------

/** Grid vertex coordinates along one axis at the given step, always including the last vertex. */
TArray<int32> GridSteps(int32 Count, int32 Step)
{
	TArray<int32> Steps;
	for (int32 Index = 0; Index < Count - 1; Index += Step)
	{
		Steps.Add(Index);
	}
	Steps.Add(Count - 1);
	return Steps;
}

/** Vertical strip hanging down from a row of terrain vertices, facing away from the tile; INDEX_NONE (holes) breaks it. */
void AddSkirt(FWorldMeshBuilder& Builder, const TArray<int32>& EdgeVertices, float Depth, int32 Material, const FVector3f& Outward)
{
	for (int32 Index = 0; Index + 1 < EdgeVertices.Num(); ++Index)
	{
		const int32 TopA = EdgeVertices[Index];
		const int32 TopB = EdgeVertices[Index + 1];
		if (TopA == INDEX_NONE || TopB == INDEX_NONE)
		{
			continue;
		}
		const FVector3f DepthCm(0.f, 0.f, Depth * MetresToCm);
		const int32 BottomA = Builder.AddVertex(Builder.GetPosition(TopA) - DepthCm, FVector2f::ZeroVector);
		const int32 BottomB = Builder.AddVertex(Builder.GetPosition(TopB) - DepthCm, FVector2f::ZeroVector);
		Builder.AddQuad(TopA, TopB, BottomB, BottomA, Material, Outward);
	}
}

void BuildTerrain(const FWorldTileData& Tile, const FDetailSettings& Settings, FWorldTileMeshes& Meshes)
{
	const FWorldTileGrid& Grid = Tile.Grid;
	const int32 Material = MaterialSlot(Meshes, TEXT("Terrain_Grass"));
	const int32 Step = FMath::Max(1, FMath::RoundToInt(Settings.TerrainStepMetres / Grid.CellSize));
	const TArray<int32> Xs = GridSteps(Grid.NumX, Step);
	const TArray<int32> Ys = GridSteps(Grid.NumY, Step);
	FWorldMeshBuilder& Builder = Meshes.Ground;
	const int32 First = Builder.NumVertices();
	for (const int32 Y : Ys)
	{
		for (const int32 X : Xs)
		{
			const float LocalX = X * Grid.CellSize;
			const float LocalY = Y * Grid.CellSize;
			Builder.AddVertex(ToCm(LocalX, LocalY, Grid.TerrainAtVertex(X, Y)), WorldUV(Tile, LocalX, LocalY), Grid.CoverAtVertex(X, Y));
		}
	}
	const int32 Columns = Xs.Num();
	const auto IsHoleAt = [&](int32 Column, int32 Row) { return Grid.IsHole(Xs[Column], Ys[Row]); };
	const auto VertexAt = [&](int32 Column, int32 Row) { return IsHoleAt(Column, Row) ? INDEX_NONE : First + Row * Columns + Column; };
	for (int32 Row = 0; Row + 1 < Ys.Num(); ++Row)
	{
		for (int32 Column = 0; Column + 1 < Columns; ++Column)
		{
			if (IsHoleAt(Column, Row) || IsHoleAt(Column + 1, Row) || IsHoleAt(Column, Row + 1) || IsHoleAt(Column + 1, Row + 1))
			{
				continue;
			}
			Builder.AddQuad(VertexAt(Column, Row), VertexAt(Column + 1, Row), VertexAt(Column + 1, Row + 1), VertexAt(Column, Row + 1), Material, Up);
		}
	}
	TArray<int32> North, South, West, East;
	for (int32 Column = 0; Column < Columns; ++Column)
	{
		North.Add(VertexAt(Column, 0));
		South.Add(VertexAt(Column, Ys.Num() - 1));
	}
	for (int32 Row = 0; Row < Ys.Num(); ++Row)
	{
		West.Add(VertexAt(0, Row));
		East.Add(VertexAt(Columns - 1, Row));
	}
	AddSkirt(Builder, North, Settings.SkirtDepth, Material, FVector3f(0.f, -1.f, 0.f));
	AddSkirt(Builder, South, Settings.SkirtDepth, Material, FVector3f(0.f, 1.f, 0.f));
	AddSkirt(Builder, West, Settings.SkirtDepth, Material, FVector3f(-1.f, 0.f, 0.f));
	AddSkirt(Builder, East, Settings.SkirtDepth, Material, FVector3f(1.f, 0.f, 0.f));
}

// --- surfaces and kerbs ---------------------------------------------------------------------------------------

/** Paths and similar small surfaces are left out in the distance. */
bool IsSurfaceShown(const FString& Material, EWorldTileDetail Detail)
{
	if (Detail != EWorldTileDetail::Far)
	{
		return true;
	}
	return Material.StartsWith(TEXT("Road_")) || Material == TEXT("Water");
}

void BuildSurface(const FWorldTileData& Tile, const FWorldSurface& Surface, const FDetailSettings& Settings, int32 Material,
	FWorldMeshBuilder& Builder)
{
	TArray<FVector2d> Vertices;
	TArray<FIndex3i> Triangles;
	TriangulatePolygon(Surface.Polygon, Settings.SurfaceEdge, Settings.SurfaceGrid, Vertices, Triangles);
	const int32 First = Builder.NumVertices();
	for (const FVector2d& Vertex : Vertices)
	{
		const float Z = Tile.SurfaceHeightAt(Surface, Vertex.X, Vertex.Y) + Settings.SurfaceLift;
		Builder.AddVertex(ToCm(Vertex.X, Vertex.Y, Z), WorldUV(Tile, Vertex.X, Vertex.Y));
	}
	for (const FIndex3i& Triangle : Triangles)
	{
		Builder.AddTriangle(First + Triangle.A, First + Triangle.B, First + Triangle.C, Material, Up);
	}
}

/** True for an outline edge that only exists because the polygon was cut at the tile border. */
bool IsOnTileBorder(const FWorldTileData& Tile, const FVector2f& A, const FVector2f& B)
{
	const auto Near = [](float Value, float Target) { return FMath::Abs(Value - Target) < BoundaryEpsilon; };
	return (Near(A.X, 0.f) && Near(B.X, 0.f)) || (Near(A.Y, 0.f) && Near(B.Y, 0.f))
		|| (Near(A.X, Tile.Size.X) && Near(B.X, Tile.Size.X)) || (Near(A.Y, Tile.Size.Y) && Near(B.Y, Tile.Size.Y));
}

/** Direction pointing out of the polygon at the middle of edge A-B. */
FVector3f OutwardNormal(const FWorldPolygon& Polygon, const FVector2f& A, const FVector2f& B)
{
	const FVector2f Side = FVector2f(-(B.Y - A.Y), B.X - A.X).GetSafeNormal();
	const FVector2f Probe = (A + B) * 0.5f + Side * 0.05f;
	const FVector2f Outward = IsInside(Polygon, Probe) ? -Side : Side;
	return FVector3f(Outward.X, Outward.Y, 0.f);
}

/** Granite kerb faces along the pavement edge, from just below the road up to the pavement surface. */
void BuildKerbs(const FWorldTileData& Tile, const FWorldSurface& Pavement, FWorldTileMeshes& Meshes)
{
	const int32 Material = MaterialSlot(Meshes, TEXT("Kerb"));
	const float KerbHeight = Pavement.Params[0];
	FWorldMeshBuilder& Builder = Meshes.Ground;
	for (const TArray<FVector2f>& Ring : Pavement.Polygon.Rings)
	{
		float Distance = 0.f;
		for (int32 Index = 0; Index < Ring.Num(); ++Index)
		{
			const FVector2f& A = Ring[Index];
			const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
			const float Length = FVector2f::Distance(A, B);
			if (Length < 0.01f || IsOnTileBorder(Tile, A, B))
			{
				Distance += Length;
				continue;
			}
			const float RoadA = Tile.Grid.RoadAt(A.X, A.Y);
			const float RoadB = Tile.Grid.RoadAt(B.X, B.Y);
			const float TopA = RoadA + KerbHeight;
			const float TopB = RoadB + KerbHeight;
			const float BottomA = RoadA - KerbDepthBelowRoad;
			const float BottomB = RoadB - KerbDepthBelowRoad;
			const int32 V0 = Builder.AddVertex(ToCm(A.X, A.Y, BottomA), FVector2f(Distance, -BottomA));
			const int32 V1 = Builder.AddVertex(ToCm(B.X, B.Y, BottomB), FVector2f(Distance + Length, -BottomB));
			const int32 V2 = Builder.AddVertex(ToCm(B.X, B.Y, TopB), FVector2f(Distance + Length, -TopB));
			const int32 V3 = Builder.AddVertex(ToCm(A.X, A.Y, TopA), FVector2f(Distance, -TopA));
			Builder.AddQuad(V0, V1, V2, V3, Material, OutwardNormal(Pavement.Polygon, A, B));
			Distance += Length;
		}
	}
}

void BuildSurfaces(const FWorldTileData& Tile, EWorldTileDetail Detail, const FDetailSettings& Settings, FWorldTileMeshes& Meshes)
{
	for (const FWorldSurface& Surface : Tile.Surfaces)
	{
		const FString& Name = Tile.Names.IsValidIndex(Surface.Material) ? Tile.Names[Surface.Material] : FString(TEXT("Road_Asphalt"));
		if (!IsSurfaceShown(Name, Detail))
		{
			continue;
		}
		BuildSurface(Tile, Surface, Settings, MaterialSlot(Meshes, Name), Meshes.Ground);
		if (Detail == EWorldTileDetail::Near && Name == TEXT("Pavement"))
		{
			BuildKerbs(Tile, Surface, Meshes);
		}
	}
}

// --- markings -------------------------------------------------------------------------------------------------

/** Point and direction at a distance along a polyline with precomputed cumulative lengths. */
void SamplePolyline(const TArray<FVector2f>& Points, const TArray<float>& Lengths, float Distance, FVector2f& OutPoint, FVector2f& OutDirection)
{
	int32 Segment = 0;
	while (Segment + 2 < Points.Num() && Lengths[Segment + 1] < Distance)
	{
		++Segment;
	}
	const float SegmentLength = FMath::Max(Lengths[Segment + 1] - Lengths[Segment], 1e-6f);
	const float T = FMath::Clamp((Distance - Lengths[Segment]) / SegmentLength, 0.f, 1.f);
	OutPoint = FMath::Lerp(Points[Segment], Points[Segment + 1], T);
	OutDirection = (Points[Segment + 1] - Points[Segment]).GetSafeNormal();
}

/** Flat strip of the marking's width between two distances along the line. */
void AddMarkingStrip(const FWorldTileData& Tile, const FWorldMarking& Marking, const TArray<float>& Lengths, float Start, float End,
	int32 Material, FWorldMeshBuilder& Builder)
{
	const int32 Samples = FMath::Max(2, FMath::CeilToInt((End - Start) / MarkingSampleStep) + 1);
	int32 PreviousLeft = INDEX_NONE;
	int32 PreviousRight = INDEX_NONE;
	for (int32 Sample = 0; Sample < Samples; ++Sample)
	{
		const float Distance = FMath::Lerp(Start, End, float(Sample) / (Samples - 1));
		FVector2f Point, Direction;
		SamplePolyline(Marking.Points, Lengths, Distance, Point, Direction);
		const FVector2f Side = FVector2f(-Direction.Y, Direction.X) * (Marking.Width * 0.5f);
		const FVector2f Left = Point + Side;
		const FVector2f Right = Point - Side;
		const float U = Marking.Phase + Distance;
		const int32 LeftVertex = Builder.AddVertex(ToCm(Left.X, Left.Y, Tile.Grid.RoadAt(Left.X, Left.Y) + MarkingLift), FVector2f(U, 0.f));
		const int32 RightVertex = Builder.AddVertex(ToCm(Right.X, Right.Y, Tile.Grid.RoadAt(Right.X, Right.Y) + MarkingLift), FVector2f(U, 1.f));
		if (PreviousLeft != INDEX_NONE)
		{
			Builder.AddQuad(PreviousLeft, LeftVertex, RightVertex, PreviousRight, Material, Up);
		}
		PreviousLeft = LeftVertex;
		PreviousRight = RightVertex;
	}
}

void BuildMarking(const FWorldTileData& Tile, const FWorldMarking& Marking, FWorldTileMeshes& Meshes)
{
	if (Marking.Points.Num() < 2)
	{
		return;
	}
	const FString& Name = Tile.Names.IsValidIndex(Marking.Material) ? Tile.Names[Marking.Material] : FString(TEXT("Marking_White"));
	const int32 Material = MaterialSlot(Meshes, Name);
	TArray<float> Lengths = {0.f};
	for (int32 Index = 1; Index < Marking.Points.Num(); ++Index)
	{
		Lengths.Add(Lengths.Last() + FVector2f::Distance(Marking.Points[Index - 1], Marking.Points[Index]));
	}
	const float Total = Lengths.Last();
	if (Marking.DashOn <= 0.f)
	{
		AddMarkingStrip(Tile, Marking, Lengths, 0.f, Total, Material, Meshes.Markings);
		return;
	}
	// Dashes are laid out along the whole original line; Phase is where this piece starts on it.
	const float Period = Marking.DashOn + Marking.DashOff;
	const float FirstDash = FMath::FloorToFloat(Marking.Phase / Period) * Period - Marking.Phase;
	for (float DashStart = FirstDash; DashStart < Total; DashStart += Period)
	{
		const float Start = FMath::Max(DashStart, 0.f);
		const float End = FMath::Min(DashStart + Marking.DashOn, Total);
		if (End - Start > 0.3f)
		{
			AddMarkingStrip(Tile, Marking, Lengths, Start, End, Material, Meshes.Markings);
		}
	}
}

// --- buildings ------------------------------------------------------------------------------------------------

/** Wall quads around one footprint ring, from below the ground up to the eaves. */
void BuildWalls(const FWorldBuilding& Building, const TArray<FVector2f>& Ring, int32 Material, const FColor& Color, FWorldMeshBuilder& Builder)
{
	const float Bottom = Building.BaseZ - BuildingFoundationDepth;
	const float Top = Building.BaseZ + Building.EaveHeight;
	float Distance = 0.f;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2f& A = Ring[Index];
		const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
		const float Length = FVector2f::Distance(A, B);
		if (Length < 0.01f)
		{
			continue;
		}
		// UV: u along the wall, v = -(height above the building base); the facade material draws windows from it.
		const int32 V0 = Builder.AddVertex(ToCm(A.X, A.Y, Bottom), FVector2f(Distance, BuildingFoundationDepth), Color);
		const int32 V1 = Builder.AddVertex(ToCm(B.X, B.Y, Bottom), FVector2f(Distance + Length, BuildingFoundationDepth), Color);
		const int32 V2 = Builder.AddVertex(ToCm(B.X, B.Y, Top), FVector2f(Distance + Length, -Building.EaveHeight), Color);
		const int32 V3 = Builder.AddVertex(ToCm(A.X, A.Y, Top), FVector2f(Distance, -Building.EaveHeight), Color);
		Builder.AddQuad(V0, V1, V2, V3, Material, OutwardNormal(Building.Footprint, A, B));
		Distance += Length;
	}
}

void BuildFlatRoof(const FWorldTileData& Tile, const FWorldBuilding& Building, int32 Material, FWorldMeshBuilder& Builder)
{
	TArray<FVector2d> Vertices;
	TArray<FIndex3i> Triangles;
	TriangulatePolygon(Building.Footprint, 1000.f, 0.f, Vertices, Triangles);
	const float Z = Building.BaseZ + Building.EaveHeight;
	const int32 First = Builder.NumVertices();
	for (const FVector2d& Vertex : Vertices)
	{
		Builder.AddVertex(ToCm(Vertex.X, Vertex.Y, Z), WorldUV(Tile, Vertex.X, Vertex.Y));
	}
	for (const FIndex3i& Triangle : Triangles)
	{
		Builder.AddTriangle(First + Triangle.A, First + Triangle.B, First + Triangle.C, Material, Up);
	}
}

/** Two roof planes over the rectangle's long axis with a little overhang, plus the gable triangles. */
void BuildGabledRoof(const FWorldBuilding& Building, int32 RoofMaterial, int32 FacadeMaterial, const FColor& Color, FWorldMeshBuilder& Builder)
{
	FVector2f Corners[4] = {Building.RoofRectangle[0], Building.RoofRectangle[1], Building.RoofRectangle[2], Building.RoofRectangle[3]};
	if (FVector2f::Distance(Corners[0], Corners[1]) < FVector2f::Distance(Corners[1], Corners[2]))
	{
		// Make Corners[0] to Corners[1] a long side.
		const FVector2f First = Corners[0];
		Corners[0] = Corners[1];
		Corners[1] = Corners[2];
		Corners[2] = Corners[3];
		Corners[3] = First;
	}
	const float Length = FVector2f::Distance(Corners[0], Corners[1]);
	const float Span = FVector2f::Distance(Corners[1], Corners[2]);
	const float Slope = FMath::Tan(FMath::DegreesToRadians(RoofPitchDegrees));
	const float Rise = FMath::Min(Span * 0.5f * Slope, RoofMaxRise);
	const float Eave = Building.BaseZ + Building.EaveHeight;
	const float Ridge = Eave + Rise;
	const FVector2f Centre = (Corners[0] + Corners[1] + Corners[2] + Corners[3]) * 0.25f;
	const FVector2f RidgeA = (Corners[0] + Corners[3]) * 0.5f;
	const FVector2f RidgeB = (Corners[1] + Corners[2]) * 0.5f;

	// Eaves pushed out by the overhang (dropping with the roof slope), ridge ends pushed out along the ridge.
	const auto PushOut = [&](const FVector2f& Point, float HalfExtent) { return Centre + (Point - Centre) * (1.f + RoofOverhang / FMath::Max(HalfExtent, 1.f)); };
	FVector2f Outer[4];
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Outer[Index] = PushOut(Corners[Index], FMath::Min(Length, Span) * 0.5f);
	}
	const FVector2f OuterRidgeA = PushOut(RidgeA, Length * 0.5f);
	const FVector2f OuterRidgeB = PushOut(RidgeB, Length * 0.5f);
	const float EaveLow = Eave - RoofOverhang * Slope;
	const float SlopeLength = FMath::Sqrt(FMath::Square(Span * 0.5f + RoofOverhang) + FMath::Square(Rise + RoofOverhang * Slope));

	const auto AddPlane = [&](const FVector2f& EaveStart, const FVector2f& EaveEnd, const FVector2f& RidgeEnd, const FVector2f& RidgeStart)
	{
		const float EaveLength = FVector2f::Distance(EaveStart, EaveEnd);
		const FVector3f P0 = ToCm(EaveStart.X, EaveStart.Y, EaveLow);
		const FVector3f P1 = ToCm(EaveEnd.X, EaveEnd.Y, EaveLow);
		const FVector3f P2 = ToCm(RidgeEnd.X, RidgeEnd.Y, Ridge);
		const FVector3f P3 = ToCm(RidgeStart.X, RidgeStart.Y, Ridge);
		const FVector3f Facing = FVector3f::CrossProduct(P1 - P0, P3 - P0).Z > 0.f ? FVector3f::CrossProduct(P1 - P0, P3 - P0) : FVector3f::CrossProduct(P3 - P0, P1 - P0);
		// Top face, then a separate underside so the overhang isn't see-through from below.
		for (const float Side : {1.f, -1.f})
		{
			const int32 V0 = Builder.AddVertex(P0, FVector2f(0.f, 0.f));
			const int32 V1 = Builder.AddVertex(P1, FVector2f(EaveLength, 0.f));
			const int32 V2 = Builder.AddVertex(P2, FVector2f(EaveLength, -SlopeLength));
			const int32 V3 = Builder.AddVertex(P3, FVector2f(0.f, -SlopeLength));
			Builder.AddQuad(V0, V1, V2, V3, RoofMaterial, Facing * Side);
		}
	};
	AddPlane(Outer[0], Outer[1], OuterRidgeB, OuterRidgeA);
	AddPlane(Outer[2], Outer[3], OuterRidgeA, OuterRidgeB);

	// Gable triangles on the short sides, in the wall material.
	const float WallHeight = Building.EaveHeight;
	const auto AddGable = [&](const FVector2f& A, const FVector2f& B, const FVector2f& Top)
	{
		const float Width = FVector2f::Distance(A, B);
		const int32 V0 = Builder.AddVertex(ToCm(A.X, A.Y, Eave), FVector2f(0.f, -WallHeight), Color);
		const int32 V1 = Builder.AddVertex(ToCm(B.X, B.Y, Eave), FVector2f(Width, -WallHeight), Color);
		const int32 V2 = Builder.AddVertex(ToCm(Top.X, Top.Y, Ridge), FVector2f(Width * 0.5f, -(WallHeight + Rise)), Color);
		const FVector2f Outward = Top - Centre;
		Builder.AddTriangle(V0, V1, V2, FacadeMaterial, FVector3f(Outward.X, Outward.Y, 0.f));
	};
	AddGable(Corners[3], Corners[0], RidgeA);
	AddGable(Corners[1], Corners[2], RidgeB);
}

void BuildBuilding(const FWorldTileData& Tile, const FWorldBuilding& Building, FWorldTileMeshes& Meshes)
{
	if (Building.Footprint.Rings.IsEmpty() || Building.Footprint.Rings[0].Num() < 3)
	{
		return;
	}
	const FString Facade = Tile.Names.IsValidIndex(Building.Facade) ? Tile.Names[Building.Facade] : FString(TEXT("Facade_Plaster"));
	const FString Roof = Tile.Names.IsValidIndex(Building.Roof) ? Tile.Names[Building.Roof] : FString(TEXT("Roof_Flat"));
	const int32 FacadeMaterial = MaterialSlot(Meshes, Facade);
	const int32 RoofMaterial = MaterialSlot(Meshes, Roof);
	const FColor Color(Building.Tint, Building.Variation, 0, 255);
	for (const TArray<FVector2f>& Ring : Building.Footprint.Rings)
	{
		BuildWalls(Building, Ring, FacadeMaterial, Color, Meshes.Buildings);
	}
	if (Building.RoofShape == EWorldRoofShape::Gabled)
	{
		BuildGabledRoof(Building, RoofMaterial, FacadeMaterial, Color, Meshes.Buildings);
	}
	else
	{
		BuildFlatRoof(Tile, Building, RoofMaterial, Meshes.Buildings);
	}
}

// --- plants ---------------------------------------------------------------------------------------------------

/** Instance transform scaled from the model's natural size to the measured crown and height. */
FTransform PlantTransform(const FWorldPlant& Plant, const FVector2f& ModelSize)
{
	const float ScaleXY = Plant.CrownDiameter / FMath::Max(ModelSize.X, 0.1f);
	float ScaleZ = Plant.Height / FMath::Max(ModelSize.Y, 0.1f);
	ScaleZ = FMath::Clamp(ScaleZ, ScaleXY / MaxPlantAnisotropy, ScaleXY * MaxPlantAnisotropy);
	return FTransform(FRotator(0.f, Plant.YawDegrees, 0.f), FVector(ToCm(Plant.Position.X, Plant.Position.Y, Plant.Position.Z)),
		FVector(ScaleXY, ScaleXY, ScaleZ));
}

void BuildPlants(const FWorldTileData& Tile, EWorldTileDetail Detail, const FWorldMeshingContext& Context, FWorldTileMeshes& Meshes)
{
	TMap<int32, int32> GroupByModel;
	for (const FWorldPlant& Plant : Tile.Plants)
	{
		const FString& Model = Tile.Names.IsValidIndex(Plant.Model) ? Tile.Names[Plant.Model] : FString();
		const FVector2f* Size = Context.PlantModelSizes.Find(Model);
		if (!Size || (Detail != EWorldTileDetail::Near && Model == TEXT("shrub")))
		{
			continue;
		}
		int32& Group = GroupByModel.FindOrAdd(Plant.Model, INDEX_NONE);
		if (Group == INDEX_NONE)
		{
			Group = Meshes.Plants.Add(FWorldPlantInstances{Model, {}});
		}
		Meshes.Plants[Group].Transforms.Add(PlantTransform(Plant, *Size));
		if (Detail == EWorldTileDetail::Near && Plant.TrunkDiameter > 0.f)
		{
			Meshes.TrunkBases.Add(FVector(ToCm(Plant.Position.X, Plant.Position.Y, Plant.Position.Z - 0.5f)));
			Meshes.TrunkDiameters.Add(Plant.TrunkDiameter * MetresToCm);
		}
	}
}
}

/** tg.Furniture 0 leaves lamps, signal poles and signs out, to measure what they cost. */
static TAutoConsoleVariable<int32> CVarFurniture(TEXT("tg.Furniture"), 1, TEXT("1 builds street furniture (lamps, signals, signs), 0 leaves it out."));

FWorldTileMeshes BuildWorldTileMeshes(const FWorldTileData& Tile, EWorldTileDetail Detail, const FWorldMeshingContext& Context)
{
	FWorldTileMeshes Meshes;
	Meshes.Detail = Detail;
	const FDetailSettings Settings = SettingsFor(Detail);
	BuildTerrain(Tile, Settings, Meshes);
	BuildSurfaces(Tile, Detail, Settings, Meshes);
	if (Detail == EWorldTileDetail::Near)
	{
		for (const FWorldMarking& Marking : Tile.Markings)
		{
			BuildMarking(Tile, Marking, Meshes);
		}
	}
	for (const FWorldBuilding& Building : Tile.Buildings)
	{
		BuildBuilding(Tile, Building, Meshes);
	}
	BuildPlants(Tile, Detail, Context, Meshes);
	if (Detail == EWorldTileDetail::Near && !Tile.Pois.IsEmpty() && CVarFurniture.GetValueOnAnyThread() != 0 && !FParse::Param(FCommandLine::Get(), TEXT("NoFurniture")))
	{
		Meshes.Furniture = MakeShared<FWorldFurnitureInstances>();
		BuildWorldFurniture(Tile, *Meshes.Furniture);
	}
	const int32 PerSide = Settings.ChunksPerSide;
	const FVector2f ChunkSize = Tile.Size * MetresToCm / float(PerSide);
	Meshes.ChunksPerSide = PerSide;
	Meshes.ChunkSizeCm = ChunkSize;
	Meshes.GroundChunks = Meshes.Ground.ToDynamicMeshes(PerSide * PerSide, [&](const FVector3f& Centroid)
	{
		const int32 Column = FMath::Clamp(FMath::FloorToInt(Centroid.X / ChunkSize.X), 0, PerSide - 1);
		const int32 Row = FMath::Clamp(FMath::FloorToInt(Centroid.Y / ChunkSize.Y), 0, PerSide - 1);
		return Row * PerSide + Column;
	});
	Meshes.MarkingsMesh = Meshes.Markings.ToDynamicMesh();
	Meshes.BuildingsMesh = Meshes.Buildings.ToDynamicMesh();
	return Meshes;
}
