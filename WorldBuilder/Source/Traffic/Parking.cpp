#include "Parking.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

#include "NumpyRandom.h"
#include "OsmTags.h"
#include "SpatialIndex.h"

namespace WorldBuilder
{
namespace
{
// Half the width of a moving car plus a hand's breadth.
constexpr double JunctionGap = 6.5;
constexpr double SignalGap = 10.5;
constexpr double CrossingGap = 8.5;
constexpr double BusStopGap = 15.0;
constexpr double MiniRoundaboutGap = 9.0;
constexpr double WayEndGap = 1.5;
constexpr double EmptyStretchChance = 0.07;
constexpr double OnKerbChance = 0.10;
constexpr double OverlapGap = 0.25;
constexpr int DiscSegments = 64;

const std::set<std::string> ParallelValues = {"parallel", "lane", "street_side", "marked", "yes", "half_on_kerb", "on_kerb"};
const std::set<std::string> AngledValues = {"diagonal", "perpendicular"};
const std::set<std::string> NoneValues = {"no", "separate", "no_parking", "no_stopping", "no_standing", "none"};
const std::set<std::string> NoParkingClasses = {"motorway", "motorway_link", "trunk", "trunk_link"};
const std::set<std::string> DefaultParkingClasses = {"residential", "living_street"};

/** A random generator that gives the same numbers for the same way on every run. */
FNumpyRandom StableRandom(int64_t WayId, int Salt)
{
	const uint64_t Mixed = static_cast<uint64_t>(WayId) * 2654435761ULL + static_cast<uint64_t>(static_cast<int64_t>(Salt));
	return FNumpyRandom(static_cast<uint32_t>(Mixed & 0xFFFFFFFFULL));
}

bool HasParkingTags(const FTags& Tags)
{
	for (const auto& [Key, Value] : Tags)
	{
		if (Key.rfind("parking:", 0) == 0 && Key.rfind("parking:condition", 0) != 0)
		{
			return true;
		}
	}
	return OsmTags::HasTag(Tags, "parking:lane");
}

/** (value, orientation) tagged for one side, or nothing. */
std::optional<std::pair<std::string, std::string>> SideValue(const FTags& Tags, const char* Side)
{
	for (const std::string& Key : {std::string("parking:") + Side, std::string("parking:both")})
	{
		const std::string* Value = FindTag(Tags, Key.c_str());
		if (Value == nullptr)
		{
			continue;
		}
		std::string Orientation = OsmTags::ValueOrEmpty(Tags, (std::string("parking:") + Side + ":orientation").c_str());
		if (Orientation.empty())
		{
			Orientation = OsmTags::ValueOrEmpty(Tags, "parking:both:orientation");
		}
		if (Orientation.empty())
		{
			Orientation = "parallel";
		}
		return std::make_pair(*Value, Orientation);
	}
	for (const std::string& Key : {std::string("parking:lane:") + Side, std::string("parking:lane:both")})
	{
		const std::string* Value = FindTag(Tags, Key.c_str());
		if (Value != nullptr)
		{
			return std::make_pair(*Value, *Value);
		}
	}
	return std::nullopt;
}

/** "lane", "street_side", "half_on_kerb" or "on_kerb" for a tagged side, or nothing when no car parks there. */
std::string TaggedPosition(const std::optional<std::pair<std::string, std::string>>& Tagged)
{
	if (!Tagged.has_value())
	{
		return "";
	}
	const std::string& Value = Tagged->first;
	const std::string& Orientation = Tagged->second;
	if (NoneValues.count(Value) > 0 || AngledValues.count(Orientation) > 0 || AngledValues.count(Value) > 0)
	{
		return "";
	}
	if (Value == "half_on_kerb" || Value == "on_kerb")
	{
		return Value;
	}
	if (ParallelValues.count(Value) > 0 || Orientation == "parallel")
	{
		return Value == "street_side" ? "street_side" : "lane";
	}
	return "";
}

/** The footprint corners of a car whose middle is at Centre and which points along Along. */
void Footprint(const FStreetPoint& Centre, const FStreetPoint& Along, double CarLength, FStreetPoint (&Corners)[4])
{
	const FStreetPoint Side = {-Along.Y, Along.X};
	const FStreetPoint HalfLength = Along * (CarLength / 2.0);
	const FStreetPoint HalfWidth = Side * (CarWidth / 2.0);
	Corners[0] = Centre - HalfLength - HalfWidth;
	Corners[1] = Centre + HalfLength - HalfWidth;
	Corners[2] = Centre + HalfLength + HalfWidth;
	Corners[3] = Centre - HalfLength + HalfWidth;
}

/** True when two convex polygons overlap or touch (separating axis test over the edges of both). */
bool ConvexOverlap(const std::vector<FStreetPoint>& First, const std::vector<FStreetPoint>& Second)
{
	for (const std::vector<FStreetPoint>* Polygon : {&First, &Second})
	{
		for (size_t Index = 0; Index < Polygon->size(); ++Index)
		{
			const FStreetPoint Edge = (*Polygon)[(Index + 1) % Polygon->size()] - (*Polygon)[Index];
			const FStreetPoint Axis = {-Edge.Y, Edge.X};
			double FirstLow = 1e300;
			double FirstHigh = -1e300;
			double SecondLow = 1e300;
			double SecondHigh = -1e300;
			for (const FStreetPoint& Point : First)
			{
				FirstLow = std::min(FirstLow, Dot(Point, Axis));
				FirstHigh = std::max(FirstHigh, Dot(Point, Axis));
			}
			for (const FStreetPoint& Point : Second)
			{
				SecondLow = std::min(SecondLow, Dot(Point, Axis));
				SecondHigh = std::max(SecondHigh, Dot(Point, Axis));
			}
			if (FirstHigh < SecondLow || SecondHigh < FirstLow)
			{
				return false;
			}
		}
	}
	return true;
}

/** The distance between two convex polygons: 0 when they overlap. */
double ConvexDistance(const std::vector<FStreetPoint>& First, const std::vector<FStreetPoint>& Second)
{
	if (ConvexOverlap(First, Second))
	{
		return 0.0;
	}
	double Best = 1e300;
	for (int Pass = 0; Pass < 2; ++Pass)
	{
		const std::vector<FStreetPoint>& Points = Pass == 0 ? First : Second;
		const std::vector<FStreetPoint>& Edges = Pass == 0 ? Second : First;
		for (const FStreetPoint& Point : Points)
		{
			for (size_t Index = 0; Index < Edges.size(); ++Index)
			{
				Best = std::min(Best, Polyline::DistanceToSegment(Edges[Index], Edges[(Index + 1) % Edges.size()], Point));
			}
		}
	}
	return Best;
}

FBox BoxOf(const std::vector<FStreetPoint>& Points)
{
	FBox Box{1e300, 1e300, -1e300, -1e300};
	for (const FStreetPoint& Point : Points)
	{
		Box.X0 = std::min(Box.X0, Point.X);
		Box.Y0 = std::min(Box.Y0, Point.Y);
		Box.X1 = std::max(Box.X1, Point.X);
		Box.Y1 = std::max(Box.Y1, Point.Y);
	}
	return Box;
}


/** True when two segments cross properly (their interiors meet at one point). */
bool SegmentsCross(const FStreetPoint& FirstStart, const FStreetPoint& FirstEnd, const FStreetPoint& SecondStart,
				   const FStreetPoint& SecondEnd, bool bTouchingCounts)
{
	const auto Side = [](const FStreetPoint& Origin, const FStreetPoint& Along, const FStreetPoint& Point) {
		return (Along.X - Origin.X) * (Point.Y - Origin.Y) - (Along.Y - Origin.Y) * (Point.X - Origin.X);
	};
	const double A = Side(FirstStart, FirstEnd, SecondStart);
	const double B = Side(FirstStart, FirstEnd, SecondEnd);
	const double C = Side(SecondStart, SecondEnd, FirstStart);
	const double D = Side(SecondStart, SecondEnd, FirstEnd);
	if (bTouchingCounts)
	{
		return A * B <= 0 && C * D <= 0;
	}
	return A * B < 0 && C * D < 0;
}

/** True when a point lies strictly inside a convex polygon given counter-clockwise or clockwise. */
bool InsideConvex(const std::vector<FStreetPoint>& Convex, const FStreetPoint& Point)
{
	bool bPositive = false;
	bool bNegative = false;
	for (size_t Index = 0; Index < Convex.size(); ++Index)
	{
		const FStreetPoint& Start = Convex[Index];
		const FStreetPoint& End = Convex[(Index + 1) % Convex.size()];
		const double Side = (End.X - Start.X) * (Point.Y - Start.Y) - (End.Y - Start.Y) * (Point.X - Start.X);
		bPositive = bPositive || Side > 0;
		bNegative = bNegative || Side < 0;
	}
	return !(bPositive && bNegative);
}

/** The pieces of a polygon set by bounding box, to answer questions about a car's footprint without clipping. */
class FPieceIndex
{
public:
	explicit FPieceIndex(const FPolygonSet& InSet) : Set(InSet), Index(64.0)
	{
		for (size_t Piece = 0; Piece < Set.All().size(); ++Piece)
		{
			Index.Insert(BoundsOf(Set.All()[Piece]), static_cast<int>(Piece));
		}
	}

