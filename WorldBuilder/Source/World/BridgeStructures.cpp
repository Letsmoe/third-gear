#include "BridgeStructures.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "PlacementFrame.h"
#include "Polyline.h"

namespace WorldBuilder
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double DegreesPerRadian = 180.0 / Pi;

/** bridges.py: surfacing on the deck, kerb height, cap width, railing foot and sizes of the repeated pieces, metres. */
constexpr double AsphaltThickness = 0.08;
constexpr double KerbHeight = 0.15;
constexpr double DefaultEdgeBeamWidth = 0.75;
constexpr double RailingInset = 0.12;
constexpr double GuardRailOffset = 0.4;
constexpr double DefaultSegmentLength = 2.0;
constexpr double FootRailBay = 1.5;
constexpr double FootGirderDepth = 0.35;
/** The footbridge deck sits this far above the ground at its ends, as the path surfaces do. */
constexpr double FootDeckLift = 0.04;
constexpr double DefaultFootWidth = 2.0;
constexpr double DefaultCycleWidth = 3.0;
/** Wall thickness of an abutment along the road, and the length of its wing walls, metres. */
constexpr double AbutmentThickness = 1.0;
constexpr double WingLength = 3.0;
/** Bridges longer than this and higher than PierMinimumClearance get piers every PierSpacing metres. */
constexpr double PierMinimumLength = 28.0;
constexpr double PierSpacing = 20.0;
constexpr double PierMinimumClearance = 3.0;
constexpr double PierCapHeight = 0.6;
constexpr double PierCapDepth = 1.2;
/** Foundations reach this far below the lowest ground, metres. */
constexpr double FoundationDepth = 0.5;
/** Timber piles stand every this many metres. */
constexpr double PileSpacing = 4.0;
/** Pieces are rolled by the deck's slope with this sign (Unreal's roll tips local +Y down for a positive angle). */
constexpr double SlopeRollSign = 1.0;

/** A deck's centre line, width and the ramp between its end heights. */
struct FDeck
{
	FPolyline Points;
	double Width = 0.0;
	double StartHeight = 0.0;
	double EndHeight = 0.0;

	/** The deck surface height at a point, from its projection on the line between the ends. */
	double HeightAt(const FWorldPoint& Point) const
	{
		const FWorldPoint& Start = Points.front();
		const FWorldPoint& End = Points.back();
		const double AlongX = End.X - Start.X;
		const double AlongY = End.Y - Start.Y;
		const double LengthSquared = AlongX * AlongX + AlongY * AlongY;
		if (LengthSquared < 1e-9)
		{
			return StartHeight;
		}
		const double Fraction = std::clamp(((Point.X - Start.X) * AlongX + (Point.Y - Start.Y) * AlongY) / LengthSquared, 0.0, 1.0);
		return StartHeight + Fraction * (EndHeight - StartHeight);
	}
};

/** The length of a deck's centre line. */
double DeckLength(const FDeck& Deck)
{
	double Length = 0.0;
	for (size_t Index = 0; Index + 1 < Deck.Points.size(); ++Index)
	{
		Length += std::hypot(Deck.Points[Index + 1].X - Deck.Points[Index].X, Deck.Points[Index + 1].Y - Deck.Points[Index].Y);
	}
	return Length;
}

/** A point on a deck's centre line and the direction of the leg it is on. */
struct FStretchPoint
{
	FWorldPoint Position;
	FWorldPoint Direction;
};

/** The point at a distance along the deck's centre line, or nothing past its end. */
std::optional<FStretchPoint> PointAlong(const FDeck& Deck, double Distance)
{
	double Walked = 0.0;
	for (size_t Index = 0; Index + 1 < Deck.Points.size(); ++Index)
	{
		const FWorldPoint& From = Deck.Points[Index];
		const FWorldPoint& To = Deck.Points[Index + 1];
		const double Leg = std::hypot(To.X - From.X, To.Y - From.Y);
		if (Leg > 1e-6 && Distance <= Walked + Leg)
		{
			const FWorldPoint Direction = {(To.X - From.X) / Leg, (To.Y - From.Y) / Leg};
			const double Along = Distance - Walked;
			return FStretchPoint{{From.X + Direction.X * Along, From.Y + Direction.Y * Along}, Direction};
		}
		Walked += Leg;
	}
	return std::nullopt;
}

