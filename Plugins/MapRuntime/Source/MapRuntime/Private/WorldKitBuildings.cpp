#include "WorldKitBuildings.h"

#include "Engine/StaticMesh.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldMeshBuilder.h"
#include "WorldTileActor.h"
#include "WorldTileData.h"

namespace
{
constexpr float KitMetresToCm = 100.f;
constexpr float KitFoundationDepth = 1.0f;
/** Class ids of the typology (osmimport/building_types.py CLASS_NAMES). */
enum EKitBuildingClass : int32
{
	ClassGruenderzeit = 0, Class1920s = 1, ClassPostwar = 2, ClassSlab = 3, ClassTerraced = 4, ClassSemidetached = 5,
	ClassDetached = 6, ClassVilla = 7, ClassModern = 8, ClassCommercial = 9, ClassFarmhouse = 10, ClassHalfTimbered = 15,
};
/** Roof shape ids of BTYP. */
enum EKitRoofShape : int32
{
	KitRoofFlat = 0, KitRoofGabled = 1, KitRoofHipped = 2, KitRoofHalfHipped = 3, KitRoofMansard = 4, KitRoofGambrel = 5, KitRoofPyramidal = 6,
};

/** Piece names and dimensions of one kit style; an empty name means the style has no such piece. */
struct FKitStyle
{
	const TCHAR* Prefix;
	float StoreyHeight;
	float Thickness;
	float PitchDegrees;
	float EaveOverhang;
	/** Height of the roof's lower edge above the wall top. */
	float EaveLift;
	bool bFlatRoof;
	const TCHAR* WallSolid;
	const TCHAR* WallWindow;
	const TCHAR* WallDoor;
	const TCHAR* Window;
	const TCHAR* Door;
	const TCHAR* Sill;
	const TCHAR* LintelWindow;
	const TCHAR* LintelDoor;
	const TCHAR* DoorExtra;
	const TCHAR* StringBand;
	const TCHAR* TopBand;
	const TCHAR* Dormer;
	float DormerWidth;
	bool bBalconies;
};

const FKitStyle KitStyles[] = {
	{TEXT("brick"), 3.25f, 0.36f, 45.f, 0.42f, 0.10f, false, TEXT("Wall_Solid"), TEXT("Wall_Window"), TEXT("Wall_Door"), TEXT("Window"),
		TEXT("Door"), TEXT("Sill"), TEXT("Lintel_Arch_Window"), TEXT("Lintel_Arch_Door"), TEXT("Door_Step"), TEXT("StringCourse"),
		TEXT("Cornice"), TEXT("Dormer_Gable"), 1.64f, false},
	{TEXT("plaster"), 2.75f, 0.30f, 35.f, 0.32f, 0.08f, false, TEXT("Wall_Solid"), TEXT("Wall_Window"), TEXT("Wall_Door"), TEXT("Window"),
		TEXT("Door"), TEXT("Sill"), TEXT(""), TEXT(""), TEXT("Canopy"), TEXT("StringCourse"), TEXT("Cornice"), TEXT("Dormer_Shed"), 2.1f, false},
	{TEXT("block"), 2.75f, 0.30f, 0.f, 0.f, 0.f, true, TEXT("Wall_Solid"), TEXT("Wall_Window"), TEXT("Wall_Door"), TEXT("Window"),
		TEXT("Door_Glazed"), TEXT("Sill"), TEXT(""), TEXT(""), TEXT(""), TEXT("FloorBand"), TEXT("RoofEdge"), TEXT(""), 0.f, true},
	{TEXT("farm"), 2.25f, 0.25f, 50.f, 0.60f, -0.10f, false, TEXT("Wall_Frame"), TEXT("Wall_Frame_Window"), TEXT("Wall_Frame_Door"),
		TEXT("Window"), TEXT("Door"), TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""), 0.f, false},
};

/** Style for a typology class, or null when the class keeps the plain extruded walls. */
const FKitStyle* KitStyleForClass(int32 ClassId)
{
	switch (ClassId)
	{
	case ClassGruenderzeit:
	case Class1920s:
	case ClassVilla:
	case ClassCommercial: return &KitStyles[0];
	case ClassPostwar:
	case ClassTerraced:
	case ClassSemidetached:
	case ClassDetached: return &KitStyles[1];
	case ClassSlab:
	case ClassModern: return &KitStyles[2];
	case ClassFarmhouse:
	case ClassHalfTimbered: return &KitStyles[3];
	default: return nullptr;
	}
}

float KitHash01(uint64 OsmId, int32 Salt)
{
	uint64 Value = OsmId * 0x9E3779B97F4A7C15ull + uint64(Salt) * 0xBF58476D1CE4E5B9ull;
	Value ^= Value >> 31;
	Value *= 0x94D049BB133111EBull;
	Value ^= Value >> 29;
	return float(Value & 0xFFFFFF) / float(0x1000000);
}

