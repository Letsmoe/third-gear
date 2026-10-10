#include "SiteStructures.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

#include "PlacementFrame.h"

namespace WorldBuilder
{
namespace
{
// ------------------------------------------------------------------------------------------------ bus stops and bins

/** A bus stop looks for the road within this distance, metres. */
constexpr double StopSearchRadius = 25.0;
/** A stop node closer than this to the road line is taken to be on the road, which serves its right side. */
constexpr double NodeOnRoadOffset = 1.0;
/** Masts closer than this to another mast are the same stop mapped twice (node and platform). */
constexpr double MastMergeDistance = 4.0;
/** The mast stands this far beyond the road surface's edge. */
constexpr double MastSetback = 0.5;
/** The front edge of a shelter stands this far beyond the road surface's edge. */
constexpr double ShelterSetback = 1.0;
/** Gap between the mast and the shelter along the road, metres. */
constexpr double ShelterGap = 1.5;
/** Half the depth of a shelter's footprint plus its front overhang, and where its middle lies behind the front edge. */
constexpr double ShelterHalfDepth = 1.2;
constexpr double ShelterMiddle = 0.4;
/** A bin faces a road within this distance. */
constexpr double BinRoadReach = 10.0;
/** Walking outward from the road centre stops after this distance. */
constexpr double MaxEdgeSearch = 25.0;
constexpr double EdgeSearchStep = 0.25;

/** Kit shelters by name with their half length in metres (shelters.Glass_2Bay.footprint_x and so on). */
struct FShelterKind
{
	const char* Piece;
	double HalfLength;
};
constexpr FShelterKind Glass2Bay{"shelters_Glass_2Bay", 1.65};
constexpr FShelterKind Glass3Bay{"shelters_Glass_3Bay", 2.4};
constexpr FShelterKind Timber{"shelters_Timber", 1.7};
/** Share of the glass shelters that have three bays. */
constexpr double ThreeBayShare = 0.3;

const char* const AdVariants[] = {"bus_drivers", "bank", "boogle", "culture", "supermarket"};
constexpr int BinStickerCount = 5;

/** Where a stop's road side is: the road centre beside the stop, travel and right directions, and the road's edge. */
struct FRoadSide
{
	int Way = 0;
	FStreetPoint Centre;
	FStreetPoint Travel;
	FStreetPoint Right;
	/** Distance from the centre to the road surface's edge along Right. */
	double EdgeDistance = 0.0;
};

bool HasTagValue(const FTags& Tags, const char* Key, const char* Value)
{
	const std::string* Found = FindTag(Tags, Key);
	return Found != nullptr && *Found == Value;
}

std::string Lowercase(std::string Text)
{
	std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char Character) { return std::tolower(Character); });
	return Text;
}

// ---------------------------------------------------------------------------------------------- petrol stations

/** petrol.py: the size of the pieces and the gaps of station_layout, metres. */
constexpr double CanopyLength = 14.0;
constexpr double ShopWidth = 16.0;
constexpr double ShopDepth = 10.0;
constexpr double CarWashLength = 12.0;
constexpr double WashWidth = 6.0;
constexpr double CanopyFront = 5.0;
constexpr double ShopGap = 5.0;
constexpr double PlotBackMargin = 2.0;
constexpr double DefaultIslandSpacing = 7.5;
/** A station is only laid out on a plot at least this wide and deep, metres (a canopy of two islands and its aprons). */
constexpr double MinimumPlotWidth = 22.0;
constexpr double MinimumPlotDepth = 22.0;
/** Hamburg stations with fewer metres of frontage than this get no car wash. */
constexpr double CarWashPlotWidth = 31.5;
/** A fuel node this close to a station from a fuel area is the same station mapped twice. */
constexpr double StationMergeDistance = 80.0;
/** A lone fuel node looks for its road within this distance. */
constexpr double LoneStationRoadReach = 45.0;
/** A lone station's plot starts this far beyond the road surface's edge, metres. */
constexpr double LoneStationSetback = 2.0;
constexpr int MinIslands = 2;
constexpr int MaxIslands = 4;

/** One side of a plot's rectangle: its midpoint, the outward normal, the frontage along it and the depth across. */
struct FSide
{
	FWorldPoint Middle;
	FWorldPoint Outward;
	double Frontage;
	double Depth;
	double RoadDistance;
};

/** What a plot of a given size gets. */
struct FStationPlan
{
	int Islands = 2;
	bool bCarWash = false;
	bool bShop = false;
	double PlotWidth = 0.0;
};