/** One stretch of a deck's leg: a frame at its start, its stretch factor and the deck slope along it. */
struct FStretch
{
	FWorldPoint Start;
	FWorldPoint Direction;
	/** Length of the stretch divided by the nominal segment length. */
	double Stretch = 1.0;
	double StartHeight = 0.0;
	double SlopeDegrees = 0.0;
};

/** Hash-free classification of what a road bridge carries. */
struct FRoadClass
{
	bool bInfillRailing = true;
	bool bGuardRail = false;
};

/** Everything one bridge's placement needs, and the output list. */
class FBridgeLayer
{
public:
	FBridgeLayer(const FGroundSampler& InGround, const FKitSpecs& InSpecs, std::vector<FSitePlacement>& InOut)
		: Ground(InGround), SegmentLength(InSpecs.Number("bridges.segment_length", DefaultSegmentLength)),
		  EdgeBeamWidth(InSpecs.Number("bridges.edge_beam_width", DefaultEdgeBeamWidth)), Out(InOut)
	{
	}

	/** The road bridge's slab, edges, abutments and piers. */
	void LayRoadBridge(const FDeck& Deck, const FRoadClass& Class);
	/** The footbridge's deck, railings and supports; bTimber picks the Vierlande timber type. */
	void LayFootBridge(const FDeck& Deck, bool bTimber);

private:
	const FGroundSampler& Ground;
	double SegmentLength;
	double EdgeBeamWidth;
	std::vector<FSitePlacement>& Out;

	/** The deck cut into stretches of about the given length along its legs. */
	std::vector<FStretch> Stretches(const FDeck& Deck, double NominalLength) const;
	/**
	 * Adds a piece on a stretch at lateral offset U. bReversed turns it around (for the left edge), which puts its
	 * origin at the far end. Scale is (x, y, z); a Y scale of 0 means the stretch factor.
	 */
	void AddOnStretch(const FStretch& Stretch, const char* Piece, double U, bool bReversed, double ScaleX,
					  double ScaleZ, double DropBelowDeck, double NominalLength);
	/** The lowest ground over the span, ends excluded. */
	double LowestGround(const FDeck& Deck) const;
	/** Walls under both deck ends with wing walls behind them, down to WallBottom. */
	void AddAbutments(const FDeck& Deck, double SlabHalfWidth, double WallBottom);
	/** Column piers with a cap every PierSpacing metres; DeckBottomOffset is the slab's underside below the surface. */
	void AddPiers(const FDeck& Deck, double DeckBottomOffset);
	void AddBlock(const FPlacementFrame& Frame, double U, double V, float Z, double ScaleX, double ScaleY, double ScaleZ);
	void AddPiece(const FPlacementFrame& Frame, const char* Piece, double U, double V, float Z, double Rotation, double ScaleX,
				  double ScaleY, double ScaleZ, double RollDegrees);
};

std::vector<FStretch> FBridgeLayer::Stretches(const FDeck& Deck, double NominalLength) const
{
	std::vector<FStretch> Result;
	for (size_t Index = 0; Index + 1 < Deck.Points.size(); ++Index)
	{
		const FWorldPoint& From = Deck.Points[Index];
		const FWorldPoint& To = Deck.Points[Index + 1];
		const double Length = std::hypot(To.X - From.X, To.Y - From.Y);
		if (Length < 0.05)
		{
			continue;
		}
		const FWorldPoint Direction = {(To.X - From.X) / Length, (To.Y - From.Y) / Length};
		const int Count = std::max(1, static_cast<int>(std::lround(Length / NominalLength)));
		const double Step = Length / Count;
		for (int Piece = 0; Piece < Count; ++Piece)
		{
			FStretch Stretch;
			Stretch.Start = {From.X + Direction.X * Step * Piece, From.Y + Direction.Y * Step * Piece};
			const FWorldPoint End = {Stretch.Start.X + Direction.X * Step, Stretch.Start.Y + Direction.Y * Step};
			Stretch.Direction = Direction;
			Stretch.Stretch = Step / NominalLength;
			Stretch.StartHeight = Deck.HeightAt(Stretch.Start);
			Stretch.SlopeDegrees = std::atan2(Deck.HeightAt(End) - Stretch.StartHeight, Step) * DegreesPerRadian;
			Result.push_back(Stretch);
		}
	}
	return Result;
}