FVector3f KitToCm(const FVector2f& Point, float ZMetres)
{
	return FVector3f(Point.X * KitMetresToCm, Point.Y * KitMetresToCm, ZMetres * KitMetresToCm);
}

/** Signed area of a ring: positive when it runs counter-clockwise in the (x, y) plane. */
float KitSignedArea(const TArray<FVector2f>& Ring)
{
	float Sum = 0.f;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2f& A = Ring[Index];
		const FVector2f& B = Ring[(Index + 1) % Ring.Num()];
		Sum += A.X * B.Y - B.X * A.Y;
	}
	return Sum * 0.5f;
}

/** One facade run of a footprint ring, seen from outside, from its left end to its right end. */
struct FKitEdge
{
	FVector2f Start = FVector2f::ZeroVector;
	/** Along the facade, to the right as seen from outside. */
	FVector2f Tangent = FVector2f::ZeroVector;
	/** Out of the building. */
	FVector2f Normal = FVector2f::ZeroVector;
	float Length = 0.f;
	float YawDegrees = 0.f;
	/** A corner post stands at the left end (this edge places it) or at the right end (the next edge does). */
	bool bPostAtStart = false;
	bool bPostAtEnd = false;
};

/** Edges of one ring with outward normals, and which vertices get corner posts. */
void KitCollectEdges(const TArray<FVector2f>& Ring, bool bHole, TArray<FKitEdge>& Edges)
{
	const int32 Count = Ring.Num();
	const float Orientation = KitSignedArea(Ring) > 0.f ? 1.f : -1.f;
	// The building is on the left of a counter-clockwise exterior ring, so outward is to the right of the travel direction.
	const float OutwardSide = (bHole ? -1.f : 1.f) * Orientation;
	TArray<bool> VertexHasPost;
	VertexHasPost.Init(false, Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2f Before = (Ring[Index] - Ring[(Index + Count - 1) % Count]).GetSafeNormal();
		const FVector2f After = (Ring[(Index + 1) % Count] - Ring[Index]).GetSafeNormal();
		const float Cross = Before.X * After.Y - Before.Y * After.X;
		const bool bRightAngle = FMath::Abs(FVector2f::DotProduct(Before, After)) < 0.26f;
		const bool bConvex = Cross * Orientation * (bHole ? -1.f : 1.f) > 0.f;
		VertexHasPost[Index] = bRightAngle && bConvex && !bHole;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 NextIndex = (Index + 1) % Count;
		const FVector2f Delta = Ring[NextIndex] - Ring[Index];
		const float Length = Delta.Size();
		if (Length < 0.05f)
		{
			continue;
		}
		const FVector2f Direction = Delta / Length;
		const FVector2f Normal = OutwardSide > 0.f ? FVector2f(Direction.Y, -Direction.X) : FVector2f(-Direction.Y, Direction.X);
		const FVector2f Tangent(Normal.Y, -Normal.X);
		FKitEdge Edge;
		Edge.Normal = Normal;
		Edge.Tangent = Tangent;
		Edge.Length = Length;
		const bool bForward = FVector2f::DotProduct(Direction, Tangent) > 0.f;
		Edge.Start = bForward ? Ring[Index] : Ring[NextIndex];
		Edge.bPostAtStart = VertexHasPost[bForward ? Index : NextIndex];
		Edge.bPostAtEnd = VertexHasPost[bForward ? NextIndex : Index];
		Edge.YawDegrees = FMath::RadiansToDegrees(FMath::Atan2(Tangent.Y, Tangent.X));
		Edges.Add(Edge);
	}
}

/** Share of the exterior perimeter that runs parallel or perpendicular to the longest edge. */
float KitOrthogonalShare(const TArray<FVector2f>& Ring)
{
	FVector2f Dominant = FVector2f(1.f, 0.f);
	float Longest = 0.f;
	float Perimeter = 0.f;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2f Delta = Ring[(Index + 1) % Ring.Num()] - Ring[Index];
		Perimeter += Delta.Size();
		if (Delta.Size() > Longest)
		{
			Longest = Delta.Size();
			Dominant = Delta.GetSafeNormal();
		}
	}
	float Aligned = 0.f;
	for (int32 Index = 0; Index < Ring.Num(); ++Index)
	{
		const FVector2f Delta = Ring[(Index + 1) % Ring.Num()] - Ring[Index];
		const float Length = Delta.Size();
		if (Length < 0.01f)
		{
			continue;
		}
		const float Dot = FMath::Abs(FVector2f::DotProduct(Delta / Length, Dominant));
		if (Dot > 0.97f || Dot < 0.26f)
		{
			Aligned += Length;
		}
	}
	return Perimeter > 0.f ? Aligned / Perimeter : 0.f;
}

