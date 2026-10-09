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
/** Height of the parapet around flat roofs. */
constexpr float KitParapetHeight = 0.5f;
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

/** A material choice with its weight. */
struct FKitChoice
{
	const TCHAR* Name;
	float Weight;
};

/** Deterministic weighted draw from the choices, seeded by the building. */
const TCHAR* KitPick(uint64 OsmId, int32 Salt, const TArray<FKitChoice>& Choices)
{
	float Total = 0.f;
	for (const FKitChoice& Choice : Choices)
	{
		Total += Choice.Weight;
	}
	float Draw = KitHash01(OsmId, Salt) * Total;
	for (const FKitChoice& Choice : Choices)
	{
		Draw -= Choice.Weight;
		if (Draw <= 0.f)
		{
			return Choice.Name;
		}
	}
	return Choices.Last().Name;
}

/** Name of the facade material instance (/Game/World/Facades/M_<name>) for the walls of a building of this class. */
FString KitFacadeName(const FWorldBuilding& Building)
{
	TArray<FKitChoice> Choices;
	switch (Building.ClassId)
	{
	case ClassGruenderzeit:
		Choices = {{TEXT("BrickGruenderzeit"), 40}, {TEXT("ClinkerDeepRed"), 22}, {TEXT("BrickSooty"), 10}, {TEXT("RenderScratchBeige"), 10},
			{TEXT("RenderScratchWhite"), 8}, {TEXT("ClinkerYellowBrown"), 10}};
		break;
	case Class1920s:
		Choices = {{TEXT("ClinkerDeepRed"), 45}, {TEXT("BrickSooty"), 20}, {TEXT("ClinkerYellowBrown"), 25}, {TEXT("BrickPostwar"), 10}};
		break;
	case ClassPostwar:
		Choices = {{TEXT("RenderScratchWhite"), 22}, {TEXT("RenderScratchBeige"), 18}, {TEXT("RenderScratchPastel"), 10}, {TEXT("RenderScratchGrey"), 8},
			{TEXT("BrickPostwar"), 22}, {TEXT("ClinkerYellowBrown"), 12}, {TEXT("ClinkerDeepRed"), 8}};
		break;
	case ClassSlab:
		Choices = {{TEXT("ConcreteSlab"), 30}, {TEXT("RenderSmoothWhite"), 20}, {TEXT("RenderSmoothGrey"), 20}, {TEXT("RenderSmoothPastel"), 15},
			{TEXT("ConcreteSmooth"), 15}};
		break;
	case ClassTerraced:
	case ClassSemidetached:
	case ClassDetached:
		Choices = {{TEXT("RenderScratchWhite"), 22}, {TEXT("RenderScratchBeige"), 16}, {TEXT("RenderScratchPastel"), 8}, {TEXT("RenderScratchGrey"), 4},
			{TEXT("BrickPostwar"), 20}, {TEXT("ClinkerDeepRed"), 14}, {TEXT("ClinkerYellowBrown"), 16}};
		break;
	case ClassVilla:
		Choices = {{TEXT("RenderScratchWhite"), 30}, {TEXT("RenderScratchBeige"), 20}, {TEXT("ClinkerDeepRed"), 30}, {TEXT("BrickGruenderzeit"), 20}};
		break;
	case ClassModern:
		Choices = {{TEXT("RenderSmoothWhite"), 30}, {TEXT("RenderSmoothGrey"), 25}, {TEXT("RenderSmoothBeige"), 15}, {TEXT("ConcreteSmooth"), 15},
			{TEXT("RenderSmoothPastel"), 15}};
		break;
	case ClassCommercial:
		Choices = {{TEXT("BrickGruenderzeit"), 35}, {TEXT("ClinkerDeepRed"), 25}, {TEXT("RenderSmoothBeige"), 20}, {TEXT("RenderScratchWhite"), 20}};
		break;
	case ClassHalfTimbered:
		Choices = {{TEXT("RenderScratchWhite"), 40}, {TEXT("ClinkerDeepRed"), 35}, {TEXT("RenderScratchBeige"), 25}};
		break;
	default:
		Choices = {{TEXT("ClinkerDeepRed"), 70}, {TEXT("BrickSooty"), 30}};
		break;
	}
	return FString::Printf(TEXT("Facade_%s"), KitPick(Building.OsmId, 4101, Choices));
}