void FBridgeLayer::AddPiece(const FPlacementFrame& Frame, const char* Piece, double U, double V, float Z, double Rotation,
							double ScaleX, double ScaleY, double ScaleZ, double RollDegrees)
{
	FStructurePlacement Placement = Frame.Piece(std::string("bridges_") + Piece, U, V, Z, Rotation);
	Placement.ScaleX = static_cast<float>(ScaleX);
	Placement.ScaleY = static_cast<float>(ScaleY);
	Placement.ScaleZ = static_cast<float>(ScaleZ);
	Placement.Roll = static_cast<float>(RollDegrees);
	Out.push_back({Placement, false});
}

void FBridgeLayer::AddBlock(const FPlacementFrame& Frame, double U, double V, float Z, double ScaleX, double ScaleY,
							double ScaleZ)
{
	AddPiece(Frame, "Block", U, V, Z, 0.0, ScaleX, ScaleY, ScaleZ, 0.0);
}

void FBridgeLayer::AddOnStretch(const FStretch& Stretch, const char* Piece, double U, bool bReversed,
								double ScaleX, double ScaleZ, double DropBelowDeck, double NominalLength)
{
	const FPlacementFrame Frame(Stretch.Start.X, Stretch.Start.Y, Stretch.Direction.X, Stretch.Direction.Y);
	const double V = bReversed ? NominalLength * Stretch.Stretch : 0.0;
	const double Roll = SlopeRollSign * Stretch.SlopeDegrees * (bReversed ? -1.0 : 1.0);
	const double Height = bReversed ? Stretch.StartHeight + std::tan(Stretch.SlopeDegrees / DegreesPerRadian) * V
									: Stretch.StartHeight;
	AddPiece(Frame, Piece, U, V, static_cast<float>(Height - DropBelowDeck), bReversed ? 180.0 : 0.0, ScaleX,
			 Stretch.Stretch, ScaleZ, Roll);
}

double FBridgeLayer::LowestGround(const FDeck& Deck) const
{
	double Lowest = 1e9;
	for (const FStretch& Stretch : Stretches(Deck, SegmentLength))
	{
		const FWorldPoint Across = {-Stretch.Direction.Y, Stretch.Direction.X};
		for (const double U : {-Deck.Width / 2.0, 0.0, Deck.Width / 2.0})
		{
			Lowest = std::min(Lowest, Ground(Stretch.Start.X + Across.X * U, Stretch.Start.Y + Across.Y * U));
		}
	}
	return Lowest;
}

void FBridgeLayer::AddAbutments(const FDeck& Deck, double SlabHalfWidth, double WallBottom)
{
	const FWorldPoint& First = Deck.Points[0];
	const FWorldPoint& Last = Deck.Points.back();
	struct FEnd
	{
		FWorldPoint Point;
		FWorldPoint Inward;
		double Height;
	};
	const auto Unit = [](const FWorldPoint& From, const FWorldPoint& To) {
		const double Length = std::max(1e-6, std::hypot(To.X - From.X, To.Y - From.Y));
		return FWorldPoint{(To.X - From.X) / Length, (To.Y - From.Y) / Length};
	};
	const FEnd Ends[] = {{First, Unit(First, Deck.Points[1]), Deck.StartHeight},
						 {Last, Unit(Last, Deck.Points[Deck.Points.size() - 2]), Deck.EndHeight}};
	for (const FEnd& End : Ends)
	{
		const double SlabTop = End.Height - AsphaltThickness - 0.01;
		// The wall stands under the deck's end, the wings run back out of the deck into the embankment.
		const FPlacementFrame Inward(End.Point.X, End.Point.Y, End.Inward.X, End.Inward.Y);
		AddBlock(Inward, 0.0, 0.0, static_cast<float>(SlabTop), SlabHalfWidth * 2.0 - 0.2, AbutmentThickness,
				 SlabTop - WallBottom);
		const FPlacementFrame Outward(End.Point.X, End.Point.Y, -End.Inward.X, -End.Inward.Y);
		for (const double Side : {-1.0, 1.0})
		{
			AddPiece(Outward, "Wing_Wall", Side * (SlabHalfWidth - 0.4), 0.0, static_cast<float>(SlabTop), 0.0, 1.0, WingLength,
					 SlabTop - WallBottom, 0.0);
		}
	}
}