/** Bounding rectangle of the exterior ring in the frame of the ridge direction. */
struct FKitRoofFrame
{
	FVector2f Centre = FVector2f::ZeroVector;
	/** Along the ridge and across it. */
	FVector2f AlongAxis = FVector2f(1.f, 0.f);
	FVector2f AcrossAxis = FVector2f(0.f, 1.f);
	float HalfAlong = 0.f;
	float HalfAcross = 0.f;
	float Rectangularity = 0.f;

	FVector2f Point(float Along, float Across) const { return Centre + AlongAxis * Along + AcrossAxis * Across; }
};

FKitRoofFrame MakeKitRoofFrame(const TArray<FVector2f>& Ring, float RidgeYawDegrees)
{
	FKitRoofFrame Frame;
	const float Radians = FMath::DegreesToRadians(RidgeYawDegrees);
	Frame.AlongAxis = FVector2f(FMath::Cos(Radians), FMath::Sin(Radians));
	Frame.AcrossAxis = FVector2f(-Frame.AlongAxis.Y, Frame.AlongAxis.X);
	float MinAlong = MAX_flt, MaxAlong = -MAX_flt, MinAcross = MAX_flt, MaxAcross = -MAX_flt;
	for (const FVector2f& Point : Ring)
	{
		const float Along = FVector2f::DotProduct(Point, Frame.AlongAxis);
		const float Across = FVector2f::DotProduct(Point, Frame.AcrossAxis);
		MinAlong = FMath::Min(MinAlong, Along);
		MaxAlong = FMath::Max(MaxAlong, Along);
		MinAcross = FMath::Min(MinAcross, Across);
		MaxAcross = FMath::Max(MaxAcross, Across);
	}
	Frame.HalfAlong = (MaxAlong - MinAlong) * 0.5f;
	Frame.HalfAcross = (MaxAcross - MinAcross) * 0.5f;
	const float MidAlong = (MaxAlong + MinAlong) * 0.5f;
	const float MidAcross = (MaxAcross + MinAcross) * 0.5f;
	Frame.Centre = Frame.AlongAxis * MidAlong + Frame.AcrossAxis * MidAcross;
	const float BoxArea = FMath::Max(4.f * Frame.HalfAlong * Frame.HalfAcross, 0.01f);
	Frame.Rectangularity = FMath::Abs(KitSignedArea(Ring)) / BoxArea;
	return Frame;
}

/** Everything one building's assembly needs while it is built. */
class FKitAssembler
{
public:
	FKitAssembler(const FWorldBuilding& InBuilding, const FKitStyle& InStyle, const FKitRoofMaterials& InMaterials,
		FWorldKitInstances& InInstances, FWorldMeshBuilder& InRoofBuilder)
		: Building(InBuilding), Style(InStyle), Materials(InMaterials), Instances(InInstances), RoofBuilder(InRoofBuilder)
	{
		StoreyCount = FMath::Clamp<int32>(Building.Storeys, 1, 40);
		GroundScale = Building.GroundHeight / Style.StoreyHeight;
		UpperScale = Building.StoreyHeight / Style.StoreyHeight;
		BaseZ = Building.BaseZ + Building.PlinthMetres;
		WallTopZ = BaseZ + Building.GroundHeight + (StoreyCount - 1) * Building.StoreyHeight;
		const float FrontRadians = FMath::DegreesToRadians(Building.FrontYaw);
		FrontDirection = FVector2f(FMath::Cos(FrontRadians), FMath::Sin(FrontRadians));
	}

	/** Walls, roof and decorations; returns true when a flat roof surface is still to be added at WallTopZ. */
	bool Build()
	{
		TArray<FKitEdge> Edges;
		for (int32 RingIndex = 0; RingIndex < Building.Footprint.Rings.Num(); ++RingIndex)
		{
			KitCollectEdges(Building.Footprint.Rings[RingIndex], RingIndex > 0, Edges);
		}
		int32 FrontEdge = INDEX_NONE;
		float BestFacing = 0.2f;
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const float Facing = FVector2f::DotProduct(Edges[Index].Normal, FrontDirection) * FMath::Min(Edges[Index].Length, 6.f);
			if (Facing > BestFacing)
			{
				BestFacing = Facing;
				FrontEdge = Index;
			}
		}
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			BuildEdge(Edges[Index], Index == FrontEdge, Index);
		}
		return BuildRoof();
	}