/** The pieces that fit a plot, or nothing when it is too small for a canopy. */
std::optional<FStationPlan> PlanStation(double Frontage, double Depth, double IslandSpacing)
{
	if (Frontage < MinimumPlotWidth || Depth < MinimumPlotDepth)
	{
		return std::nullopt;
	}
	FStationPlan Plan;
	const double ShopFrontToBack = ShopDepth + PlotBackMargin;
	const int Fitting = static_cast<int>(std::floor((Depth - CanopyFront - ShopGap - ShopFrontToBack) / IslandSpacing));
	Plan.Islands = std::clamp(Fitting, MinIslands, MaxIslands);
	Plan.bShop = CanopyFront + Plan.Islands * IslandSpacing + ShopGap + ShopFrontToBack <= Depth;
	Plan.bCarWash = Frontage >= CarWashPlotWidth;
	const double RightSide = Plan.bCarWash ? WashWidth + 7.0 : 6.0;
	Plan.PlotWidth = 4.5 + CanopyLength + RightSide;
	return Plan;
}

/** The brand material variant for an OSM brand or name: Aral, BP, Shell and the like to nordtank, the discounters to hopp. */
std::string BrandVariant(const FTags& Tags, double X, double Y)
{
	const std::string* Brand = FindTag(Tags, "brand");
	const std::string* Name = FindTag(Tags, "name");
	const std::string Label = Lowercase(Brand != nullptr ? *Brand : (Name != nullptr ? *Name : std::string()));
	for (const char* Large : {"aral", "bp", "shell", "esso", "total"})
	{
		if (Label.rfind(Large, 0) == 0)
		{
			return "nordtank";
		}
	}
	for (const char* Discount : {"jet", "star", "oil", "hoyer", "avia", "as 24", "as24", "lidl", "aldi", "edeka", "famila"})
	{
		if (Label.rfind(Discount, 0) == 0)
		{
			return "hopp";
		}
	}
	if (Label.rfind("hem", 0) == 0)
	{
		return "vierlaender";
	}
	const char* const Brands[] = {"nordtank", "hopp", "vierlaender"};
	return Brands[static_cast<int>(PositionNoise(X, Y, 11) * 3.0) % 3];
}

// ---------------------------------------------------------------------------------------------------- greenhouses

/** A greenhouse is rebuilt from the kit when its footprint fills this share of its rectangle. */
constexpr double GreenhouseRectangleFill = 0.85;
/** Smallest footprint worth a kit greenhouse, square metres, and the narrowest side in metres. */
constexpr double MinimumGreenhouseArea = 50.0;
constexpr double MinimumGreenhouseWidth = 5.5;
/** Houses of the old type are the small ones: below this area, square metres. */
constexpr double OldGlasshouseMaxArea = 600.0;
/** Long narrow houses up to this wide, with at least this length to width ratio, are foil tunnels. */
constexpr double TunnelMaxWidth = 10.5;
constexpr double TunnelMinAspect = 3.0;

/** kit/greenhouses.py's GlasshouseStyle numbers that glasshouse_parts needs, read from the kit spec. */
struct FGlasshouseStyle
{
	std::string Name;
	double Span = 3.2;
	double Bay = 4.0;
	double GutterHeight = 4.5;
};

class FSiteBuilder
{
public:
	FSiteBuilder(const FOsmData& InOsm, const FRoadGraph& InGraph, const FPolygonSet& InRoadGround,
				 const FPolygonSet& InBuildings, const FKitSpecs& InSpecs, const FGroundSampler& InGround)
		: Osm(InOsm), Graph(InGraph), RoadGround(InRoadGround), Buildings(InBuildings), Specs(InSpecs), Ground(InGround)
	{
	}

	/** Runs every kind of placement. */
	FSiteStructures Build(FSiteCounts* Counts);

private:
	const FOsmData& Osm;
	const FRoadGraph& Graph;
	const FPolygonSet& RoadGround;
	const FPolygonSet& Buildings;
	const FKitSpecs& Specs;
	const FGroundSampler& Ground;
	FSiteStructures Result;
	FSiteCounts Totals;
	std::vector<FWorldPoint> MastPositions;
	std::vector<FWorldPoint> StationCentres;

	/** Appends a placement; bFootHeight says its Z is found per tile (pavement or ground). */
	void Add(const FStructurePlacement& Placement, bool bFootHeight);
	/**
	 * The road beside a point: the nearest way, the travel direction of the side the point is on (the right side when it
	 * lies on the road and bRightWhenOnRoad), and the distance to the road surface's edge. Nothing when none is near.
	 */
	std::optional<FRoadSide> FindRoadSide(double X, double Y, double MaxDistance, bool bRightWhenOnRoad) const;