	/** True when the convex polygon overlaps or touches the set (what shapely's intersects says). */
	bool Intersects(const std::vector<FStreetPoint>& Convex) const
	{
		for (int Piece : Index.Query(BoxOf(Convex)))
		{
			if (PieceIntersects(Set.All()[Piece], Convex))
			{
				return true;
			}
		}
		return false;
	}

	/** True when one piece holds the whole convex polygon. */
	bool OnePieceHolds(const std::vector<FStreetPoint>& Convex) const
	{
		for (int Piece : Index.Query(BoxOf(Convex)))
		{
			if (PieceHolds(Set.All()[Piece], Convex))
			{
				return true;
			}
		}
		return false;
	}

private:
	const FPolygonSet& Set;
	FSpatialIndex Index;

	static bool PieceIntersects(const FPolygons& Piece, const std::vector<FStreetPoint>& Convex)
	{
		for (const FStreetPoint& Corner : Convex)
		{
			if (Contains(Piece, Corner.X, Corner.Y))
			{
				return true;
			}
		}
		for (const Clipper2Lib::PathD& Ring : Piece)
		{
			for (size_t Point = 0; Point < Ring.size(); ++Point)
			{
				const Clipper2Lib::PointD& Start = Ring[Point];
				const Clipper2Lib::PointD& End = Ring[(Point + 1) % Ring.size()];
				if (InsideConvex(Convex, {Start.x, Start.y}))
				{
					return true;
				}
				for (size_t Edge = 0; Edge < Convex.size(); ++Edge)
				{
					if (SegmentsCross({Start.x, Start.y}, {End.x, End.y}, Convex[Edge], Convex[(Edge + 1) % Convex.size()], true))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	static bool PieceHolds(const FPolygons& Piece, const std::vector<FStreetPoint>& Convex)
	{
		for (const FStreetPoint& Corner : Convex)
		{
			if (!Contains(Piece, Corner.X, Corner.Y))
			{
				return false;
			}
		}
		for (const Clipper2Lib::PathD& Ring : Piece)
		{
			for (size_t Point = 0; Point < Ring.size(); ++Point)
			{
				const Clipper2Lib::PointD& Start = Ring[Point];
				const Clipper2Lib::PointD& End = Ring[(Point + 1) % Ring.size()];
				for (size_t Edge = 0; Edge < Convex.size(); ++Edge)
				{
					if (SegmentsCross({Start.x, Start.y}, {End.x, End.y}, Convex[Edge], Convex[(Edge + 1) % Convex.size()], false))
					{
						return false;
					}
				}
				if (InsideConvex(Convex, {Start.x, Start.y}))
				{
					// A ring vertex inside the footprint without any crossing is a hole inside it.
					bool bOnBoundary = false;
					for (size_t Edge = 0; Edge < Convex.size(); ++Edge)
					{
						bOnBoundary = bOnBoundary
							|| Polyline::DistanceToSegment(Convex[Edge], Convex[(Edge + 1) % Convex.size()], {Start.x, Start.y}) < 1e-9;
					}
					if (!bOnBoundary)
					{
						return false;
					}
				}
			}
		}
		return true;
	}
};

/** Discs around junctions, crossings, signals, stop signs and bus stops that cars keep clear. */
class FKeepout
{
public:
	FKeepout(const FOsmData& Data, const FRoadGraph& Graph, const std::vector<FZebra>& Zebras) : Index(64.0)
	{
		for (const auto& [Node, Entries] : Graph.AtNode)
		{
			if (!Graph.IsJunction(Node))
			{
				continue;
			}
			double HalfWidth = 0.0;
			for (const FNodeEntry& Entry : Entries)
			{
				HalfWidth = std::max(HalfWidth, Graph.WayWidth[Entry.Way]);
			}
			AddDisc(Graph.NodePoint.at(Node), HalfWidth / 2.0 + JunctionGap);
		}
		for (const FOsmPoint& Point : Data.Points)
		{
			const std::optional<double> Radius = PointRadius(Point.Tags);
			if (Radius.has_value())
			{
				AddDisc({Point.Position.X, Point.Position.Y}, *Radius);
			}
		}
		for (const FZebra& Zebra : Zebras)
		{
			AddDisc({Zebra.X, Zebra.Y}, CrossingGap);
		}
	}

	/** True when a convex polygon touches any disc. */
	bool Intersects(const std::vector<FStreetPoint>& Polygon) const
	{
		for (int Disc : Index.Query(BoxOf(Polygon)))
		{
			if (ConvexOverlap(Discs[Disc], Polygon))
			{
				return true;
			}
		}
		return false;
	}

private:
	std::vector<std::vector<FStreetPoint>> Discs;
	FSpatialIndex Index;

	static std::optional<double> PointRadius(const FTags& Tags)
	{
		const std::string Highway = OsmTags::ValueOrEmpty(Tags, "highway");
		if (Highway == "traffic_signals" || Highway == "stop")
		{
			return SignalGap;
		}
		if (Highway == "give_way")
		{
			return JunctionGap;
		}
		if (Highway == "crossing")
		{
			return CrossingGap;
		}
		if (Highway == "bus_stop")
		{
			return BusStopGap;
		}
		if (Highway == "mini_roundabout")
		{
			return MiniRoundaboutGap;
		}
		if (OsmTags::TagEquals(Tags, "public_transport", "platform"))
		{
			return BusStopGap;
		}
		return std::nullopt;
	}

	/** A buffer(radius) disc: DiscSegments corners starting at angle 0 and running clockwise in a y-up frame. */
	void AddDisc(const FStreetPoint& Centre, double Radius)
	{
		std::vector<FStreetPoint> Disc;
		for (int Corner = 0; Corner < DiscSegments; ++Corner)
		{
			const double Angle = -2.0 * std::numbers::pi * Corner / DiscSegments;
			Disc.push_back({Centre.X + Radius * std::cos(Angle), Centre.Y + Radius * std::sin(Angle)});
		}
		Index.Insert(BoxOf(Disc), static_cast<int>(Discs.size()));
		Discs.push_back(std::move(Disc));
	}
};

class FParkedCarBuilder
{
public:
	FParkedCarBuilder(const FOsmData& Data, const FRoadGraph& InGraph, const std::vector<FZebra>& Zebras,
					  const FPolygonSet& InCarriageway, const FPolygonSet& InBuildings)
		: Graph(InGraph), Carriageway(InCarriageway), CarriagewayPieces(InCarriageway),
		  BuildingPieces(InBuildings), Keepout(Data, InGraph, Zebras)
	{
	}

	std::vector<FParkedCar> Build(const std::optional<FBox>& Region);

private:
	const FRoadGraph& Graph;
	const FPolygonSet& Carriageway;
	FPieceIndex CarriagewayPieces;
	FPieceIndex BuildingPieces;
	FKeepout Keepout;
	std::vector<FParkedCar> Cars;

	int PickModel(FNumpyRandom& Random) const;
	void FillSide(int Way, double Width, const FParkingSide& Side);
	std::optional<FParkedCar> TryCar(int Way, double Width, const FParkingSide& Side, bool bOneway, double S,
									 double CarLength, int Model, FNumpyRandom& Random) const;
	std::pair<FStreetPoint, FStreetPoint> CentreAt(int Way, double S, double Offset) const;
	bool IsMostlyOnCarriageway(const std::vector<FStreetPoint>& Box, double BoxArea) const;
	void DropOverlaps();
};

bool FParkedCarBuilder::IsMostlyOnCarriageway(const std::vector<FStreetPoint>& Box, double BoxArea) const
{
	FPolygons BoxPolygon(1);
	for (const FStreetPoint& Corner : Box)
	{
		BoxPolygon[0].emplace_back(Corner.X, Corner.Y);
	}
	return Carriageway.CoveredArea(BoxPolygon) >= 0.75 * BoxArea - 1e-9;
}

int FParkedCarBuilder::PickModel(FNumpyRandom& Random) const
{
	double Total = 0.0;
	for (const FParkedCarModel& Model : ParkedCarModels())
	{
		Total += Model.Weight;
	}
	std::vector<double> Probabilities;
	for (const FParkedCarModel& Model : ParkedCarModels())
	{
		Probabilities.push_back(Model.Weight / Total);
	}
	return Random.Choice(Probabilities);
}

std::pair<FStreetPoint, FStreetPoint> FParkedCarBuilder::CentreAt(int Way, double S, double Offset) const
{
	const FPositionAndDirection Here = Graph.PointAndDirection(Way, S, +1);
	const FStreetPoint Right = {-Here.Direction.Y, Here.Direction.X};
	return {Here.Position + Right * Offset, Here.Direction};
}

std::optional<FParkedCar> FParkedCarBuilder::TryCar(int Way, double Width, const FParkingSide& Side, bool bOneway,
													double S, double CarLength, int Model, FNumpyRandom& Random) const
{
	double KerbTilt = 0.0;
	double Extra = Random.Uniform(0.0, 0.18);
	const bool bKerbPosition = Side.Position == "half_on_kerb" || Side.Position == "on_kerb";
	if (bKerbPosition || Random.Random() < OnKerbChance)
	{
		Extra = Side.Position != "on_kerb" ? 0.55 : 0.9;
		KerbTilt = 3.0;
	}
	const double Offset = Side.Sign * (Width / 2.0 - CarWidth / 2.0 - 0.15 + Extra);
	const FStreetPoint Rear = CentreAt(Way, S, Offset).first;
	const FStreetPoint Front = CentreAt(Way, S + CarLength, Offset).first;
	FStreetPoint Along = Front - Rear;
	if (Norm(Along) < 1e-3)
	{
		return std::nullopt;
	}
	Along = Along / Norm(Along);
	if (Random.Random() < 0.15)
	{
		const double Wobble = Random.Uniform(-4.0, 4.0) * std::numbers::pi / 180.0;
		Along = {Along.X * std::cos(Wobble) - Along.Y * std::sin(Wobble),
				 Along.X * std::sin(Wobble) + Along.Y * std::cos(Wobble)};
	}
	const FStreetPoint Centre = (Rear + Front) / 2.0;
	const double Facing = (Side.Sign > 0 || bOneway) ? 1.0 : -1.0;
	const FStreetPoint Heading = Along * Facing;
	FParkedCar Car;
	Footprint(Centre, Along, CarLength, Car.Corners);
	const std::vector<FStreetPoint> Box(std::begin(Car.Corners), std::end(Car.Corners));
	if (Keepout.Intersects(Box))
	{
		return std::nullopt;
	}
	if (BuildingPieces.Intersects(Box))
	{
		return std::nullopt;
	}
	if (!CarriagewayPieces.OnePieceHolds(Box) && !IsMostlyOnCarriageway(Box, CarLength * CarWidth))
	{
		return std::nullopt;
	}
	Car.X = Centre.X;
	Car.Y = Centre.Y;
	Car.YawDegrees = std::atan2(Heading.Y, Heading.X) * 180.0 / std::numbers::pi;
	Car.Model = Model;
	Car.Length = CarLength;
	Car.Heading = Heading;
	Car.KerbTilt = KerbTilt;
	Car.Sign = Side.Sign;
	Car.Facing = Facing;
	return Car;
}

void FParkedCarBuilder::FillSide(int Way, double Width, const FParkingSide& Side)
{
	FNumpyRandom Random = StableRandom(Graph.Ways[Way].Id, Side.Sign + 3);
	const double Length = Graph.LineLength(Way);
	const bool bOneway = OsmTags::IsOneway(Graph.Ways[Way].Tags);
	double S = WayEndGap + Random.Uniform(0.0, 3.0);
	while (true)
	{
		if (Random.Random() < EmptyStretchChance)
		{
			S += Random.Uniform(6.0, 24.0);
		}
		const int Model = PickModel(Random);
		const double CarLength = ParkedCarModels()[Model].Length;
		if (S + CarLength > Length - WayEndGap)
		{
			return;
		}
		std::optional<FParkedCar> Car = TryCar(Way, Width, Side, bOneway, S, CarLength, Model, Random);
		if (!Car.has_value())
		{
			S += 0.8;
			continue;
		}
		Cars.push_back(*Car);
		const double Gap = Random.Random() < 0.12 ? Random.Uniform(1.8, 4.0) : Random.Uniform(0.35, 1.3);
		S += CarLength + Gap;
	}
}

void FParkedCarBuilder::DropOverlaps()
{
	FSpatialIndex Index(16.0);
	for (size_t Car = 0; Car < Cars.size(); ++Car)
	{
		const std::vector<FStreetPoint> Box(std::begin(Cars[Car].Corners), std::end(Cars[Car].Corners));
		Index.Insert(BoxOf(Box), static_cast<int>(Car));
	}
	std::vector<bool> Removed(Cars.size(), false);
	std::vector<FParkedCar> Kept;
	for (size_t Car = 0; Car < Cars.size(); ++Car)
	{
		if (Removed[Car])
		{
			continue;
		}
		const std::vector<FStreetPoint> Box(std::begin(Cars[Car].Corners), std::end(Cars[Car].Corners));
		FBox Search = BoxOf(Box);
		Search.X0 -= OverlapGap;
		Search.Y0 -= OverlapGap;
		Search.X1 += OverlapGap;
		Search.Y1 += OverlapGap;
		for (int Other : Index.Query(Search))
		{
			if (Other <= static_cast<int>(Car))
			{
				continue;
			}
			const std::vector<FStreetPoint> OtherBox(std::begin(Cars[Other].Corners), std::end(Cars[Other].Corners));
			if (ConvexDistance(Box, OtherBox) <= OverlapGap)
			{
				Removed[Other] = true;
			}
		}
		Kept.push_back(Cars[Car]);
	}
	Cars = std::move(Kept);
}

std::vector<FParkedCar> FParkedCarBuilder::Build(const std::optional<FBox>& Region)
{
	for (size_t WayIndex = 0; WayIndex < Graph.Ways.size(); ++WayIndex)
	{
		const int Way = static_cast<int>(WayIndex);
		if (Region.has_value())
		{
			FPolyline Line;
			for (const FStreetPoint& Point : Graph.Ways[Way].Points)
			{
				Line.push_back({Point.X, Point.Y});
			}
			if (ClipPolylineToBox(Line, *Region).empty())
			{
				continue;
			}
		}
		const double Width = Graph.WayWidth[Way];
		for (const FParkingSide& Side : ParkingSides(Graph.Ways[Way].Tags, Graph.Ways[Way].Id, Width))
		{
			FillSide(Way, Width, Side);
		}
	}
	DropOverlaps();
	return std::move(Cars);
}
}

const std::vector<FParkedCarModel>& ParkedCarModels()
{
	static const std::vector<FParkedCarModel> Models = {
		{"vehicle07_Car", 4.45, 1.6}, {"vehicle02_Car", 4.65, 1.0}, {"vehicle03_Car", 4.85, 0.8},
		{"vehicle05_Car", 4.70, 1.2}, {"vehicle06_Car", 4.40, 0.5}, {"vehicle01_Van", 5.20, 0.45}};
	return Models;
}

std::vector<FParkingSide> ParkingSides(const FTags& Tags, int64_t WayId, double Width)
{
	const std::string Highway = OsmTags::ValueOrEmpty(Tags, "highway");
	if (NoParkingClasses.count(Highway) > 0 || OsmTags::TagEquals(Tags, "junction", "roundabout")
		|| OsmTags::TagEquals(Tags, "junction", "circular"))
	{
		return {};
	}
	std::vector<FParkingSide> Sides;
	const bool bTagged = HasParkingTags(Tags);
	if (bTagged)
	{
		for (const auto& [Sign, Name] : {std::pair<int, const char*>{+1, "right"}, std::pair<int, const char*>{-1, "left"}})
		{
			const std::string Position = TaggedPosition(SideValue(Tags, Name));
			if (!Position.empty())
			{
				Sides.push_back({Sign, Position});
			}
		}
	}
	else if (DefaultParkingClasses.count(Highway) > 0 && Width >= ParkingMinWidth)
	{
		FNumpyRandom Random = StableRandom(WayId, 7);
		const int Chosen = Random.Random() < 0.5 ? +1 : -1;
		Sides.push_back({Chosen, "lane"});
		if (Width >= ParkingBothWidth)
		{
			Sides.push_back({-Chosen, "lane"});
		}
	}
	else
	{
		return {};
	}
	const double MinimumBoth = bTagged ? TaggedBothMinWidth : ParkingBothWidth;
	if (Sides.size() == 2 && Width < MinimumBoth)
	{
		FNumpyRandom Random = StableRandom(WayId, 7);
		const int Keep = Random.Random() < 0.5 ? +1 : -1;
		const auto Found = std::find_if(Sides.begin(), Sides.end(), [Keep](const FParkingSide& Side) { return Side.Sign == Keep; });
		Sides = {*Found};
	}
	return Sides;
}

FParkedCarPose FinishParkedCar(const FParkedCar& Car, const std::function<double(double X, double Y)>& RoadHeight)
{
	const double Reach = Car.Length * 0.31;  // half the wheelbase
	const FStreetPoint Centre = {Car.X, Car.Y};
	const FStreetPoint Rear = Centre - Car.Heading * Reach;
	const FStreetPoint Front = Centre + Car.Heading * Reach;
	const double RearHeight = RoadHeight(Rear.X, Rear.Y);
	const double FrontHeight = RoadHeight(Front.X, Front.Y);
	FParkedCarPose Pose;
	Pose.PitchDegrees = std::atan2(FrontHeight - RearHeight, 2 * Reach) * 180.0 / std::numbers::pi;
	// Unreal rolls the right side down; the kerb side is the car's right when it faces with the traffic of its side.
	const bool bKerbOnRight = Car.Sign * Car.Facing > 0;
	Pose.RollDegrees = bKerbOnRight ? -Car.KerbTilt : Car.KerbTilt;
	Pose.Z = (RearHeight + FrontHeight) / 2.0 + (Car.KerbTilt != 0.0 ? 0.04 : 0.0);
	return Pose;
}

std::vector<FParkedCar> BuildParkedCars(const FOsmData& Data, const FRoadGraph& Graph, const std::vector<FZebra>& Zebras,
										const FPolygonSet& Carriageway, const FPolygonSet& Buildings,
										const std::optional<FBox>& Region)
{
	FParkedCarBuilder Builder(Data, Graph, Zebras, Carriageway, Buildings);
	return Builder.Build(Region);
}
}