private:
	const FWorldBuilding& Building;
	const FKitStyle& Style;
	const FKitRoofMaterials& Materials;
	FWorldKitInstances& Instances;
	FWorldMeshBuilder& RoofBuilder;
	int32 StoreyCount = 1;
	float GroundScale = 1.f;
	float UpperScale = 1.f;
	float BaseZ = 0.f;
	float WallTopZ = 0.f;
	FVector2f FrontDirection = FVector2f(1.f, 0.f);

	/** Adds one instance of a kit piece: location in metres (tile-relative), yaw, scale along the wall and in height. */
	void Place(const TCHAR* Piece, const FVector2f& Point, float ZMetres, float YawDegrees, float ScaleX, float ScaleZ = 1.f)
	{
		if (!Piece || !Piece[0])
		{
			return;
		}
		const FName Name(*FString::Printf(TEXT("%s_%s"), Style.Prefix, Piece));
		Instances.Pieces.FindOrAdd(Name).Emplace(FRotator(0.f, YawDegrees, 0.f), FVector(KitToCm(Point, ZMetres)), FVector(ScaleX, 1.f, ScaleZ));
		++Instances.NumInstances;
	}

	void PlaceBand(const FString& Band, const FKitEdge& Edge, float ZMetres, float BayWidth, float FirstOffset, int32 Bays, float Scale)
	{
		const FString Straight = Band + TEXT("_M");
		for (int32 Bay = 0; Bay < Bays; ++Bay)
		{
			Place(*Straight, Edge.Start + Edge.Tangent * (FirstOffset + Bay * BayWidth), ZMetres, Edge.YawDegrees, Scale);
		}
		if (Edge.bPostAtStart)
		{
			Place(*(Band + TEXT("_CornerL")), Edge.Start, ZMetres, Edge.YawDegrees, 1.f);
		}
	}

	float StoreyBaseZ(int32 Storey) const { return Storey == 0 ? BaseZ : BaseZ + Building.GroundHeight + (Storey - 1) * Building.StoreyHeight; }
	float StoreyScale(int32 Storey) const { return Storey == 0 ? GroundScale : UpperScale; }
	/** Z of a band modelled at the top of a kit storey, on the stretched storey. */
	float StoreyTopBandZ(int32 Storey) const { return StoreyBaseZ(Storey) + (StoreyScale(Storey) - 1.f) * Style.StoreyHeight; }

	/** Walls of one facade run on every storey, with bands and openings. */
	void BuildEdge(const FKitEdge& Edge, bool bFront, int32 EdgeIndex)
	{
		const float Reserve = Style.Thickness;
		const float StartOffset = Edge.bPostAtStart ? Reserve : 0.f;
		const float Available = Edge.Length - StartOffset - (Edge.bPostAtEnd ? Reserve : 0.f);
		if (Available < 0.4f)
		{
			return;
		}
		const int32 Bays = FMath::Max(1, FMath::RoundToInt(Available / 2.f));
		const float Scale = Available / (2.f * Bays);
		const float BayWidth = 2.f * Scale;
		const float Facing = FVector2f::DotProduct(Edge.Normal, FrontDirection);
		const bool bSide = FMath::Abs(Facing) < 0.7f;
		const bool bPartyWall = bSide && IsPartyWall(Edge);
		const int32 DoorBay = bFront && Bays >= 1 ? Bays / 2 : INDEX_NONE;
		// Below the raised ground floor the wall continues into the ground, so sloping terrain leaves no gap.
		const float FoundationScale = (Building.PlinthMetres + KitFoundationDepth) / Style.StoreyHeight;
		for (int32 Bay = 0; Bay < Bays; ++Bay)
		{
			Place(Style.WallSolid, Edge.Start + Edge.Tangent * (StartOffset + Bay * BayWidth), Building.BaseZ - KitFoundationDepth,
				Edge.YawDegrees, Scale, FoundationScale);
		}
		if (Edge.bPostAtStart)
		{
			Place(TEXT("Corner_L"), Edge.Start, Building.BaseZ - KitFoundationDepth, Edge.YawDegrees, 1.f, FoundationScale);
		}
		for (int32 Storey = 0; Storey < StoreyCount; ++Storey)
		{
			const float Z = StoreyBaseZ(Storey);
			const float ScaleZ = StoreyScale(Storey);
			if (Edge.bPostAtStart)
			{
				Place(TEXT("Corner_L"), Edge.Start, Z, Edge.YawDegrees, 1.f, ScaleZ);
			}
			for (int32 Bay = 0; Bay < Bays; ++Bay)
			{
				const FVector2f Origin = Edge.Start + Edge.Tangent * (StartOffset + Bay * BayWidth);
				BuildBay(Edge, Origin, Z, ScaleZ, Scale, Storey, Bay, Bays, DoorBay, bSide, bPartyWall, bFront, EdgeIndex);
			}
		}
		BuildBands(Edge, StartOffset, BayWidth, Bays, Scale);
	}

	/** Which wall piece and openings one bay gets. */
	void BuildBay(const FKitEdge& Edge, const FVector2f& Origin, float Z, float ScaleZ, float ScaleX, int32 Storey, int32 Bay, int32 Bays,
		int32 DoorBay, bool bSide, bool bPartyWall, bool bFront, int32 EdgeIndex)
	{
		const float Yaw = Edge.YawDegrees;
		const bool bGroundDoor = Storey == 0 && Bay == DoorBay && Bays >= 1 && Edge.Length >= 3.f;
		if (bGroundDoor)
		{
			Place(Style.WallDoor, Origin, Z, Yaw, ScaleX, ScaleZ);
			Place(Style.Door, Origin, Z, Yaw, ScaleX, ScaleZ);
			Place(Style.LintelDoor, Origin, Z, Yaw, ScaleX, ScaleZ);
			Place(Style.DoorExtra, Origin, Z, Yaw, ScaleX);
			return;
		}
		const bool bSolid = bPartyWall || Edge.Length < 2.2f || (bSide && (Bay % 2) == 1)
			|| KitHash01(Building.OsmId, EdgeIndex * 97 + Bay * 13 + Storey) < 0.04f;
		if (bSolid)
		{
			Place(Style.WallSolid, Origin, Z, Yaw, ScaleX, ScaleZ);
			return;
		}
		if (Style.bBalconies && Storey >= 1 && !bSide && (Bay % 3) == 1)
		{
			Place(TEXT("Wall_Balcony"), Origin, Z, Yaw, ScaleX, ScaleZ);
			Place(TEXT("Window_Balcony"), Origin, Z, Yaw, ScaleX, ScaleZ);
			Place(TEXT("Balcony_Rail"), Origin, Z, Yaw, ScaleX);
			return;
		}
		Place(Style.WallWindow, Origin, Z, Yaw, ScaleX, ScaleZ);
		Place(Style.Window, Origin, Z, Yaw, ScaleX, ScaleZ);
		Place(Style.Sill, Origin, Z, Yaw, ScaleX, ScaleZ);
		Place(Style.LintelWindow, Origin, Z, Yaw, ScaleX, ScaleZ);
	}

	/** True for a side wall that touches a neighbour, judged from the closed-left and closed-right flags. */
	bool IsPartyWall(const FKitEdge& Edge) const
	{
		const bool bClosedLeft = (Building.TypeFlags & 4) != 0;
		const bool bClosedRight = (Building.TypeFlags & 8) != 0;
		const FVector2f Right(FrontDirection.Y, -FrontDirection.X);
		const float Side = FVector2f::DotProduct(Edge.Normal, Right);
		return (Side > 0.f && bClosedRight) || (Side < 0.f && bClosedLeft);
	}

	/** Plinth, string courses, top band and gutter along one facade run. */
	void BuildBands(const FKitEdge& Edge, float StartOffset, float BayWidth, int32 Bays, float Scale)
	{
		PlaceBand(TEXT("Plinth"), Edge, BaseZ, BayWidth, StartOffset, Bays, Scale);
		if (Style.StringBand && Style.StringBand[0])
		{
			for (int32 Storey = 0; Storey < StoreyCount - 1; ++Storey)
			{
				PlaceBand(Style.StringBand, Edge, StoreyTopBandZ(Storey), BayWidth, StartOffset, Bays, Scale);
			}
		}
		if (Style.TopBand && Style.TopBand[0])
		{
			PlaceBand(Style.TopBand, Edge, StoreyTopBandZ(StoreyCount - 1), BayWidth, StartOffset, Bays, Scale);
		}
	}

	// ------------------------------------------------------------------------------------------------ roof

	/** Adds a convex roof polygon (cm) facing up, with its underside so the eaves aren't see-through. */
	void AddRoofFace(const TArray<FVector3f>& Points, int32 Material)
	{
		if (Points.Num() < 3)
		{
			return;
		}
		FVector3f Normal = FVector3f::ZeroVector;
		for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
		{
			Normal += FVector3f::CrossProduct(Points[Index] - Points[0], Points[Index + 1] - Points[0]);
		}
		Normal = Normal.GetSafeNormal();
		if (Normal.Z < 0.f)
		{
			Normal = -Normal;
		}
		const FVector3f EaveDirection = (Points[1] - Points[0]).GetSafeNormal();
		const FVector3f SlopeDirection = FVector3f::CrossProduct(Normal, EaveDirection).GetSafeNormal();
		for (const float Side : {1.f, -1.f})
		{
			TArray<int32> Indices;
			for (const FVector3f& Point : Points)
			{
				const FVector3f Offset = (Point - Points[0]) / KitMetresToCm;
				Indices.Add(RoofBuilder.AddVertex(Point, FVector2f(FVector3f::DotProduct(Offset, EaveDirection), -FMath::Abs(FVector3f::DotProduct(Offset, SlopeDirection)))));
			}
			for (int32 Index = 1; Index + 1 < Indices.Num(); ++Index)
			{
				RoofBuilder.AddTriangle(Indices[0], Indices[Index], Indices[Index + 1], Materials.RoofTiles, Normal * Side);
			}
		}
	}

	/** One gable triangle or trapezoid in the wall plane at the end of the ridge, facing outward. */
	void AddGableFace(const TArray<FVector3f>& Points, const FVector3f& Outward)
	{
		TArray<int32> Indices;
		for (const FVector3f& Point : Points)
		{
			Indices.Add(RoofBuilder.AddVertex(Point, FVector2f(FVector3f::DotProduct(Point, FVector3f(Outward.Y, -Outward.X, 0.f)) / KitMetresToCm,
				-Point.Z / KitMetresToCm)));
		}
		for (int32 Index = 1; Index + 1 < Indices.Num(); ++Index)
		{
			RoofBuilder.AddTriangle(Indices[0], Indices[Index], Indices[Index + 1], Materials.Gable, Outward);
		}
	}

	/** Roof surfaces, ridge caps, gutters, dormers and chimney. Returns true when the roof is flat. */
	bool BuildRoof()
	{
		const TArray<FVector2f>& Ring = Building.Footprint.Rings[0];
		const FKitRoofFrame Frame = MakeKitRoofFrame(Ring, Building.RidgeYaw);
		const int32 Shape = Building.TypedRoofShape;
		const bool bPitchedShape = Shape == KitRoofGabled || Shape == KitRoofHipped || Shape == KitRoofHalfHipped || Shape == KitRoofMansard
			|| Shape == KitRoofGambrel || Shape == KitRoofPyramidal;
		const float Slope = FMath::Tan(FMath::DegreesToRadians(Style.PitchDegrees));
		const float Overhang = Style.EaveOverhang;
		const float Rise = (FMath::Min(Frame.HalfAcross, Frame.HalfAlong * (Shape == KitRoofGabled ? 100.f : 1.f)) + Overhang) * Slope;
		const bool bFits = Frame.Rectangularity > 0.8f && Building.Footprint.Rings.Num() == 1 && Rise < 7.5f && Frame.HalfAcross > 1.5f;
		if (Style.bFlatRoof || !bPitchedShape || !bFits)
		{
			return true;
		}
		const float EaveZ = WallTopZ + Style.EaveLift;
		const bool bGable = Shape == KitRoofGabled || Shape == KitRoofGambrel;
		BuildPitchedRoof(Frame, EaveZ, Slope, bGable, Shape == KitRoofPyramidal);
		AddGutters(Frame, EaveZ, bGable);
		AddRoofDetails(Frame, EaveZ, Slope, bGable);
		return false;
	}

	void BuildPitchedRoof(const FKitRoofFrame& Frame, float EaveZ, float Slope, bool bGable, bool bPyramid)
	{
		const float Overhang = Style.EaveOverhang;
		const float GableOverhang = 0.25f;
		const float HalfAlong = Frame.HalfAlong;
		const float HalfAcross = Frame.HalfAcross;
		const float AcrossOuter = HalfAcross + Overhang;
		const float AlongOuter = HalfAlong + (bGable ? GableOverhang : Overhang);
		float RidgeHalf = bGable ? AlongOuter : HalfAlong - HalfAcross;
		const bool bApex = bPyramid || RidgeHalf < 0.3f;
		RidgeHalf = bApex ? 0.f : RidgeHalf;
		const float RidgeZ = EaveZ + (bApex ? FMath::Min(AcrossOuter, AlongOuter) : AcrossOuter) * Slope;
		const auto P = [&](float Along, float Across, float Z) { return KitToCm(Frame.Point(Along, Across), Z); };
		const TArray<FVector3f> Front = {P(AlongOuter, -AcrossOuter, EaveZ), P(-AlongOuter, -AcrossOuter, EaveZ), P(-RidgeHalf, 0.f, RidgeZ), P(RidgeHalf, 0.f, RidgeZ)};
		const TArray<FVector3f> Back = {P(-AlongOuter, AcrossOuter, EaveZ), P(AlongOuter, AcrossOuter, EaveZ), P(RidgeHalf, 0.f, RidgeZ), P(-RidgeHalf, 0.f, RidgeZ)};
		AddRoofFace(Front, Materials.RoofTiles);
		AddRoofFace(Back, Materials.RoofTiles);
		if (bGable)
		{
			AddGableEnd(Frame, HalfAlong, 1.f, EaveZ, Slope, RidgeZ);
			AddGableEnd(Frame, -HalfAlong, -1.f, EaveZ, Slope, RidgeZ);
			return;
		}
		const TArray<FVector3f> EndPlus = {P(AlongOuter, AcrossOuter, EaveZ), P(AlongOuter, -AcrossOuter, EaveZ), P(RidgeHalf, 0.f, RidgeZ)};
		const TArray<FVector3f> EndMinus = {P(-AlongOuter, -AcrossOuter, EaveZ), P(-AlongOuter, AcrossOuter, EaveZ), P(-RidgeHalf, 0.f, RidgeZ)};
		AddRoofFace(EndPlus, Materials.RoofTiles);
		AddRoofFace(EndMinus, Materials.RoofTiles);
		if (!bApex)
		{
			PlaceRidge(Frame, RidgeHalf * 2.f, RidgeZ);
		}
	}

	/** The wall above the eave line at one end of a gabled roof, up to where the roof planes meet it. */
	void AddGableEnd(const FKitRoofFrame& Frame, float AlongPosition, float Direction, float EaveZ, float Slope, float RidgeZ)
	{
		const float HalfAcross = Frame.HalfAcross;
		const float BaseZ0 = WallTopZ;
		const float PlaneZAtWall = EaveZ + Style.EaveOverhang * Slope;
		const float TopZ = EaveZ + (HalfAcross + Style.EaveOverhang) * Slope;
		const auto P = [&](float Across, float Z) { return KitToCm(Frame.Point(AlongPosition, Across), Z); };
		const FVector2f OutwardAxis = Frame.AlongAxis * Direction;
		const TArray<FVector3f> Face = {P(-HalfAcross, BaseZ0), P(HalfAcross, BaseZ0), P(HalfAcross, PlaneZAtWall), P(0.f, TopZ), P(-HalfAcross, PlaneZAtWall)};
		AddGableFace(Face, FVector3f(OutwardAxis.X, OutwardAxis.Y, 0.f));
		(void)RidgeZ;
	}

	void PlaceRidge(const FKitRoofFrame& Frame, float RidgeLength, float RidgeZ)
	{
		const int32 Pieces = FMath::Max(1, FMath::CeilToInt(RidgeLength / 2.f));
		const float Scale = RidgeLength / (2.f * Pieces);
		const FVector2f Direction = -Frame.AlongAxis;
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			Place(TEXT("Roof_Tile_Ridge"), Frame.Point(RidgeLength * 0.5f - Piece * 2.f * Scale, 0.f), RidgeZ, Yaw, Scale);
		}
	}

	/** Gutters under the long eaves (and the short ones of a hipped roof). */
	void AddGutters(const FKitRoofFrame& Frame, float EaveZ, bool bGable)
	{
		if (Style.bFlatRoof || Style.EaveOverhang <= 0.f || FString(Style.Prefix) == TEXT("farm"))
		{
			return;
		}
		PlaceGutterSide(Frame, -1.f, EaveZ, false);
		PlaceGutterSide(Frame, 1.f, EaveZ, false);
		if (!bGable)
		{
			PlaceGutterSide(Frame, -1.f, EaveZ, true);
			PlaceGutterSide(Frame, 1.f, EaveZ, true);
		}
	}

	/** Gutter pieces along the roof rectangle's side facing Sign (along the across axis, or the along axis for the ends). */
	void PlaceGutterSide(const FKitRoofFrame& Frame, float Sign, float EaveZ, bool bEnd)
	{
		const FVector2f Normal = bEnd ? Frame.AlongAxis * Sign : Frame.AcrossAxis * Sign;
		const FVector2f Tangent(Normal.Y, -Normal.X);
		const float Length = 2.f * (bEnd ? Frame.HalfAcross : Frame.HalfAlong);
		const float HalfNormal = bEnd ? Frame.HalfAlong : Frame.HalfAcross;
		const FVector2f Start = Frame.Centre + Normal * HalfNormal - Tangent * (Length * 0.5f);
		const int32 Pieces = FMath::Max(1, FMath::RoundToInt(Length / 2.f));
		const float Scale = Length / (2.f * Pieces);
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Tangent.Y, Tangent.X));
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			Place(TEXT("Gutter_M"), Start + Tangent * (Piece * 2.f * Scale), EaveZ, Yaw, Scale);
		}
	}

	/** Dormers on the street slope and a chimney on the ridge. */
	void AddRoofDetails(const FKitRoofFrame& Frame, float EaveZ, float Slope, bool bGable)
	{
		const bool bAttic = (Building.TypeFlags & 1) != 0;
		if (bAttic && Style.Dormer && Style.Dormer[0] && Frame.HalfAlong * 2.f >= 6.f)
		{
			PlaceDormers(Frame, EaveZ, Slope);
		}
		if (KitHash01(Building.OsmId, 777) < 0.6f && Frame.HalfAlong > 3.f)
		{
			const FVector2f Direction = -Frame.AlongAxis;
			const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
			const float RidgeZ = EaveZ + (Frame.HalfAcross + Style.EaveOverhang) * Slope;
			const float Along = Frame.HalfAlong - 1.8f - 2.f * KitHash01(Building.OsmId, 778);
			Place(TEXT("Chimney"), Frame.Point(Along, 0.f) + Direction * 0.f - Frame.AcrossAxis * 0.4f, RidgeZ - 0.35f, Yaw, 1.f);
		}
		(void)bGable;
	}

	void PlaceDormers(const FKitRoofFrame& Frame, float EaveZ, float Slope)
	{
		const float FacingAcross = FVector2f::DotProduct(Frame.AcrossAxis, FrontDirection);
		const float Sign = FacingAcross >= 0.f ? 1.f : -1.f;
		const FVector2f Normal = Frame.AcrossAxis * Sign;
		const FVector2f Tangent(Normal.Y, -Normal.X);
		const float Length = Frame.HalfAlong * 2.f;
		const int32 Count = FMath::Clamp(FMath::FloorToInt((Length - 1.5f) / 3.8f), 1, 4);
		const float Slot = Length / Count;
		const FVector2f Start = Frame.Centre + Normal * Frame.HalfAcross - Tangent * (Length * 0.5f);
		const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Tangent.Y, Tangent.X));
		const float SurfaceInset = 2.0f;
		const float Z = EaveZ + (SurfaceInset + Style.EaveOverhang) * Slope - 0.03f;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Along = Slot * (Index + 0.5f) - Style.DormerWidth * 0.5f;
			Place(Style.Dormer, Start + Tangent * Along - Normal * SurfaceInset, Z, Yaw, 1.f);
		}
	}
};
}