	/** Places the mast and, where OSM says shelter=yes, the shelter of every highway=bus_stop. */
	void PlaceBusStops();
	void PlaceBusStop(const FOsmPoint& Stop);
	/** The shelter upstream of the mast, open toward the road; left out when it would stand on the road or a building. */
	void PlaceShelter(const FOsmPoint& Stop, const FRoadSide& Side, const FStreetPoint& MastPosition);
	/** True when another mast stands within MastMergeDistance. */
	bool IsMastNearAnother(const FStreetPoint& Position) const;
	/** True when the shelter's footprint touches the road surface or a building. */
	bool BlocksShelter(const FOrientedRectangle& Footprint) const;

	/** Places a bin for every amenity=waste_basket that is neither on the road nor in a building. */
	void PlaceBins();
	void PlaceBin(const FOsmPoint& Basket);

	/** Lays a station out in every amenity=fuel area, and drops the OSM buildings inside it. */
	void PlaceFuelAreas();
	void PlaceFuelAreaStation(const FOsmArea& Area);
	/** Places a standard station for fuel nodes that no area covers, where the plot behind the road is clear. */
	void PlaceLoneFuelStations();
	void PlaceLoneFuelStation(const FOsmPoint& Node);
	/** True when a rectangle at the frame's plot position (centre u, from v to v + Depth) covers the road surface. */
	bool HitsRoad(const FPlacementFrame& Frame, double CentreU, double FrontV, double HalfWidth, double Depth) const;
	/** Puts the pieces of the plan on a plot (petrol.py's station_parts), all at height Z. */
	void AddStation(const FPlacementFrame& Frame, const FStationPlan& Plan, const std::string& Variant, float Z);
	/** True when a station from a fuel area already stands within StationMergeDistance of the node. */
	bool IsInsideFuelArea(const FOsmPoint& Node) const;
	/** True when a rectangle covers the road surface or a building. */
	bool IsBlocked(const FOrientedRectangle& Footprint) const;

	/** Rebuilds every nearly rectangular building=greenhouse from the kit. */
	void PlaceGreenhouses();
	void PlaceGreenhouse(const FOsmArea& Building);
	/** greenhouses.py's glasshouse_parts: gables, sides, posts, gutters, roofs, columns and trusses of a house. */
	void AddGlasshouse(const FPlacementFrame& Frame, const FGlasshouseStyle& Style, int Spans, int Bays, float Z);
	/** greenhouses.py's tunnel_parts. */
	void AddTunnel(const FPlacementFrame& Frame, int Sections, float Z);
	/** The span, bay and gutter height of a glasshouse style from the kit spec. */
	FGlasshouseStyle ReadStyle(const std::string& Name) const;
};

FSiteStructures FSiteBuilder::Build(FSiteCounts* Counts)
{
	PlaceBusStops();
	PlaceBins();
	PlaceFuelAreas();
	PlaceLoneFuelStations();
	PlaceGreenhouses();
	if (Counts != nullptr)
	{
		*Counts = Totals;
	}
	return std::move(Result);
}

void FSiteBuilder::Add(const FStructurePlacement& Placement, bool bFootHeight)
{
	Result.Placements.push_back({Placement, bFootHeight});
}

std::optional<FRoadSide> FSiteBuilder::FindRoadSide(double X, double Y, double MaxDistance, bool bRightWhenOnRoad) const
{
	const std::optional<FNearestWay> Found = Graph.NearestWay(X, Y, MaxDistance);
	if (!Found.has_value())
	{
		return std::nullopt;
	}
	int Travel = Found->Offset >= 0.0 ? +1 : -1;
	if (bRightWhenOnRoad && std::abs(Found->Offset) < NodeOnRoadOffset)
	{
		Travel = +1;
	}
	if (!Graph.CanTravel(Found->Way, Travel))
	{
		Travel = -Travel;
	}
	if (!Graph.CanTravel(Found->Way, Travel))
	{
		return std::nullopt;
	}
	const FPositionAndDirection Here = Graph.PointAndDirection(Found->Way, Found->Along, Travel);
	FRoadSide Side;
	Side.Way = Found->Way;
	Side.Centre = Here.Position;
	Side.Travel = Here.Direction;
	Side.Right = Polyline::RightOf(Here.Direction);
	double Distance = 0.0;
	while (Distance < MaxEdgeSearch
		   && RoadGround.Contains(Side.Centre.X + Side.Right.X * Distance, Side.Centre.Y + Side.Right.Y * Distance))
	{
		Distance += EdgeSearchStep;
	}
	if (Distance >= MaxEdgeSearch)
	{
		return std::nullopt;
	}
	Side.EdgeDistance = Distance;
	return Side;
}