void FBridgeLayer::AddPiers(const FDeck& Deck, double DeckBottomOffset)
{
	const double Length = DeckLength(Deck);
	const int Count = static_cast<int>(Length / PierSpacing);
	for (int Pier = 1; Pier <= Count; ++Pier)
	{
		const std::optional<FStretchPoint> Spot = PointAlong(Deck, Length * Pier / (Count + 1));
		if (!Spot.has_value())
		{
			continue;
		}
		const FPlacementFrame Frame(Spot->Position.X, Spot->Position.Y, Spot->Direction.X, Spot->Direction.Y);
		// The cap carries the girder, so its top is the deck's underside here.
		const double CapTop = Deck.HeightAt(Spot->Position) - DeckBottomOffset;
		const double Foot = Ground(Spot->Position.X, Spot->Position.Y) - FoundationDepth;
		if (CapTop - PierCapHeight - Foot < 1.0)
		{
			continue;
		}
		for (const double Side : {-1.0, 1.0})
		{
			AddPiece(Frame, "Pier_Column", Side * Deck.Width * 0.25, 0.0, static_cast<float>(CapTop - PierCapHeight), 0.0, 1.0,
					 1.0, CapTop - PierCapHeight - Foot, 0.0);
		}
		AddBlock(Frame, 0.0, -PierCapDepth / 2.0, static_cast<float>(CapTop), Deck.Width * 0.6, PierCapDepth, PierCapHeight);
	}
}

void FBridgeLayer::LayRoadBridge(const FDeck& Deck, const FRoadClass& Class)
{
	if (Deck.Points.size() < 2)
	{
		return;
	}
	const double Length = DeckLength(Deck);
	const double Lowest = LowestGround(Deck);
	const double MiddleHeight = (Deck.StartHeight + Deck.EndHeight) / 2.0;
	const double Clearance = MiddleHeight - Lowest;
	const bool bPiers = Length > PierMinimumLength && Clearance > PierMinimumClearance;
	const double SlabDepth = bPiers ? std::max(0.8, PierSpacing / 20.0) : std::max(0.45, Length / 18.0);
	const double SlabWidth = Deck.Width + 2.0 * EdgeBeamWidth;
	const double DeckBottomOffset = AsphaltThickness + SlabDepth;
	const double RailX = Deck.Width / 2.0 + EdgeBeamWidth - RailingInset;
	const char* Rail = Class.bInfillRailing ? "Rail_Infill" : "Rail_Posts";
	for (const FStretch& Stretch : Stretches(Deck, SegmentLength))
	{
		AddOnStretch(Stretch, "Block", 0.0, false, SlabWidth, SlabDepth, AsphaltThickness, 1.0);
		for (const double Side : {-1.0, 1.0})
		{
			const bool bLeft = Side < 0.0;
			AddOnStretch(Stretch, "Edge_Beam", Side * Deck.Width / 2.0, bLeft, 1.0, 1.0, 0.0, SegmentLength);
			AddOnStretch(Stretch, Rail, Side * RailX, bLeft, 1.0, 1.0, 0.0, SegmentLength);
			if (Class.bGuardRail)
			{
				AddOnStretch(Stretch, "Guard_Rail", Side * (Deck.Width / 2.0 + GuardRailOffset), bLeft, 1.0, 1.0, 0.0,
							 SegmentLength);
			}
		}
	}
	const double WallBottom = std::min(Lowest, std::min(Deck.StartHeight, Deck.EndHeight) - 1.0) - FoundationDepth;
	AddAbutments(Deck, SlabWidth / 2.0, WallBottom);
	if (bPiers)
	{
		AddPiers(Deck, DeckBottomOffset);
	}
}