FString KitMeshPath(const FName& PieceName)
{
	return FString::Printf(TEXT("/Game/World/Kit/Meshes/SM_%s.SM_%s"), *PieceName.ToString(), *PieceName.ToString());
}

bool IsKitBuilding(const FWorldBuilding& Building)
{
	if (!Building.bTyped || !KitStyleForClass(Building.ClassId) || Building.Footprint.Rings.IsEmpty())
	{
		return false;
	}
	const TArray<FVector2f>& Ring = Building.Footprint.Rings[0];
	if (Ring.Num() < 4 || FMath::Abs(KitSignedArea(Ring)) < 25.f || Building.Storeys == 0)
	{
		return false;
	}
	return KitOrthogonalShare(Ring) > 0.75f;
}

bool BuildKitBuilding(const FWorldTileData& Tile, const FWorldBuilding& Building, const FKitRoofMaterials& Materials,
	FWorldKitInstances& Instances, FWorldMeshBuilder& RoofBuilder, float& FlatRoofZ)
{
	const FKitStyle* Style = KitStyleForClass(Building.ClassId);
	FKitAssembler Assembler(Building, *Style, Materials, Instances, RoofBuilder);
	const bool bFlat = Assembler.Build();
	++Instances.NumBuildings;
	FlatRoofZ = Building.BaseZ + Building.PlinthMetres + Building.GroundHeight + (FMath::Clamp<int32>(Building.Storeys, 1, 40) - 1) * Building.StoreyHeight;
	(void)Tile;
	return bFlat;
}