// ------------------------------------------------------------------------------------------------------ bus stops

void FSiteBuilder::PlaceBusStops()
{
	for (const FOsmPoint& Point : Osm.Points)
	{
		if (HasTagValue(Point.Tags, "highway", "bus_stop"))
		{
			PlaceBusStop(Point);
		}
	}
}

bool FSiteBuilder::IsMastNearAnother(const FStreetPoint& Position) const
{
	for (const FWorldPoint& Other : MastPositions)
	{
		if (std::hypot(Other.X - Position.X, Other.Y - Position.Y) < MastMergeDistance)
		{
			return true;
		}
	}
	return false;
}

void FSiteBuilder::PlaceBusStop(const FOsmPoint& Stop)
{
	const std::optional<FRoadSide> Side = FindRoadSide(Stop.Position.X, Stop.Position.Y, StopSearchRadius, true);
	if (!Side.has_value())
	{
		return;
	}
	const FStreetPoint Mast = Side->Centre + Side->Right * (Side->EdgeDistance + MastSetback);
	if (IsMastNearAnother(Mast) || Buildings.Contains(Mast.X, Mast.Y))
	{
		return;
	}
	MastPositions.push_back({Mast.X, Mast.Y});
	// The panel looks toward the oncoming traffic, so the mast's back is the way the buses travel.
	const FPlacementFrame Frame(Mast.X, Mast.Y, Side->Travel.X, Side->Travel.Y);
	Add(Frame.Piece("shelters_StopSign", 0.0, 0.0, 0.0f), true);
	++Totals.BusMasts;
	if (HasTagValue(Stop.Tags, "shelter", "yes"))
	{
		PlaceShelter(Stop, *Side, Mast);
	}
}

bool FSiteBuilder::BlocksShelter(const FOrientedRectangle& Footprint) const
{
	return RectangleHits(Footprint, 0.5, [&](double X, double Y) { return RoadGround.Contains(X, Y) || Buildings.Contains(X, Y); });
}

void FSiteBuilder::PlaceShelter(const FOsmPoint& Stop, const FRoadSide& Side, const FStreetPoint& MastPosition)
{
	const bool bUrban = Graph.WayUrban[Side.Way];
	const double Roll = PositionNoise(Stop.Position.X, Stop.Position.Y, 21);
	const FShelterKind& Kind = !bUrban ? Timber : (Roll < ThreeBayShare ? Glass3Bay : Glass2Bay);
	// Behind the mast along the road, the open front toward the road.
	const FStreetPoint Front = MastPosition - Side.Right * (MastSetback - ShelterSetback)
		- Side.Travel * (Kind.HalfLength + ShelterGap);
	FOrientedRectangle Footprint;
	Footprint.Centre = {Front.X + Side.Right.X * ShelterMiddle, Front.Y + Side.Right.Y * ShelterMiddle};
	Footprint.Axis = {Side.Travel.X, Side.Travel.Y};
	Footprint.HalfFirst = Kind.HalfLength;
	Footprint.HalfSecond = ShelterHalfDepth;
	if (BlocksShelter(Footprint))
	{
		return;
	}
	std::string Variant;
	if (bUrban)
	{
		Variant = AdVariants[static_cast<int>(PositionNoise(Stop.Position.X, Stop.Position.Y, 22) * 5.0) % 5];
	}
	// The shelter's back wall stands away from the road, along Right.
	const FPlacementFrame Frame(Front.X, Front.Y, Side.Right.X, Side.Right.Y);
	Add(Frame.Piece(Kind.Piece, 0.0, 0.0, 0.0f, 0.0, Variant), true);
	++Totals.Shelters;
}

// ---------------------------------------------------------------------------------------------------------- bins

void FSiteBuilder::PlaceBins()
{
	for (const FOsmPoint& Point : Osm.Points)
	{
		if (HasTagValue(Point.Tags, "amenity", "waste_basket"))
		{
			PlaceBin(Point);
		}
	}
}