void FBridgeLayer::LayFootBridge(const FDeck& Deck, bool bTimber)
{
	if (Deck.Points.size() < 2)
	{
		return;
	}
	const double Length = DeckLength(Deck);
	const double GirderScale = std::max(1.0, Length / 22.0 / FootGirderDepth);
	const double RailX = Deck.Width / 2.0 + (bTimber ? 0.08 : 0.03);
	for (const FStretch& Stretch : Stretches(Deck, SegmentLength))
	{
		AddOnStretch(Stretch, "Foot_Planks", 0.0, false, Deck.Width, 1.0, 0.0, SegmentLength);
		if (bTimber)
		{
			for (const double Fraction : {-0.35, 0.0, 0.35})
			{
				AddOnStretch(Stretch, "Foot_Stringer", Fraction * Deck.Width, false, 1.0, 1.0, 0.0, SegmentLength);
			}
			continue;
		}
		for (const double Side : {-1.0, 1.0})
		{
			AddOnStretch(Stretch, "Foot_Girder", Side * Deck.Width * 0.3, false, 1.0, GirderScale, 0.0, SegmentLength);
		}
	}
	const char* Rail = bTimber ? "Foot_Rail_Timber" : "Foot_Rail";
	const double Bay = bTimber ? SegmentLength : FootRailBay;
	for (const FStretch& Stretch : Stretches(Deck, Bay))
	{
		for (const double Side : {-1.0, 1.0})
		{
			AddOnStretch(Stretch, Rail, Side * RailX, Side < 0.0, 1.0, 1.0, 0.0, Bay);
		}
	}
	// The ends rest on bank seats (steel) or the piles carry the deck (timber).
	const FWorldPoint& First = Deck.Points[0];
	const FWorldPoint& Last = Deck.Points.back();
	const double SeatTop = -0.05 - FootGirderDepth;
	for (const FStretch& Stretch : Stretches(Deck, PileSpacing))
	{
		if (!bTimber)
		{
			break;
		}
		const FPlacementFrame Frame(Stretch.Start.X, Stretch.Start.Y, Stretch.Direction.X, Stretch.Direction.Y);
		const double Foot = Ground(Stretch.Start.X, Stretch.Start.Y) - 0.8;
		const double Top = Stretch.StartHeight - 0.17;
		if (Top - Foot < 0.5)
		{
			continue;
		}
		for (const double Side : {-1.0, 1.0})
		{
			AddPiece(Frame, "Foot_Pile", Side * (Deck.Width / 2.0 - 0.15), 0.0, static_cast<float>(Top), 0.0, 1.0, 1.0,
					 Top - Foot, 0.0);
		}
	}
	if (bTimber)
	{
		return;
	}
	const FWorldPoint Ends[] = {First, Last};
	const FWorldPoint Inwards[] = {{Deck.Points[1].X - First.X, Deck.Points[1].Y - First.Y},
								   {Deck.Points[Deck.Points.size() - 2].X - Last.X, Deck.Points[Deck.Points.size() - 2].Y - Last.Y}};
	const double Heights[] = {Deck.StartHeight, Deck.EndHeight};
	const double Foot = LowestGround(Deck) - FoundationDepth;
	for (int End = 0; End < 2; ++End)
	{
		const FPlacementFrame Frame(Ends[End].X, Ends[End].Y, Inwards[End].X, Inwards[End].Y);
		const double Top = Heights[End] + SeatTop;
		AddBlock(Frame, 0.0, 0.0, static_cast<float>(Top), Deck.Width + 0.6, 1.2, std::max(0.4, Top - Foot));
	}
}