namespace
{
constexpr int32 KitPiecesPerStepCount = 6;

/**
 * Big pieces shadow the street; the small trim (sills, bands, frames, gutters, steps) is too small to show in a shadow
 * and costs a lot in the virtual shadow maps, where every instance is rendered into several pages.
 */
bool PieceCastsShadow(const FName& PieceName)
{
	const FString Name = PieceName.ToString();
	for (const TCHAR* Big : {TEXT("_Wall_"), TEXT("_Corner_"), TEXT("Cornice"), TEXT("RoofEdge"), TEXT("Chimney"), TEXT("Dormer"), TEXT("Balcony"), TEXT("Canopy")})
	{
		if (Name.Contains(Big))
		{
			return true;
		}
	}
	return false;
}

/** Kit meshes by piece name, loaded on first use and kept alive; a missing mesh is remembered as null. */
UStaticMesh* FindKitMeshCached(const FName& PieceName)
{
	static TMap<FName, TStrongObjectPtr<UStaticMesh>> Cache;
	if (const TStrongObjectPtr<UStaticMesh>* Found = Cache.Find(PieceName))
	{
		return Found->Get();
	}
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *KitMeshPath(PieceName), nullptr, LOAD_NoWarn);
	if (!Mesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Building kit mesh %s missing; run Scripts/import_building_kit.py"), *PieceName.ToString());
	}
	Cache.Add(PieceName, TStrongObjectPtr<UStaticMesh>(Mesh));
	return Mesh;
}
}

bool AddKitInstancesStep(AWorldTileActor& Actor, const FWorldKitInstances& Kit, int32 Step)
{
	TArray<FName> Names;
	Kit.Pieces.GetKeys(Names);
	Names.Sort(FNameLexicalLess());
	const int32 First = Step * KitPiecesPerStepCount;
	const int32 Last = FMath::Min(First + KitPiecesPerStepCount, Names.Num());
	for (int32 Index = First; Index < Last; ++Index)
	{
		Actor.AddKitInstances(FindKitMeshCached(Names[Index]), Kit.Pieces[Names[Index]], PieceCastsShadow(Names[Index]));
	}
	return Last >= Names.Num();
}