void FSiteBuilder::PlaceBin(const FOsmPoint& Basket)
{
	const double X = Basket.Position.X;
	const double Y = Basket.Position.Y;
	if (RoadGround.Contains(X, Y) || Buildings.Contains(X, Y))
	{
		return;
	}
	// The bin faces the nearest road; its back is the direction away from it.
	const double Spin = PositionNoise(X, Y, 31) * 6.283185307179586;
	double BackX = std::cos(Spin);
	double BackY = std::sin(Spin);
	const std::optional<FNearestWay> Found = Graph.NearestWay(X, Y, BinRoadReach);
	if (Found.has_value())
	{
		const FPositionAndDirection Road = Graph.PointAndDirection(Found->Way, Found->Along, +1);
		const double Away = std::hypot(X - Road.Position.X, Y - Road.Position.Y);
		if (Away > 0.1)
		{
			BackX = (X - Road.Position.X) / Away;
			BackY = (Y - Road.Position.Y) / Away;
		}
	}
	const FPlacementFrame Frame(X, Y, BackX, BackY);
	const std::string Sticker = std::to_string(static_cast<int>(PositionNoise(X, Y, 32) * BinStickerCount) % BinStickerCount);
	Add(Frame.Piece("shelters_Bin", 0.0, 0.0, 0.0f, 0.0, Sticker), true);
	++Totals.Bins;
}

// ---------------------------------------------------------------------------------------------- petrol stations

bool FSiteBuilder::IsBlocked(const FOrientedRectangle& Footprint) const
{
	return RectangleHits(Footprint, 1.0, [&](double X, double Y) { return RoadGround.Contains(X, Y) || Buildings.Contains(X, Y); });
}

bool FSiteBuilder::HitsRoad(const FPlacementFrame& Frame, double CentreU, double FrontV, double HalfWidth,
							double Depth) const
{
	FOrientedRectangle Footprint;
	Footprint.Axis = Frame.UAxis();
	Footprint.HalfFirst = HalfWidth;
	Footprint.HalfSecond = Depth / 2.0;
	Footprint.Centre = Frame.ToWorld(CentreU, FrontV + Depth / 2.0);
	return RectangleHits(Footprint, 1.0, [&](double X, double Y) { return RoadGround.Contains(X, Y); });
}

void FSiteBuilder::AddStation(const FPlacementFrame& Frame, const FStationPlan& Plan, const std::string& Variant, float Z)
{
	const double IslandSpacing = Specs.Number("petrol.island_spacing", DefaultIslandSpacing);
	const double CanopyDepth = Plan.Islands * IslandSpacing;
	const double CanopyX = -Plan.PlotWidth / 2.0 + 4.5 + CanopyLength / 2.0;
	const double ShopFront = CanopyFront + CanopyDepth + ShopGap;
	const double WashX = CanopyX + CanopyLength / 2.0 + 3.5 + WashWidth / 2.0;
	const auto Piece = [&](const char* Name, double U, double V, double Rotation) {
		Add(Frame.Piece(std::string("petrol_") + Name, U, V, Z, Rotation, Variant), false);
	};
	Piece(("Canopy_" + std::to_string(Plan.Islands)).c_str(), CanopyX, CanopyFront + CanopyDepth / 2.0, 0.0);
	for (int Island = 0; Island < Plan.Islands; ++Island)
	{
		Piece("PumpIsland", CanopyX, CanopyFront + (Island + 0.5) * IslandSpacing, 0.0);
	}
	// The fuel area is not a rectangle: what would stand on the road is left out.
	if (Plan.bShop && !HitsRoad(Frame, CanopyX, ShopFront, ShopWidth / 2.0, ShopDepth))
	{
		Piece("Shop", CanopyX, ShopFront, 0.0);
	}
	Piece("PricePylon", -Plan.PlotWidth / 2.0 + 1.5, 1.5, 90.0);
	if (!HitsRoad(Frame, CanopyX + CanopyLength / 2.0 + 2.0, ShopFront - 3.0, 2.0, 3.0))
	{
		Piece("AirVacuum", CanopyX + CanopyLength / 2.0 + 2.0, ShopFront - 2.0, 90.0);
	}
	if (Plan.bCarWash && !HitsRoad(Frame, WashX, CanopyFront, WashWidth / 2.0, CarWashLength))
	{
		Piece("CarWash", WashX, CanopyFront, 0.0);
	}
	++Totals.Stations;
}

void FSiteBuilder::PlaceFuelAreas()
{
	for (const FOsmArea& Area : Osm.Areas)
	{
		if (HasTagValue(Area.Tags, "amenity", "fuel"))
		{
			PlaceFuelAreaStation(Area);
		}
	}
}

