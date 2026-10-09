#include "Map3DTileBuilder.h"

#include "ConstrainedDelaunay2.h"
#include "Map3DMeshBuilder.h"
#include "MapPalette.h"
#include "Polygon2.h"
#include "WorldTileData.h"

using namespace UE::Geometry;

namespace
{
// Heights of the flat layers above the ground, metres. The depth buffer resolves a few centimetres at any map distance.
constexpr float GreenHeight = 0.04f;
constexpr float WaterHeight = 0.08f;
constexpr float FootwayHeight = 0.12f;
constexpr float RoadHeight = 0.16f;
constexpr float MarkingHeight = 0.2f;

/** Cell size of the green area grid, metres. */
constexpr float GreenCellMeters = 2.5f;

/** Markings are drawn at least this wide, metres, so they stay visible at map scale. */
constexpr float MinimumMarkingWidth = 0.28f;

/** Roof limits, as in the game's own buildings. */
constexpr float DefaultRoofPitchDegrees = 40.f;
constexpr float MaxRoofRise = 7.5f;
constexpr float MinimumRectangleShare = 0.75f;
constexpr float MinimumEaveHeight = 2.5f;

/** The fill style of a building: shops, offices, public buildings and retail centres are warm, industrial halls darker. */
EMapBuildingStyle StyleOf(const FWorldBuilding& Building)
{
	if (!Building.bTyped)
	{
		return EMapBuildingStyle::Residential;
	}
	switch (Building.ClassId)
	{
	case 9:
	case 13:
	case 14:
		return EMapBuildingStyle::Commercial;
	case 12:
		return EMapBuildingStyle::Industrial;
	default:
		return EMapBuildingStyle::Residential;
	}
}

/** Typed roof shape ids (building_types.py ROOF_NAMES) that become a ridge roof. */
constexpr uint8 RoofGabled = 1;
constexpr uint8 RoofHipped = 2;
constexpr uint8 RoofHalfHipped = 3;
constexpr uint8 RoofMansard = 4;
constexpr uint8 RoofGambrel = 5;
constexpr uint8 RoofPyramidal = 6;

bool IsPitchedShape(uint8 Shape)
{
	return Shape >= RoofGabled && Shape <= RoofPyramidal;
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

/** Triangulates a polygon with holes; false when it cannot. */
bool Triangulate(const FWorldPolygon& Polygon, TArray<FVector2f>& OutVertices, TArray<FIntVector3>& OutTriangles)
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
	if (!bOk)
	{
		return false;
	}
	for (const FVector2d& Vertex : Triangulator.Vertices)
	{
		OutVertices.Add(FVector2f(Vertex));
	}
	for (const FIndex3i& Triangle : Triangulator.Triangles)
	{
		OutTriangles.Add(FIntVector3(Triangle.A, Triangle.B, Triangle.C));
	}
	return true;
}

/** Adds a polygon as a flat layer. */
void AddFlatPolygon(const FWorldPolygon& Polygon, float Height, EMap3DColor Color, FMap3DMeshBuilder& Out)
{
	TArray<FVector2f> Vertices;
	TArray<FIntVector3> Triangles;
	if (Triangulate(Polygon, Vertices, Triangles))
	{
		Out.AddFlatTriangles(Vertices, Triangles, Height, Color);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Ground, water, roads
// ---------------------------------------------------------------------------------------------------------------------

/** Whether the land cover at a grid vertex is meadow or forest, which the map paints green. */
bool IsGreenCover(const FWorldTileGrid& Grid, int32 VertexX, int32 VertexY)
{
	const int32 Base = (FMath::Clamp(VertexY, 0, Grid.NumY - 1) * Grid.NumX + FMath::Clamp(VertexX, 0, Grid.NumX - 1)) * 3;
	if (!Grid.Cover.IsValidIndex(Base + 2))
	{
		return false;
	}
	return Grid.Cover[Base] >= 128 || Grid.Cover[Base + 2] >= 128;
}

/** Green areas from the land cover grid, as runs of equal cells per row. */
void AddGreenAreas(const FWorldTileData& Tile, FMap3DMeshBuilder& Out)
{
	const FWorldTileGrid& Grid = Tile.Grid;
	if (Grid.NumX < 2 || Grid.NumY < 2 || Grid.Cover.Num() < Grid.NumX * Grid.NumY * 3)
	{
		return;
	}
	const int32 CellVertices = FMath::Max(1, FMath::RoundToInt(GreenCellMeters / Grid.CellSize));
	const float CellMeters = CellVertices * Grid.CellSize;
	const int32 CellsX = (Grid.NumX - 1) / CellVertices;
	const int32 CellsY = (Grid.NumY - 1) / CellVertices;
	for (int32 CellY = 0; CellY < CellsY; ++CellY)
	{
		int32 RunStart = INDEX_NONE;
		for (int32 CellX = 0; CellX <= CellsX; ++CellX)
		{
			const bool bGreen = CellX < CellsX && IsGreenCover(Grid, CellX * CellVertices + CellVertices / 2, CellY * CellVertices + CellVertices / 2);
			if (bGreen && RunStart == INDEX_NONE)
			{
				RunStart = CellX;
			}
			if (!bGreen && RunStart != INDEX_NONE)
			{
				Out.AddFlatQuad(FVector2f(RunStart * CellMeters, CellY * CellMeters), FVector2f(CellX * CellMeters, CellY * CellMeters),
					FVector2f(CellX * CellMeters, (CellY + 1) * CellMeters), FVector2f(RunStart * CellMeters, (CellY + 1) * CellMeters), GreenHeight, EMap3DColor::Green);
				RunStart = INDEX_NONE;
			}
		}
	}
}

/** Water, footways and roads from the surface polygons. */
void AddSurfaces(const FWorldTileData& Tile, FMap3DMeshBuilder& Out)
{
	for (const FWorldSurface& Surface : Tile.Surfaces)
	{
		if (!Tile.Names.IsValidIndex(Surface.Material))
		{
			continue;
		}
		const FString& Name = Tile.Names[Surface.Material];
		if (Name == TEXT("Water"))
		{
			AddFlatPolygon(Surface.Polygon, WaterHeight, EMap3DColor::Water, Out);
		}
		else if (Name == TEXT("Pavement") || Name.StartsWith(TEXT("Path_")))
		{
			AddFlatPolygon(Surface.Polygon, FootwayHeight, EMap3DColor::Footway, Out);
		}
		else if (Name.StartsWith(TEXT("Road_")))
		{
			AddFlatPolygon(Surface.Polygon, RoadHeight, EMap3DColor::Road, Out);
		}
	}
}

/** The part of a polyline between two distances along it, including the vertices in between. */
void SlicePolyline(const TArray<FVector2f>& Points, const TArray<float>& Lengths, float Start, float End, TArray<FVector2f>& OutSlice)
{
	OutSlice.Reset();
	const auto PointAt = [&](float Distance)
	{
		for (int32 Index = 1; Index < Points.Num(); ++Index)
		{
			if (Distance <= Lengths[Index] || Index == Points.Num() - 1)
			{
				const float SegmentLength = FMath::Max(Lengths[Index] - Lengths[Index - 1], 1e-6f);
				return FMath::Lerp(Points[Index - 1], Points[Index], FMath::Clamp((Distance - Lengths[Index - 1]) / SegmentLength, 0.f, 1.f));
			}
		}
		return Points.Last();
	};
	OutSlice.Add(PointAt(Start));
	for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
	{
		if (Lengths[Index] > Start && Lengths[Index] < End)
		{
			OutSlice.Add(Points[Index]);
		}
	}
	OutSlice.Add(PointAt(End));
}

/** One marking line, as one ribbon or as dashes along it. */
void AddMarking(const FWorldMarking& Marking, FMap3DMeshBuilder& Out)
{
	if (Marking.Points.Num() < 2)
	{
		return;
	}
	TArray<float> Lengths = {0.f};
	for (int32 Index = 1; Index < Marking.Points.Num(); ++Index)
	{
		Lengths.Add(Lengths.Last() + FVector2f::Distance(Marking.Points[Index - 1], Marking.Points[Index]));
	}
	const float Total = Lengths.Last();
	const float Width = FMath::Max(Marking.Width, MinimumMarkingWidth);
	TArray<FVector2f> Slice;
	if (Marking.DashOn <= 0.f)
	{
		Out.AddRibbon(Marking.Points, Width, MarkingHeight, EMap3DColor::Marking);
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
			SlicePolyline(Marking.Points, Lengths, Start, End, Slice);
			Out.AddRibbon(Slice, Width, MarkingHeight, EMap3DColor::Marking);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Buildings
// ---------------------------------------------------------------------------------------------------------------------

/** The colours of one building's walls and roof. */
struct FBuildingColors
{
	EMap3DColor Wall;
	EMap3DColor Roof;
};

FBuildingColors ColorsOf(const FWorldBuilding& Building)
{
	switch (StyleOf(Building))
	{
	case EMapBuildingStyle::Commercial:
		return FBuildingColors{EMap3DColor::WallCommercial, EMap3DColor::RoofCommercial};
	case EMapBuildingStyle::Industrial:
		return FBuildingColors{EMap3DColor::WallIndustrial, EMap3DColor::RoofIndustrial};
	default:
		return FBuildingColors{EMap3DColor::WallResidential, EMap3DColor::RoofResidential};
	}
}

/** Wall height above the ground: the typed eave height where the tile has one, else the plain one. */
float EaveHeightOf(const FWorldBuilding& Building)
{
	const float Eave = Building.bTyped && Building.TypedEaveHeight > 1.f ? Building.TypedEaveHeight : Building.EaveHeight;
	return FMath::Max(Eave, MinimumEaveHeight);
}

/** Twice the signed area of a ring (shoelace), positive when it runs counter-clockwise in x and y. */
float SignedAreaTwice(const TArray<FVector2f>& Ring)
{
	float Area = 0.f;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2f& A = Ring[Index];
		const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
		Area += A.X * B.Y - B.X * A.Y;
	}
	return Area;
}

/** Shade of a roof plane: flat roofs are full brightness, planes turned from the light are a little darker. */
float RoofShade(const FVector3f& Normal)
{
	return FMath::Clamp(Map3D::ShadeForNormal(Normal) + (1.f - Map3D::ShadeForNormal(FVector3f::UpVector)), 0.8f, 1.f);
}

/** Wall quads around every ring, from the ground up to the eaves. */
void AddWalls(const FWorldBuilding& Building, float EaveHeight, EMap3DColor Color, FMap3DMeshBuilder& Out)
{
	for (int32 RingIndex = 0; RingIndex < Building.Footprint.Rings.Num(); ++RingIndex)
	{
		const TArray<FVector2f>& Ring = Building.Footprint.Rings[RingIndex];
		if (Ring.Num() < 3)
		{
			continue;
		}
		// The footprint's solid lies to the left of a counter-clockwise outline and to the right of a hole's.
		const float Orientation = SignedAreaTwice(Ring) > 0.f ? 1.f : -1.f;
		const float OutwardSign = RingIndex == 0 ? Orientation : -Orientation;
		for (int32 Index = 0; Index < Ring.Num(); ++Index)
		{
			const FVector2f& A = Ring[Index];
			const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
			const FVector2f Edge = B - A;
			if (Edge.SizeSquared() < 0.0001f)
			{
				continue;
			}
			const FVector2f Outward = FVector2f(Edge.Y, -Edge.X).GetSafeNormal() * OutwardSign;
			const FVector3f Normal(Outward.X, Outward.Y, 0.f);
			Out.AddQuad(FVector3f(A, 0.f), FVector3f(B, 0.f), FVector3f(B, EaveHeight), FVector3f(A, EaveHeight), Color, Map3D::ShadeForNormal(Normal), Normal);
		}
	}
}

void AddFlatRoof(const FWorldBuilding& Building, float EaveHeight, EMap3DColor Color, FMap3DMeshBuilder& Out)
{
	AddFlatPolygon(Building.Footprint, EaveHeight, Color, Out);
}

/** The roof planes of a skeleton roof, which sit above the eave line; each plane is one shade. */
void AddSkeletonRoof(const FWorldBuilding& Building, float EaveHeight, EMap3DColor Color, FMap3DMeshBuilder& Out)
{
	for (const FWorldRoofFace& Face : Building.RoofFaces)
	{
		if (Face.Indices.Num() < 3)
		{
			continue;
		}
		const auto Position = [&](int32 Index) { return Face.Positions[Face.Indices[Index]] + FVector3f(0.f, 0.f, EaveHeight); };
		const bool bValid = Face.Positions.IsValidIndex(Face.Indices[0]) && Face.Positions.IsValidIndex(Face.Indices[1]) && Face.Positions.IsValidIndex(Face.Indices[2]);
		if (!bValid)
		{
			continue;
		}
		FVector3f Normal = FVector3f::CrossProduct(Position(2) - Position(0), Position(1) - Position(0)).GetSafeNormal();
		if (Normal.Z < 0.f)
		{
			Normal = -Normal;
		}
		const float Shade = RoofShade(Normal);
		TArray<int32> VertexIds;
		for (const FVector3f& Vertex : Face.Positions)
		{
			VertexIds.Add(Out.AddVertex(Vertex + FVector3f(0.f, 0.f, EaveHeight), Color, Shade));
		}
		for (int32 Index = 0; Index + 2 < Face.Indices.Num(); Index += 3)
		{
			const bool bInRange = Face.Indices[Index] < VertexIds.Num() && Face.Indices[Index + 1] < VertexIds.Num() && Face.Indices[Index + 2] < VertexIds.Num();
			if (bInRange)
			{
				Out.AddTriangle(VertexIds[Face.Indices[Index]], VertexIds[Face.Indices[Index + 1]], VertexIds[Face.Indices[Index + 2]], Normal);
			}
		}
	}
}

/** The roof shape the building gets over its rectangle, or none. */
bool WantsRidgeRoof(const FWorldBuilding& Building)
{
	if (Building.bTyped)
	{
		return IsPitchedShape(Building.TypedRoofShape);
	}
	return Building.RoofShape == EWorldRoofShape::Gabled;
}

/** Whether the footprint is close enough to its rectangle for a ridge roof to look right. */
bool FitsRectangle(const FWorldBuilding& Building, float SideA, float SideB)
{
	if (Building.Footprint.Rings.Num() != 1 || SideA < 2.f || SideB < 2.f)
	{
		return false;
	}
	return FMath::Abs(SignedAreaTwice(Building.Footprint.Rings[0])) * 0.5f > MinimumRectangleShare * SideA * SideB;
}

/** A ridge roof over a rectangle: where the ridge runs, how far the eaves are from it, how high it rises and how far hips cut it short. */
struct FRidgeFrame
{
	FVector2f Centre = FVector2f::ZeroVector;
	FVector2f RidgeAxis = FVector2f(1.f, 0.f);
	FVector2f AcrossAxis = FVector2f(0.f, 1.f);
	float HalfAlong = 0.f;
	float HalfAcross = 0.f;
	float Rise = 0.f;
	/** How far each ridge end is pulled in from the end of the building: 0 for a gable, the half span for a full hip. */
	float Inset = 0.f;
};

/** The ridge direction: the typed ridge yaw where the tile has one (whichever rectangle side it is closest to), else the long side. */
FVector2f ChooseRidgeAxis(const FWorldBuilding& Building, const FVector2f& FirstSide, const FVector2f& SecondSide)
{
	const FVector2f FirstAxis = FirstSide.GetSafeNormal();
	const FVector2f SecondAxis = SecondSide.GetSafeNormal();
	if (!Building.bTyped)
	{
		return FirstSide.Size() >= SecondSide.Size() ? FirstAxis : SecondAxis;
	}
	const float Radians = FMath::DegreesToRadians(Building.RidgeYaw);
	const FVector2f Typed(FMath::Cos(Radians), FMath::Sin(Radians));
	return FMath::Abs(FVector2f::DotProduct(FirstAxis, Typed)) >= FMath::Abs(FVector2f::DotProduct(SecondAxis, Typed)) ? FirstAxis : SecondAxis;
}

/** The hip inset of a roof shape: gables (and the default roof of an untyped building) have none. */
float HipInset(const FWorldBuilding& Building, float HalfAcross)
{
	const uint8 Shape = Building.TypedRoofShape;
	const bool bGabled = !Building.bTyped || Shape == RoofGabled || Shape == RoofMansard || Shape == RoofGambrel;
	if (bGabled)
	{
		return 0.f;
	}
	return Shape == RoofHalfHipped ? 0.5f * HalfAcross : HalfAcross;
}

/** The frame of the building's roof rectangle; false when the plan is no rectangle. */
bool MakeRidgeFrame(const FWorldBuilding& Building, FRidgeFrame& OutFrame)
{
	const FVector2f* Corners = Building.RoofRectangle;
	const FVector2f FirstSide = Corners[1] - Corners[0];
	const FVector2f SecondSide = Corners[2] - Corners[1];
	if (!FitsRectangle(Building, FirstSide.Size(), SecondSide.Size()))
	{
		return false;
	}
	OutFrame.RidgeAxis = ChooseRidgeAxis(Building, FirstSide, SecondSide);
	OutFrame.AcrossAxis = FVector2f(-OutFrame.RidgeAxis.Y, OutFrame.RidgeAxis.X);
	OutFrame.Centre = (Corners[0] + Corners[1] + Corners[2] + Corners[3]) * 0.25f;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const FVector2f FromCentre = Corners[Index] - OutFrame.Centre;
		OutFrame.HalfAlong = FMath::Max(OutFrame.HalfAlong, FMath::Abs(FVector2f::DotProduct(FromCentre, OutFrame.RidgeAxis)));
		OutFrame.HalfAcross = FMath::Max(OutFrame.HalfAcross, FMath::Abs(FVector2f::DotProduct(FromCentre, OutFrame.AcrossAxis)));
	}
	const float PitchDegrees = Building.bTyped ? FMath::Clamp<float>(Building.PitchDegrees, 20.f, 65.f) : DefaultRoofPitchDegrees;
	OutFrame.Rise = FMath::Min(OutFrame.HalfAcross * FMath::Tan(FMath::DegreesToRadians(PitchDegrees)), MaxRoofRise);
	OutFrame.Inset = HipInset(Building, OutFrame.HalfAcross);
	return true;
}

/** The roof planes over a ridge frame: two long slopes, and at each end a vertical gable in the wall colour or a sloping hip. */
void AddRidgeFaces(const FRidgeFrame& Frame, float EaveHeight, const FBuildingColors& Colors, FMap3DMeshBuilder& Out)
{
	const float RidgeHalf = FMath::Max(Frame.HalfAlong - Frame.Inset, 0.f);
	const auto Eave = [&](float Along, float Across) { return FVector3f(Frame.Centre + Frame.RidgeAxis * Along + Frame.AcrossAxis * Across, EaveHeight); };
	const auto Ridge = [&](float Along) { return FVector3f(Frame.Centre + Frame.RidgeAxis * Along, EaveHeight + Frame.Rise); };
	const auto Slope = [&](const FVector3f& A, const FVector3f& B, const FVector3f& C)
	{
		FVector3f Normal = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
		if (Normal.Z < 0.f)
		{
			Normal = -Normal;
		}
		Out.AddTriangle(A, B, C, Colors.Roof, RoofShade(Normal), Normal);
	};
	for (const float Side : {-1.f, 1.f})
	{
		const FVector3f A = Eave(-Frame.HalfAlong, Side * Frame.HalfAcross);
		const FVector3f B = Eave(Frame.HalfAlong, Side * Frame.HalfAcross);
		Slope(A, B, Ridge(RidgeHalf));
		Slope(A, Ridge(RidgeHalf), Ridge(-RidgeHalf));
	}
	for (const float End : {-1.f, 1.f})
	{
		const FVector3f A = Eave(End * Frame.HalfAlong, -Frame.HalfAcross);
		const FVector3f B = Eave(End * Frame.HalfAlong, Frame.HalfAcross);
		const FVector3f Top = Ridge(End * RidgeHalf);
		if (Frame.Inset > 0.f)
		{
			Slope(A, B, Top);
			continue;
		}
		const FVector3f Outward(Frame.RidgeAxis.X * End, Frame.RidgeAxis.Y * End, 0.f);
		Out.AddTriangle(A, B, Top, Colors.Wall, Map3D::ShadeForNormal(Outward), Outward);
	}
}

/** The planes of a gabled, hipped or pyramidal roof over the roof rectangle; false when the plan is no rectangle. */
bool AddRidgeRoof(const FWorldBuilding& Building, float EaveHeight, const FBuildingColors& Colors, FMap3DMeshBuilder& Out)
{
	FRidgeFrame Frame;
	if (!MakeRidgeFrame(Building, Frame))
	{
		return false;
	}
	AddRidgeFaces(Frame, EaveHeight, Colors, Out);
	return true;
}

void AddBuilding(const FWorldBuilding& Building, EMap3DDetail Detail, FMap3DMeshBuilder& Out)
{
	if (Building.Footprint.Rings.IsEmpty() || Building.Footprint.Rings[0].Num() < 3)
	{
		return;
	}
	const FBuildingColors Colors = ColorsOf(Building);
	const float EaveHeight = EaveHeightOf(Building);
	AddWalls(Building, EaveHeight, Colors.Wall, Out);
	if (Detail == EMap3DDetail::Near)
	{
		if (!Building.RoofFaces.IsEmpty())
		{
			AddSkeletonRoof(Building, EaveHeight, Colors.Roof, Out);
			return;
		}
		if (WantsRidgeRoof(Building) && AddRidgeRoof(Building, EaveHeight, Colors, Out))
		{
			return;
		}
	}
	AddFlatRoof(Building, EaveHeight, Colors.Roof, Out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Trees and traffic lights
// ---------------------------------------------------------------------------------------------------------------------

constexpr float MinimumCrownDiameter = 3.f;
constexpr float MaximumCrownRadius = 9.f;

/** A tree: a stubby trunk with a round crown, drawn bigger than life like on a navigation map. */
void AddTree(const FWorldTileData& Tile, const FWorldPlant& Plant, FMap3DMeshBuilder& Out)
{
	if (Tile.Names.IsValidIndex(Plant.Model) && Tile.Names[Plant.Model].Contains(TEXT("shrub")))
	{
		return;
	}
	if (Plant.CrownDiameter < MinimumCrownDiameter)
	{
		return;
	}
	const float CrownRadius = FMath::Min(Plant.CrownDiameter * 0.5f, MaximumCrownRadius);
	const float CrownCentre = FMath::Max(Plant.Height - 0.8f * CrownRadius, CrownRadius * 0.9f + 1.f);
	Out.AddBox(FVector3f(Plant.Position.X, Plant.Position.Y, 0.f), FVector3f(0.3f, 0.3f, 0.5f * CrownCentre), 0.f, EMap3DColor::Trunk, 1.f);
	Out.AddBlob(FVector3f(Plant.Position.X, Plant.Position.Y, CrownCentre), FVector3f(CrownRadius, CrownRadius, 0.85f * CrownRadius), EMap3DColor::Crown, 1.f);
}

/** A traffic light: a pole, a dark housing and three lamps facing the traffic it governs, a little oversized. */
void AddSignalHead(const FWorldPoi& Poi, FMap3DMeshBuilder& Out)
{
	constexpr float Scale = 2.2f;
	const float PoleHeight = FMath::Clamp(Poi.Param0, 2.5f, 7.f);
	const FVector3f Base(Poi.Position.X, Poi.Position.Y, 0.f);
	const float YawRadians = FMath::DegreesToRadians(Poi.YawDegrees + 180.f);
	const FVector3f Face(FMath::Cos(YawRadians), FMath::Sin(YawRadians), 0.f);
	Out.AddBox(Base, FVector3f(0.12f, 0.12f, 0.5f * PoleHeight), 0.f, EMap3DColor::SignalHousing, 1.f);
	const float HousingBottom = PoleHeight - 1.f * Scale;
	Out.AddBox(Base + FVector3f(0.f, 0.f, HousingBottom), FVector3f(0.2f * Scale, 0.2f * Scale, 0.5f * Scale), YawRadians, EMap3DColor::SignalHousing, 1.f);
	const EMap3DColor Lamps[3] = {EMap3DColor::SignalRed, EMap3DColor::SignalYellow, EMap3DColor::SignalGreen};
	for (int32 Lamp = 0; Lamp < 3; ++Lamp)
	{
		const float Height = HousingBottom + (0.84f - 0.34f * Lamp) * Scale;
		Out.AddBox(Base + Face * (0.22f * Scale) + FVector3f(0.f, 0.f, Height - 0.1f * Scale), FVector3f(0.05f * Scale, 0.1f * Scale, 0.1f * Scale), YawRadians, Lamps[Lamp], 1.f);
	}
}
}

void Map3D::BuildTile(const FWorldTileData& Tile, EMap3DDetail Detail, FMap3DMeshBuilder& OutMesh)
{
	AddGreenAreas(Tile, OutMesh);
	AddSurfaces(Tile, OutMesh);
	for (const FWorldBuilding& Building : Tile.Buildings)
	{
		AddBuilding(Building, Detail, OutMesh);
	}
	if (Detail != EMap3DDetail::Near)
	{
		return;
	}
	for (const FWorldMarking& Marking : Tile.Markings)
	{
		AddMarking(Marking, OutMesh);
	}
	for (const FWorldPlant& Plant : Tile.Plants)
	{
		AddTree(Tile, Plant, OutMesh);
	}
	for (const FWorldPoi& Poi : Tile.Pois)
	{
		if (Poi.Kind == EWorldPoiKind::SignalHead)
		{
			AddSignalHead(Poi, OutMesh);
		}
	}
}