/** The road class's railing and guard rail: infill railings in town, posts in the country, guard rails on main roads. */
FRoadClass ClassOf(const FStreetWay& Way, bool bUrban)
{
	FRoadClass Class;
	Class.bInfillRailing = bUrban;
	const std::string* Highway = FindTag(Way.Tags, "highway");
	if (Highway == nullptr)
	{
		return Class;
	}
	Class.bGuardRail = *Highway == "motorway" || *Highway == "trunk" || *Highway == "primary" || *Highway == "secondary"
		|| *Highway == "motorway_link" || *Highway == "trunk_link" || *Highway == "primary_link" || *Highway == "secondary_link";
	return Class;
}

/** The width of a footbridge deck from the way's width tag or its kind. */
double FootWidthOf(const FTags& Tags)
{
	const std::string* Width = FindTag(Tags, "width");
	const double Tagged = Width == nullptr ? 0.0 : std::atof(Width->c_str());
	if (Tagged >= 1.0)
	{
		return std::min(Tagged, 6.0);
	}
	const std::string* Highway = FindTag(Tags, "highway");
	return Highway != nullptr && *Highway == "cycleway" ? DefaultCycleWidth : DefaultFootWidth;
}

bool IsTimberFootBridge(const FTags& Tags)
{
	const std::string* Highway = FindTag(Tags, "highway");
	const std::string* Bridge = FindTag(Tags, "bridge");
	return (Highway != nullptr && *Highway == "path") || (Bridge != nullptr && *Bridge == "boardwalk");
}
}

bool IsFootBridge(const FTags& Tags)
{
	const std::string* Bridge = FindTag(Tags, "bridge");
	const std::string* Highway = FindTag(Tags, "highway");
	if (Bridge == nullptr || *Bridge == "no" || Highway == nullptr)
	{
		return false;
	}
	return *Highway == "footway" || *Highway == "path" || *Highway == "cycleway" || *Highway == "pedestrian";
}

std::vector<FSitePlacement> BuildBridgeStructures(const FOsmData& Osm, const FStreetModel& Model, const FRoadSurfaces& Roads,
												  const FKitSpecs& Specs, const FGroundSampler& Ground, int* RoadBridgeCount,
												  int* FootBridgeCount)
{
	std::vector<FSitePlacement> Placements;
	FBridgeLayer Layer(Ground, Specs, Placements);
	int RoadBridges = 0;
	for (const FBridgeDeck& Bridge : Roads.AllBridges())
	{
		if (Bridge.Way == nullptr)
		{
			continue;
		}
		FDeck Deck;
		for (const FStreetPoint& Point : Bridge.Way->Points)
		{
			Deck.Points.push_back({Point.X, Point.Y});
		}
		Deck.Width = Model.Widths.at(Bridge.Way->Id);
		Deck.StartHeight = Bridge.StartHeight;
		Deck.EndHeight = Bridge.EndHeight;
		const auto Urban = Model.Urban.find(Bridge.Way->Id);
		Layer.LayRoadBridge(Deck, ClassOf(*Bridge.Way, Urban != Model.Urban.end() && Urban->second));
		++RoadBridges;
	}
	int FootBridges = 0;
	for (const FOsmWay& Way : Osm.Footways)
	{
		if (!IsFootBridge(Way.Tags) || Way.Points.size() < 2)
		{
			continue;
		}
		FDeck Deck;
		Deck.Points = Way.Points;
		Deck.Width = FootWidthOf(Way.Tags);
		Deck.StartHeight = Ground(Way.Points.front().X, Way.Points.front().Y) + FootDeckLift;
		Deck.EndHeight = Ground(Way.Points.back().X, Way.Points.back().Y) + FootDeckLift;
		Layer.LayFootBridge(Deck, IsTimberFootBridge(Way.Tags));
		++FootBridges;
	}
	if (RoadBridgeCount != nullptr)
	{
		*RoadBridgeCount = RoadBridges;
	}
	if (FootBridgeCount != nullptr)
	{
		*FootBridgeCount = FootBridges;
	}
	return Placements;
}
}