/** The side of a plot's rectangle nearest the road (the longest one when no road is near). */
FSide FrontSide(const FOrientedRectangle& Rectangle, const FPolygonSet& RoadGround)
{
	constexpr double RoadLimit = 40.0;
	const FWorldPoint Second = {-Rectangle.Axis.Y, Rectangle.Axis.X};
	std::vector<FSide> Sides;
	for (const double Sign : {-1.0, 1.0})
	{
		Sides.push_back({Rectangle.Point(Sign * Rectangle.HalfFirst, 0.0), {Sign * Rectangle.Axis.X, Sign * Rectangle.Axis.Y},
						 2.0 * Rectangle.HalfSecond, 2.0 * Rectangle.HalfFirst, 0.0});
		Sides.push_back({Rectangle.Point(0.0, Sign * Rectangle.HalfSecond), {Sign * Second.X, Sign * Second.Y},
						 2.0 * Rectangle.HalfFirst, 2.0 * Rectangle.HalfSecond, 0.0});
	}
	size_t Best = 0;
	for (size_t Index = 0; Index < Sides.size(); ++Index)
	{
		Sides[Index].RoadDistance = RoadGround.Distance(Sides[Index].Middle.X, Sides[Index].Middle.Y, RoadLimit);
		if (Sides[Index].RoadDistance < Sides[Best].RoadDistance)
		{
			Best = Index;
		}
	}
	if (Sides[Best].RoadDistance < RoadLimit)
	{
		return Sides[Best];
	}
	for (size_t Index = 0; Index < Sides.size(); ++Index)
	{
		if (Sides[Index].Frontage > Sides[Best].Frontage)
		{
			Best = Index;
		}
	}
	return Sides[Best];
}

/** The rings of an area as paths, appended to Out. */
void AppendRings(const FOsmArea& Area, FPolygons& Out)
{
	for (const FOsmPolygon& Polygon : Area.Polygons)
	{
		for (const FPolyline& Ring : Polygon.Rings)
		{
			Clipper2Lib::PathD Path;
			for (const FWorldPoint& Point : Ring)
			{
				Path.emplace_back(Point.X, Point.Y);
			}
			Out.push_back(std::move(Path));
		}
	}
}

void FSiteBuilder::PlaceFuelAreaStation(const FOsmArea& Area)
{
	if (Area.Polygons.empty() || Area.Polygons[0].Rings.empty())
	{
		return;
	}
	const FOrientedRectangle Rectangle = MinimumAreaRectangle(Area.Polygons[0].Rings[0]);
	const FSide Front = FrontSide(Rectangle, RoadGround);
	const double IslandSpacing = Specs.Number("petrol.island_spacing", DefaultIslandSpacing);
	const std::optional<FStationPlan> Plan = PlanStation(Front.Frontage, Front.Depth, IslandSpacing);
	if (!Plan.has_value())
	{
		return;
	}
	const FPlacementFrame Frame(Front.Middle.X, Front.Middle.Y, -Front.Outward.X, -Front.Outward.Y);
	const FWorldPoint Centre = Frame.ToWorld(0.0, Front.Depth / 2.0);
	const float Z = static_cast<float>(Ground(Centre.X, Centre.Y));
	AddStation(Frame, *Plan, BrandVariant(Area.Tags, Centre.X, Centre.Y), Z);
	StationCentres.push_back(Centre);
	AppendRings(Area, Result.Replaced);
}

bool FSiteBuilder::IsInsideFuelArea(const FOsmPoint& Node) const
{
	for (const FWorldPoint& Centre : StationCentres)
	{
		if (std::hypot(Centre.X - Node.Position.X, Centre.Y - Node.Position.Y) < StationMergeDistance)
		{
			return true;
		}
	}
	return false;
}

void FSiteBuilder::PlaceLoneFuelStations()
{
	for (const FOsmPoint& Point : Osm.Points)
	{
		if (HasTagValue(Point.Tags, "amenity", "fuel") && !IsInsideFuelArea(Point))
		{
			PlaceLoneFuelStation(Point);
		}
	}
}