/** Name of the roof tile material (/Game/World/Facades/M_<name>) of a pitched roof. */
FString KitRoofTileName(const FWorldBuilding& Building)
{
	TArray<FKitChoice> Choices;
	switch (Building.ClassId)
	{
	case ClassGruenderzeit:
	case Class1920s:
	case ClassVilla:
	case ClassCommercial:
	case ClassHalfTimbered:
		if (Building.TypedRoofShape == 4 || Building.TypedRoofShape == 5)
		{
			Choices = {{TEXT("Roof_Slate"), 70}, {TEXT("Roof_ClayOld"), 30}};
		}
		else
		{
			Choices = {{TEXT("Roof_ClayOld"), 35}, {TEXT("Roof_Clay"), 35}, {TEXT("Roof_Slate"), 30}};
		}
		break;
	case ClassFarmhouse:
		Choices = {{TEXT("Roof_Clay"), 50}, {TEXT("Roof_ClayOld"), 50}};
		break;
	default:
		Choices = {{TEXT("Roof_Clay"), 35}, {TEXT("Roof_ConcreteAnthracite"), 30}, {TEXT("Roof_ConcreteGrey"), 15}, {TEXT("Roof_ClayOld"), 20}};
		break;
	}
	return KitPick(Building.OsmId, 4102, Choices);
}

/** Flat roofs: gravel on the older and plainer blocks, bitumen sheet on shops, offices and new buildings. */
const TCHAR* KitFlatRoofName(const FWorldBuilding& Building)
{
	const bool bBitumen = Building.ClassId == ClassModern || Building.ClassId == ClassCommercial || KitHash01(Building.OsmId, 4103) < 0.3f;
	return bBitumen ? TEXT("Roof_Bitumen") : TEXT("Roof_Gravel");
}

/** Which kind of roof a building gets. */
enum class EKitRoofMode : uint8
{
	Flat,
	Skeleton,
	Rectangle,
};

/** Everything one building's assembly needs while it is built. */
class FKitAssembler
{
public:
	FKitAssembler(const FWorldBuilding& InBuilding, const FKitStyle& InStyle, const TFunction<int32(const FString&)>& InFindSlot,
		FWorldKitInstances& InInstances, FWorldMeshBuilder& InRoofBuilder)
		: Building(InBuilding), Style(InStyle), FindSlot(InFindSlot), Instances(InInstances), RoofBuilder(InRoofBuilder)
	{
		StoreyCount = FMath::Clamp<int32>(Building.Storeys, 1, 40);
		GroundScale = Building.GroundHeight / Style.StoreyHeight;
		UpperScale = Building.StoreyHeight / Style.StoreyHeight;
		BaseZ = Building.BaseZ + Building.PlinthMetres;
		WallTopZ = BaseZ + Building.GroundHeight + (StoreyCount - 1) * Building.StoreyHeight;
		const float FrontRadians = FMath::DegreesToRadians(Building.FrontYaw);
		FrontDirection = FVector2f(FMath::Cos(FrontRadians), FMath::Sin(FrontRadians));
		FacadeName = KitFacadeName(Building);
		RoofTileName = KitRoofTileName(Building);
		RoofFrame = MakeKitRoofFrame(Building.Footprint.Rings[0], Building.RidgeYaw);
		PitchSlope = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp<float>(Building.PitchDegrees, 20.f, 65.f)));
		RoofMode = DecideRoofMode();
	}

	/** Walls, roof and decorations. Fills Flat when the caller has to add a flat roof surface. */
	void Build(FKitFlatRoof& Flat)
	{
		for (int32 RingIndex = 0; RingIndex < Building.Footprint.Rings.Num(); ++RingIndex)
		{
			KitCollectEdges(Building.Footprint.Rings[RingIndex], RingIndex > 0, Edges);
		}
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
		BuildRoof();
		if (RoofMode == EKitRoofMode::Flat)
		{
			Flat.bNeeded = true;
			Flat.Z = WallTopZ + 0.02f;
			Flat.Material = FindSlot(KitFlatRoofName(Building));
		}
	}