void FSiteBuilder::PlaceLoneFuelStation(const FOsmPoint& Node)
{
	const std::optional<FNearestWay> Found = Graph.NearestWay(Node.Position.X, Node.Position.Y, LoneStationRoadReach);
	if (!Found.has_value())
	{
		return;
	}
	const FPositionAndDirection Road = Graph.PointAndDirection(Found->Way, Found->Along, +1);
	FStreetPoint Away = Polyline::Unit({Node.Position.X - Road.Position.X, Node.Position.Y - Road.Position.Y});
	if (Norm({Node.Position.X - Road.Position.X, Node.Position.Y - Road.Position.Y}) < 1.0)
	{
		Away = Polyline::RightOf(Road.Direction);
	}
	double Distance = 0.0;
	while (Distance < MaxEdgeSearch && RoadGround.Contains(Road.Position.X + Away.X * Distance, Road.Position.Y + Away.Y * Distance))
	{
		Distance += EdgeSearchStep;
	}
	const FStreetPoint Origin = Road.Position + Away * (Distance + LoneStationSetback);
	const FPlacementFrame Frame(Origin.X, Origin.Y, Away.X, Away.Y);
	const double IslandSpacing = Specs.Number("petrol.island_spacing", DefaultIslandSpacing);
	FStationPlan Plan = *PlanStation(MinimumPlotWidth, MinimumPlotDepth + 15.0, IslandSpacing);
	Plan.bCarWash = false;
	Plan.PlotWidth = 4.5 + CanopyLength + 6.0;
	FOrientedRectangle Canopy;
	Canopy.Axis = Frame.UAxis();
	Canopy.HalfFirst = CanopyLength / 2.0 + 1.0;
	Canopy.HalfSecond = Plan.Islands * IslandSpacing / 2.0 + 1.0;
	const double CanopyX = -Plan.PlotWidth / 2.0 + 4.5 + CanopyLength / 2.0;
	Canopy.Centre = Frame.ToWorld(CanopyX, CanopyFront + Plan.Islands * IslandSpacing / 2.0);
	if (IsBlocked(Canopy))
	{
		return;
	}
	FOrientedRectangle Shop = Canopy;
	Shop.HalfFirst = ShopWidth / 2.0;
	Shop.HalfSecond = ShopDepth / 2.0;
	Shop.Centre = Frame.ToWorld(CanopyX, CanopyFront + Plan.Islands * IslandSpacing + ShopGap + ShopDepth / 2.0);
	Plan.bShop = !IsBlocked(Shop);
	const FWorldPoint Centre = Frame.ToWorld(0.0, 20.0);
	AddStation(Frame, Plan, BrandVariant(Node.Tags, Node.Position.X, Node.Position.Y),
			   static_cast<float>(Ground(Centre.X, Centre.Y)));
	StationCentres.push_back(Centre);
}

// ---------------------------------------------------------------------------------------------------- greenhouses

FGlasshouseStyle FSiteBuilder::ReadStyle(const std::string& Name) const
{
	const std::string Key = "greenhouses." + Name;
	FGlasshouseStyle Style;
	Style.Name = Name;
	Style.Span = Specs.Number(Key + ".span", Style.Span);
	Style.Bay = Specs.Number(Key + ".bay", Style.Bay);
	Style.GutterHeight = Specs.Number(Key + ".gutter_height", Style.GutterHeight);
	return Style;
}

void FSiteBuilder::PlaceGreenhouses()
{
	for (const FOsmArea& Building : Osm.Buildings)
	{
		if (HasTagValue(Building.Tags, "building", "greenhouse"))
		{
			PlaceGreenhouse(Building);
		}
	}
}

void FSiteBuilder::AddGlasshouse(const FPlacementFrame& Frame, const FGlasshouseStyle& Style, int Spans, int Bays, float Z)
{
	// greenhouses.py's glasshouse_parts, in the house frame with the front gable on v = 0 and the width centred on u = 0.
	const double Width = Spans * Style.Span;
	const double Length = Bays * Style.Bay;
	const std::string Prefix = "greenhouses_" + Style.Name + "_";
	const auto Place = [&](const char* Piece, double X, double Y, double Height, double Yaw) {
		Add(Frame.Piece(Prefix + Piece, X - Width / 2.0, Y, Z + static_cast<float>(Height), Yaw), false);
	};
	for (int Span = 0; Span < Spans; ++Span)
	{
		Place(Span == Spans / 2 ? "GableDoor" : "Gable", Span * Style.Span, 0.0, 0.0, 0.0);
		Place("Gable", Width - Span * Style.Span, Length, 0.0, 180.0);
	}
	for (int Bay = 0; Bay < Bays; ++Bay)
	{
		Place("Side", 0.0, Length - Bay * Style.Bay, 0.0, -90.0);
		Place("Side", Width, Bay * Style.Bay, 0.0, 90.0);
	}
	for (int Span = 0; Span <= Spans; ++Span)
	{
		Place("Post", Span * Style.Span, 0.0, 0.0, 0.0);
		Place("Post", Span * Style.Span, Length, 0.0, 0.0);
	}
	for (int Bay = 1; Bay < Bays; ++Bay)
	{
		Place("Post", 0.0, Bay * Style.Bay, 0.0, 0.0);
		Place("Post", Width, Bay * Style.Bay, 0.0, 0.0);
	}
	for (int Span = 0; Span <= Spans; ++Span)
	{
		for (int Bay = 0; Bay < Bays; ++Bay)
		{
			Place("Gutter", Span * Style.Span, Bay * Style.Bay, Style.GutterHeight, 0.0);
		}
	}
	for (int Span = 0; Span < Spans; ++Span)
	{
		for (int Bay = 0; Bay < Bays; ++Bay)
		{
			Place((Span + Bay) % 2 == 0 ? "RoofVent" : "Roof", Span * Style.Span, Bay * Style.Bay, Style.GutterHeight, 0.0);
		}
	}
	for (int Bay = 1; Bay < Bays; ++Bay)
	{
		for (int Span = 1; Span < Spans; ++Span)
		{
			Place("Column", Span * Style.Span, Bay * Style.Bay, 0.0, 0.0);
		}
		for (int Span = 0; Span < Spans; ++Span)
		{
			Place("Truss", Span * Style.Span, Bay * Style.Bay, Style.GutterHeight, 0.0);
		}
	}
}

void FSiteBuilder::AddTunnel(const FPlacementFrame& Frame, int Sections, float Z)
{
	// greenhouses.py's tunnel_parts: an end wall at each end and a section every Section metres.
	const double Width = Specs.Number("greenhouses.Tunnel.width", 8.0);
	const double Section = Specs.Number("greenhouses.Tunnel.section", 2.0);
	const double Length = Sections * Section;
	Add(Frame.Piece("greenhouses_Tunnel_End", -Width / 2.0, 0.0, Z, 0.0), false);
	Add(Frame.Piece("greenhouses_Tunnel_End", Width / 2.0, Length, Z, 180.0), false);
	for (int Index = 0; Index < Sections; ++Index)
	{
		Add(Frame.Piece("greenhouses_Tunnel_Section", -Width / 2.0, Index * Section, Z, 0.0), false);
	}
}

void FSiteBuilder::PlaceGreenhouse(const FOsmArea& Building)
{
	if (Building.Polygons.size() != 1 || Building.Polygons[0].Rings.empty())
	{
		return;
	}
	const FOsmPolygon& Polygon = Building.Polygons[0];
	FOrientedRectangle Rectangle = MinimumAreaRectangle(Polygon.Rings[0]);
	const double Area = std::abs(PolygonArea({Polygon.Rings}));
	if (Rectangle.Area() <= 0.0 || Area / Rectangle.Area() < GreenhouseRectangleFill || Area < MinimumGreenhouseArea)
	{
		return;
	}
	// The long side is the first axis: the ridges run along it and the spans stand across.
	if (Rectangle.HalfFirst < Rectangle.HalfSecond)
	{
		std::swap(Rectangle.HalfFirst, Rectangle.HalfSecond);
		Rectangle.Axis = {-Rectangle.Axis.Y, Rectangle.Axis.X};
	}
	const double Length = 2.0 * Rectangle.HalfFirst;
	const double Width = 2.0 * Rectangle.HalfSecond;
	if (Width < MinimumGreenhouseWidth)
	{
		return;
	}
	const float Z = static_cast<float>(Ground(Rectangle.Centre.X, Rectangle.Centre.Y));
	const bool bTunnel = Width <= TunnelMaxWidth && Length / Width >= TunnelMinAspect;
	const FGlasshouseStyle Style = ReadStyle(Area < OldGlasshouseMaxArea ? "VenloOld" : "Venlo");
	const double Module = bTunnel ? Specs.Number("greenhouses.Tunnel.section", 2.0) : Style.Bay;
	const int Bays = std::max(1, static_cast<int>(std::lround(Length / Module)));
	const double HouseLength = Bays * Module;
	// The frame's origin is the middle of the front gable, v running along the ridges.
	const FWorldPoint Front = Rectangle.Point(-HouseLength / 2.0, 0.0);
	const FPlacementFrame Frame(Front.X, Front.Y, Rectangle.Axis.X, Rectangle.Axis.Y);
	if (bTunnel)
	{
		AddTunnel(Frame, Bays, Z);
	}
	else
	{
		const int Spans = std::max(1, static_cast<int>(std::lround(Width / Style.Span)));
		AddGlasshouse(Frame, Style, Spans, Bays, Z);
	}
	++Totals.Greenhouses;
	AppendRings(Building, Result.Replaced);
}
}

FSiteStructures BuildSiteStructures(const FOsmData& Osm, const FRoadGraph& Graph, const FPolygonSet& RoadGround,
									const FPolygonSet& Buildings, const FKitSpecs& Specs, const FGroundSampler& Ground,
									FSiteCounts* Counts)
{
	FSiteBuilder Builder(Osm, Graph, RoadGround, Buildings, Specs, Ground);
	return Builder.Build(Counts);
}
}