private:
	const FWorldBuilding& Building;
	const FKitStyle& Style;
	const TFunction<int32(const FString&)>& FindSlot;
	FWorldKitInstances& Instances;
	FWorldMeshBuilder& RoofBuilder;
	int32 StoreyCount = 1;
	float GroundScale = 1.f;
	float UpperScale = 1.f;
	float BaseZ = 0.f;
	float WallTopZ = 0.f;
	float PitchSlope = 1.f;
	FVector2f FrontDirection = FVector2f(1.f, 0.f);
	FString FacadeName;
	FString RoofTileName;
	FKitRoofFrame RoofFrame;
	EKitRoofMode RoofMode = EKitRoofMode::Flat;
	TArray<FKitEdge> Edges;
	int32 FrontEdge = INDEX_NONE;

	/** Pitched roofs from the skeleton when the tile has one, over the bounding rectangle when the plan is one, else flat. */
	EKitRoofMode DecideRoofMode() const
	{
		const int32 Shape = Building.TypedRoofShape;
		const bool bPitchedShape = Shape == KitRoofGabled || Shape == KitRoofHipped || Shape == KitRoofHalfHipped || Shape == KitRoofMansard
			|| Shape == KitRoofGambrel || Shape == KitRoofPyramidal;
		if (Style.bFlatRoof || !bPitchedShape)
		{
			return EKitRoofMode::Flat;
		}
		if (!Building.RoofFaces.IsEmpty())
		{
			return EKitRoofMode::Skeleton;
		}
		const float Rise = (FMath::Min(RoofFrame.HalfAcross, RoofFrame.HalfAlong * (Shape == KitRoofGabled ? 100.f : 1.f)) + Style.EaveOverhang) * PitchSlope;
		const bool bFits = RoofFrame.Rectangularity > 0.8f && Building.Footprint.Rings.Num() == 1 && Rise < 7.5f && RoofFrame.HalfAcross > 1.5f;
		return bFits ? EKitRoofMode::Rectangle : EKitRoofMode::Flat;
	}

	/** Key of an instance list: the piece, and the material that replaces its wall, trim or roof slots (empty for none). */
	FName PieceKey(const TCHAR* Piece, const FString& Material) const
	{
		return Material.IsEmpty() ? FName(*FString::Printf(TEXT("%s_%s"), Style.Prefix, Piece))
			: FName(*FString::Printf(TEXT("%s_%s|%s"), Style.Prefix, Piece, *Material));
	}

	/** Material that the slots of this piece that show the wall take: the facade for walls, corners and cornices. */
	FString MaterialFor(const FString& Piece) const
	{
		if (Piece.StartsWith(TEXT("Wall_")) || Piece.StartsWith(TEXT("Corner_")) || Piece.StartsWith(TEXT("Cornice")))
		{
			return FacadeName;
		}
		if (Piece.StartsWith(TEXT("Plinth")))
		{
			return TEXT("Facade_Plinth");
		}
		return FString();
	}

	/** Adds one instance of a kit piece: location in metres (tile-relative), yaw, scale along the wall and in height. */
	void Place(const TCHAR* Piece, const FVector2f& Point, float ZMetres, float YawDegrees, float ScaleX, float ScaleZ = 1.f)
	{
		if (!Piece || !Piece[0])
		{
			return;
		}
		AddInstance(PieceKey(Piece, MaterialFor(Piece)), FTransform(FRotator(0.f, YawDegrees, 0.f), FVector(KitToCm(Point, ZMetres)), FVector(ScaleX, 1.f, ScaleZ)));
	}

	void AddInstance(const FName& Key, const FTransform& Transform)
	{
		Instances.Pieces.FindOrAdd(Key).Add(Transform);
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
		if (RoofMode == EKitRoofMode::Flat && !Style.bFlatRoof)
		{
			BuildParapet(Edge, StartOffset, BayWidth, Bays, Scale);
		}
		if (RoofMode != EKitRoofMode::Flat && ShouldHaveGutter(Edge))
		{
			PlaceGutter(Edge);
		}
	}

	/** A low wall above the cornice around a flat roof, from solid bays stretched down to the parapet height. */
	void BuildParapet(const FKitEdge& Edge, float StartOffset, float BayWidth, int32 Bays, float Scale)
	{
		const float ParapetScale = KitParapetHeight / Style.StoreyHeight;
		for (int32 Bay = 0; Bay < Bays; ++Bay)
		{
			Place(Style.WallSolid, Edge.Start + Edge.Tangent * (StartOffset + Bay * BayWidth), WallTopZ, Edge.YawDegrees, Scale, ParapetScale);
		}
		if (Edge.bPostAtStart)
		{
			Place(TEXT("Corner_L"), Edge.Start, WallTopZ, Edge.YawDegrees, 1.f, ParapetScale);
		}
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

	/** Plinth, string courses and top band along one facade run. */
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

	float EaveZ() const { return WallTopZ + Style.EaveLift; }

	void BuildRoof()
	{
		switch (RoofMode)
		{
		case EKitRoofMode::Skeleton:
			BuildSkeletonRoof();
			PlaceHipCaps();
			PlaceFrontDormers();
			PlaceChimneyOnTop();
			break;
		case EKitRoofMode::Rectangle:
			BuildRectangleRoof();
			PlaceFrontDormers();
			PlaceChimneyOnRidge();
			break;
		case EKitRoofMode::Flat:
			break;
		}
	}

	/** The roof planes the world compiler cut from the straight skeleton (osmimport/roofs.py), above the eave line. */
	void BuildSkeletonRoof()
	{
		const int32 TileSlot = FindSlot(RoofTileName);
		const int32 FlatSlot = FindSlot(KitFlatRoofName(Building));
		for (const FWorldRoofFace& Face : Building.RoofFaces)
		{
			const int32 Material = Face.Kind == 1 ? FlatSlot : TileSlot;
			AddRoofFaceMesh(Face, Material);
		}
	}

	void AddRoofFaceMesh(const FWorldRoofFace& Face, int32 Material)
	{
		for (const float Side : {1.f, -1.f})
		{
			TArray<int32> Vertices;
			for (int32 Index = 0; Index < Face.Positions.Num(); ++Index)
			{
				const FVector3f& Position = Face.Positions[Index];
				Vertices.Add(RoofBuilder.AddVertex(KitToCm(FVector2f(Position.X, Position.Y), EaveZ() + Position.Z), Face.UVs[Index]));
			}
			for (int32 Triangle = 0; Triangle + 2 < Face.Indices.Num(); Triangle += 3)
			{
				const FVector3f A = RoofBuilder.GetPosition(Vertices[Face.Indices[Triangle]]);
				const FVector3f B = RoofBuilder.GetPosition(Vertices[Face.Indices[Triangle + 1]]);
				const FVector3f C = RoofBuilder.GetPosition(Vertices[Face.Indices[Triangle + 2]]);
				FVector3f Normal = FVector3f::CrossProduct(B - A, C - A).GetSafeNormal();
				if (Normal.Z < 0.f)
				{
					Normal = -Normal;
				}
				RoofBuilder.AddTriangle(Vertices[Face.Indices[Triangle]], Vertices[Face.Indices[Triangle + 1]], Vertices[Face.Indices[Triangle + 2]],
					Material, Normal * Side);
			}
		}
	}

	/** Ridge cap pieces along the hips and ridges, tilted to follow each line. */
	void PlaceHipCaps()
	{
		for (const FWorldRoofCap& Cap : Building.RoofCaps)
		{
			const FVector3f Delta = Cap.End - Cap.Start;
			const float Length = Delta.Size();
			if (Length < 0.5f)
			{
				continue;
			}
			const FVector3f Direction = Delta / Length;
			const FRotator Rotation(FMath::RadiansToDegrees(FMath::Asin(Direction.Z)), FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.f);
			const int32 Pieces = FMath::Max(1, FMath::CeilToInt(Length / 2.f));
			const float Scale = Length / (2.f * Pieces);
			for (int32 Piece = 0; Piece < Pieces; ++Piece)
			{
				const FVector3f Point = Cap.Start + Direction * (Piece * 2.f * Scale);
				AddInstance(RidgeKey(), FTransform(Rotation, FVector(KitToCm(FVector2f(Point.X, Point.Y), EaveZ() + Point.Z)), FVector(Scale, 1.f, 1.f)));
			}
		}
	}

	/** The kit's ridge cap, in the roof tile material of the building (the farm style only has a thatch one). */
	FName RidgeKey() const { return FName(*FString::Printf(TEXT("brick_Roof_Tile_Ridge|%s"), *RoofTileName)); }

	/** A chimney near the highest point of the skeleton roof. */
	void PlaceChimneyOnTop()
	{
		if (KitHash01(Building.OsmId, 777) > 0.6f)
		{
			return;
		}
		FVector3f Top = FVector3f::ZeroVector;
		for (const FWorldRoofFace& Face : Building.RoofFaces)
		{
			if (Face.Kind != 0)
			{
				continue;
			}
			for (const FVector3f& Position : Face.Positions)
			{
				if (Position.Z > Top.Z)
				{
					Top = Position;
				}
			}
		}
		if (Top.Z < 1.f)
		{
			return;
		}
		const float Yaw = Building.RidgeYaw + 180.f;
		AddInstance(PieceKey(TEXT("Chimney"), FString()), FTransform(FRotator(0.f, Yaw, 0.f), FVector(KitToCm(FVector2f(Top.X, Top.Y), EaveZ() + Top.Z - 0.35f))));
	}

	// -- plain gable or hip roof over the bounding rectangle, for outlines without a skeleton roof

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
				RoofBuilder.AddTriangle(Indices[0], Indices[Index], Indices[Index + 1], Material, Normal * Side);
			}
		}
	}

	/** One gable triangle or trapezoid in the wall plane at the end of the ridge, facing outward. */
	void AddGableFace(const TArray<FVector3f>& Points, const FVector3f& Outward, int32 Material)
	{
		TArray<int32> Indices;
		for (const FVector3f& Point : Points)
		{
			Indices.Add(RoofBuilder.AddVertex(Point, FVector2f(FVector3f::DotProduct(Point, FVector3f(Outward.Y, -Outward.X, 0.f)) / KitMetresToCm,
				-Point.Z / KitMetresToCm)));
		}
		for (int32 Index = 1; Index + 1 < Indices.Num(); ++Index)
		{
			RoofBuilder.AddTriangle(Indices[0], Indices[Index], Indices[Index + 1], Material, Outward);
		}
	}

	bool IsGabled() const { return Building.TypedRoofShape == KitRoofGabled || Building.TypedRoofShape == KitRoofGambrel; }

	void BuildRectangleRoof()
	{
		const int32 TileSlot = FindSlot(RoofTileName);
		const float Overhang = Style.EaveOverhang;
		const bool bGable = IsGabled();
		const bool bPyramid = Building.TypedRoofShape == KitRoofPyramidal;
		const float HalfAlong = RoofFrame.HalfAlong;
		const float HalfAcross = RoofFrame.HalfAcross;
		const float AcrossOuter = HalfAcross + Overhang;
		const float AlongOuter = HalfAlong + (bGable ? 0.25f : Overhang);
		float RidgeHalf = bGable ? AlongOuter : HalfAlong - HalfAcross;
		const bool bApex = bPyramid || RidgeHalf < 0.3f;
		RidgeHalf = bApex ? 0.f : RidgeHalf;
		const float Z = EaveZ();
		const float RidgeZ = Z + (bApex ? FMath::Min(AcrossOuter, AlongOuter) : AcrossOuter) * PitchSlope;
		const auto P = [&](float Along, float Across, float Height) { return KitToCm(RoofFrame.Point(Along, Across), Height); };
		AddRoofFace({P(AlongOuter, -AcrossOuter, Z), P(-AlongOuter, -AcrossOuter, Z), P(-RidgeHalf, 0.f, RidgeZ), P(RidgeHalf, 0.f, RidgeZ)}, TileSlot);
		AddRoofFace({P(-AlongOuter, AcrossOuter, Z), P(AlongOuter, AcrossOuter, Z), P(RidgeHalf, 0.f, RidgeZ), P(-RidgeHalf, 0.f, RidgeZ)}, TileSlot);
		if (bGable)
		{
			AddGableEnd(HalfAlong, 1.f);
			AddGableEnd(-HalfAlong, -1.f);
			PlaceRidge(RidgeHalf * 2.f, RidgeZ);
			return;
		}
		AddRoofFace({P(AlongOuter, AcrossOuter, Z), P(AlongOuter, -AcrossOuter, Z), P(RidgeHalf, 0.f, RidgeZ)}, TileSlot);
		AddRoofFace({P(-AlongOuter, -AcrossOuter, Z), P(-AlongOuter, AcrossOuter, Z), P(-RidgeHalf, 0.f, RidgeZ)}, TileSlot);
		if (!bApex)
		{
			PlaceRidge(RidgeHalf * 2.f, RidgeZ);
		}
		AddRectangleHipCaps(RidgeHalf, AlongOuter, AcrossOuter, Z, RidgeZ);
	}

	/** Hip caps from the four eave corners of a hipped rectangle up to the ridge ends. */
	void AddRectangleHipCaps(float RidgeHalf, float AlongOuter, float AcrossOuter, float Z, float RidgeZ)
	{
		for (const float AlongSign : {1.f, -1.f})
		{
			for (const float AcrossSign : {1.f, -1.f})
			{
				const FVector2f From = RoofFrame.Point(AlongOuter * AlongSign, AcrossOuter * AcrossSign);
				const FVector2f To = RoofFrame.Point(RidgeHalf * AlongSign, 0.f);
				FWorldRoofCap Cap;
				Cap.Start = FVector3f(From.X, From.Y, 0.f);
				Cap.End = FVector3f(To.X, To.Y, RidgeZ - Z);
				PlaceCap(Cap);
			}
		}
	}

	void PlaceCap(const FWorldRoofCap& Cap)
	{
		const FVector3f Delta = Cap.End - Cap.Start;
		const float Length = Delta.Size();
		if (Length < 0.5f)
		{
			return;
		}
		const FVector3f Direction = Delta / Length;
		const FRotator Rotation(FMath::RadiansToDegrees(FMath::Asin(Direction.Z)), FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.f);
		const int32 Pieces = FMath::Max(1, FMath::CeilToInt(Length / 2.f));
		const float Scale = Length / (2.f * Pieces);
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			const FVector3f Point = Cap.Start + Direction * (Piece * 2.f * Scale);
			AddInstance(RidgeKey(), FTransform(Rotation, FVector(KitToCm(FVector2f(Point.X, Point.Y), EaveZ() + Point.Z)), FVector(Scale, 1.f, 1.f)));
		}
	}

	/** The wall above the eave line at one end of a gabled roof, up to where the roof planes meet it. */
	void AddGableEnd(float AlongPosition, float Direction)
	{
		const float HalfAcross = RoofFrame.HalfAcross;
		const float PlaneZAtWall = EaveZ() + Style.EaveOverhang * PitchSlope;
		const float TopZ = EaveZ() + (HalfAcross + Style.EaveOverhang) * PitchSlope;
		const auto P = [&](float Across, float Height) { return KitToCm(RoofFrame.Point(AlongPosition, Across), Height); };
		const FVector2f OutwardAxis = RoofFrame.AlongAxis * Direction;
		AddGableFace({P(-HalfAcross, WallTopZ), P(HalfAcross, WallTopZ), P(HalfAcross, PlaneZAtWall), P(0.f, TopZ), P(-HalfAcross, PlaneZAtWall)},
			FVector3f(OutwardAxis.X, OutwardAxis.Y, 0.f), FindSlot(FacadeName));
	}

	void PlaceRidge(float RidgeLength, float RidgeZ)
	{
		FWorldRoofCap Cap;
		const FVector2f A = RoofFrame.Point(RidgeLength * 0.5f, 0.f);
		const FVector2f B = RoofFrame.Point(-RidgeLength * 0.5f, 0.f);
		Cap.Start = FVector3f(A.X, A.Y, RidgeZ - EaveZ());
		Cap.End = FVector3f(B.X, B.Y, RidgeZ - EaveZ());
		PlaceCap(Cap);
	}

	void PlaceChimneyOnRidge()
	{
		if (KitHash01(Building.OsmId, 777) > 0.6f || RoofFrame.HalfAlong < 3.f)
		{
			return;
		}
		const float RidgeZ = EaveZ() + (RoofFrame.HalfAcross + Style.EaveOverhang) * PitchSlope;
		const float Along = RoofFrame.HalfAlong - 1.8f - 2.f * KitHash01(Building.OsmId, 778);
		AddInstance(PieceKey(TEXT("Chimney"), FString()), FTransform(FRotator(0.f, Building.RidgeYaw + 180.f, 0.f),
			FVector(KitToCm(RoofFrame.Point(Along, 0.f) - RoofFrame.AcrossAxis * 0.4f, RidgeZ - 0.35f))));
	}

	/** Gutters on the eave sides: every wall of a hipped roof, only the long walls under a gable roof. */
	bool ShouldHaveGutter(const FKitEdge& Edge) const
	{
		if (Style.EaveOverhang <= 0.f || FCString::Strcmp(Style.Prefix, TEXT("farm")) == 0)
		{
			return false;
		}
		if (RoofMode == EKitRoofMode::Skeleton && !IsGabled())
		{
			return true;
		}
		return FMath::Abs(FVector2f::DotProduct(Edge.Normal, RoofFrame.AcrossAxis)) > 0.7f;
	}

	void PlaceGutter(const FKitEdge& Edge)
	{
		const int32 Pieces = FMath::Max(1, FMath::RoundToInt(Edge.Length / 2.f));
		const float Scale = Edge.Length / (2.f * Pieces);
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			Place(TEXT("Gutter_M"), Edge.Start + Edge.Tangent * (Piece * 2.f * Scale), EaveZ(), Edge.YawDegrees, Scale);
		}
	}

	/** Dormers on the street side wall's roof slope when the attic is lived in. */
	void PlaceFrontDormers()
	{
		const bool bAttic = (Building.TypeFlags & 1) != 0;
		if (!bAttic || !Style.Dormer || !Style.Dormer[0] || FrontEdge == INDEX_NONE || Building.TypedRoofShape == KitRoofMansard)
		{
			return;
		}
		const FKitEdge& Edge = Edges[FrontEdge];
		const float Margin = RoofMode == EKitRoofMode::Skeleton ? 3.4f : 1.6f;
		const float Usable = Edge.Length - 2.f * Margin;
		if (Usable < Style.DormerWidth)
		{
			return;
		}
		const int32 Count = FMath::Clamp(FMath::FloorToInt(Usable / 3.6f) + 1, 1, 4);
		const float Slot = Usable / Count;
		const float SurfaceInset = 2.0f;
		const float Z = EaveZ() + (SurfaceInset + Style.EaveOverhang) * PitchSlope - 0.03f;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Along = Margin + Slot * (Index + 0.5f) - Style.DormerWidth * 0.5f;
			Place(Style.Dormer, Edge.Start + Edge.Tangent * Along - Edge.Normal * SurfaceInset, Z, Edge.YawDegrees, 1.f);
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

FKitFlatRoof BuildKitBuilding(const FWorldTileData& Tile, const FWorldBuilding& Building, const TFunction<int32(const FString&)>& FindSlot,
	FWorldKitInstances& Instances, FWorldMeshBuilder& RoofBuilder)
{
	const FKitStyle* Style = KitStyleForClass(Building.ClassId);
	FKitAssembler Assembler(Building, *Style, FindSlot, Instances, RoofBuilder);
	FKitFlatRoof Flat;
	Assembler.Build(Flat);
	++Instances.NumBuildings;
	(void)Tile;
	return Flat;
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

/** Material instance of the world's facade folder by name (Facade_ClinkerDeepRed, Roof_Slate ...), cached. */
UMaterialInterface* FindFacadeMaterialCached(const FString& Name)
{
	static TMap<FString, TStrongObjectPtr<UMaterialInterface>> Cache;
	if (const TStrongObjectPtr<UMaterialInterface>* Found = Cache.Find(Name))
	{
		return Found->Get();
	}
	const FString Path = FString::Printf(TEXT("/Game/World/Facades/M_%s.M_%s"), *Name, *Name);
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn);
	if (!Material)
	{
		UE_LOG(LogTemp, Warning, TEXT("Facade material %s missing; run Scripts/create_facade_materials.py"), *Path);
	}
	Cache.Add(Name, TStrongObjectPtr<UMaterialInterface>(Material));
	return Material;
}

/** Which slots of a kit mesh the replacement material takes: wall slots, the trim of a plinth, or the roof tile slot. */
void KitSlotOverrides(const FString& Piece, const FString& Material, TMap<FName, UMaterialInterface*>& Out)
{
	UMaterialInterface* Replacement = FindFacadeMaterialCached(Material);
	if (!Replacement)
	{
		return;
	}
	if (Material.StartsWith(TEXT("Roof_")))
	{
		Out.Add(TEXT("RoofTile"), Replacement);
		return;
	}
	for (const TCHAR* Slot : {TEXT("Brick"), TEXT("Plaster"), TEXT("Concrete")})
	{
		Out.Add(Slot, Replacement);
	}
	if (Material == TEXT("Facade_Plinth"))
	{
		Out.Add(TEXT("Sill"), Replacement);
	}
	if (Piece.StartsWith(TEXT("farm_")))
	{
		Out.Add(TEXT("Timber"), FindFacadeMaterialCached(TEXT("Facade_TimberBeam")));
	}
}
}

bool AddKitInstancesStep(AWorldTileActor& Actor, const FWorldKitInstances& Kit, int32 Step)
{
	TArray<FName> Keys;
	Kit.Pieces.GetKeys(Keys);
	Keys.Sort(FNameLexicalLess());
	const int32 First = Step * KitPiecesPerStepCount;
	const int32 Last = FMath::Min(First + KitPiecesPerStepCount, Keys.Num());
	for (int32 Index = First; Index < Last; ++Index)
	{
		FString Piece, Material;
		if (!Keys[Index].ToString().Split(TEXT("|"), &Piece, &Material))
		{
			Piece = Keys[Index].ToString();
		}
		TMap<FName, UMaterialInterface*> Overrides;
		if (!Material.IsEmpty())
		{
			KitSlotOverrides(Piece, Material, Overrides);
		}
		Actor.AddKitInstances(FindKitMeshCached(FName(*Piece)), Kit.Pieces[Keys[Index]], PieceCastsShadow(Keys[Index]), Overrides);
	}
	return Last >= Keys.Num();
}
