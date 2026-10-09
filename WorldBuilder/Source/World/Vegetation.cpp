#include "Vegetation.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include "Paths.h"
#include "Projection.h"
#include "PolygonQuery.h"

namespace WorldBuilder
{
namespace
{
const std::set<std::string> ConiferGenera = {"Kiefer", "Fichte", "Tanne", "Lärche", "Douglasie", "Eibe", "Lebensbaum",
											 "Scheinzypresse", "Zeder", "Hemlocktanne", "Mammutbaum", "Sicheltanne",
											 "Wacholder"};
const std::unordered_map<std::string, double> SlenderGenera = {
	{"Pappel", 2.2}, {"Birke", 1.9}, {"Erle", 1.8}, {"Hainbuche", 1.4}, {"Säulen-Hainbuche", 2.5},
	{"Esche", 1.5}, {"Robinie, Scheinakazie", 1.6}, {"Weide", 1.3}};
const std::set<std::string> SmallGenera = {"Steinobst", "Apfelbaum", "Weissdorn", "Vogelbeere", "Mehlbeere", "Birne",
										   "Hasel", "Zierkirsche", "Magnolie", "Felsenbirne", "Amberbaum"};
/** Mature linden, oak and maple street trees: about 20 m tall at a 15 m crown. */
constexpr double DefaultHeightRatio = 1.3;

/** Street trees whose trunk lands on the carriageway are pushed this far off it. */
constexpr double KeepOffRoad = 0.4;
/** No trunk within this distance of a pavement, register street trees included. */
constexpr double PavementTrunkClearance = 0.5;
/** Every trunk stays this far from facades. */
constexpr double BuildingTrunkClearance = 1.0;
/** A crown may reach this far past the nearest facade. */
constexpr double CrownFacadeOverlap = 0.8;
/** Trees whose crown would shrink below this next to a building are dropped. */
constexpr double MinimumClampedCrown = 2.5;
/** A trunk keeps this fraction of a neighbour's crown radius clear. */
constexpr double CrownOverlapFactor = 0.6;
/** Register crowns are clipped to 25 m, so no crown reaches further than this from its trunk. */
constexpr double MaximumCrownRadius = 12.5;
/** Procedural plants keep this far from roads, pavements, buildings and water, and from paths. */
constexpr double HardClearance = 1.5;
constexpr double PathClearance = 0.8;
/** The understorey band along a wood's edge, metres. */
constexpr double WoodEdgeWidth = 6.0;
/** Facade distances beyond this make no difference to a crown, metres. */
constexpr double FacadeSearchLimit = 20.0;
/** How far the nearest road or building edge is looked for when a register tree is moved off it. */
constexpr double OutlineSearchLimit = 50.0;
/** Plants interact over a crown radius plus gaps; the tile's window reaches this far beyond it. */
constexpr double PlantingMargin = 16.0;

/** Small deterministic random numbers (SplitMix64), seeded from a feature's identity. */
class FRandom
{
public:
	FRandom(uint64_t Kind, uint64_t Id, uint64_t Extra = 0) : State(Mix(Mix(Kind * 0x9E3779B97F4A7C15ull ^ Id) ^ Extra)) {}

	double Next01()
	{
		State += 0x9E3779B97F4A7C15ull;
		return static_cast<double>(Mix(State) >> 11) * (1.0 / 9007199254740992.0);
	}

	double Uniform(double Low, double High) { return Low + (High - Low) * Next01(); }

private:
	uint64_t State;

	static uint64_t Mix(uint64_t Value)
	{
		Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ull;
		Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBull;
		return Value ^ (Value >> 31);
	}
};

/** One seed from two numbers. */
uint64_t Mix2(uint64_t First, uint64_t Second)
{
	FRandom Random(First, Second);
	return static_cast<uint64_t>(Random.Next01() * 9007199254740992.0);
}

enum ESeedKind : uint64_t
{
	SeedStreetTree = 1,
	SeedOsmTree = 2,
	SeedTreeRow = 3,
	SeedHedge = 4,
	SeedWood = 5,
	SeedWoodEdge = 6,
	SeedScrub = 7,
	SeedPark = 8,
};

struct FTreeShape
{
	const char* Model;
	double Crown;
	double Height;
	double Trunk;
};

/** Model and size of a register tree from its genus, crown and girth. */
FTreeShape StreetTreeShape(const FStreetTree& Tree, FRandom& Random)
{
	double Crown = Tree.Crown != 0.0 ? Tree.Crown : Random.Uniform(4.0, 8.0);
	Crown = std::clamp(Crown, 1.5, 25.0);
	const double Trunk = Tree.Girth != 0.0 ? std::clamp(Tree.Girth / 100.0 / std::numbers::pi, 0.08, 1.5)
										   : std::max(0.1, Crown * 0.04);
	if (ConiferGenera.count(Tree.Genus))
	{
		return {"conifer", Crown, std::clamp(Crown * 2.8, 3.0, 30.0), Trunk};
	}
	if (SmallGenera.count(Tree.Genus))
	{
		return {"broadleaf", Crown, std::clamp(Crown * 1.2, 3.0, 12.0), Trunk};
	}
	const auto Slender = SlenderGenera.find(Tree.Genus);
	const double Ratio = Slender != SlenderGenera.end() ? Slender->second : DefaultHeightRatio;
	return {"broadleaf", Crown, std::clamp(Crown * Ratio * Random.Uniform(0.9, 1.1), 3.0, 32.0), Trunk};
}

/** Model and size of an OSM tree from its tags. */
FTreeShape OsmTreeShape(const FTags& Tags, FRandom& Random)
{
	const double TaggedCrown = TagNumber(FindTag(Tags, "diameter_crown"));
	const double Crown = TaggedCrown != 0.0 ? TaggedCrown : Random.Uniform(5.0, 10.0);
	const double Height = TagNumber(FindTag(Tags, "height"));
	const double Girth = TagNumber(FindTag(Tags, "circumference"));
	const double Trunk = Girth != 0.0 ? std::clamp(Girth / std::numbers::pi, 0.08, 1.5) : std::max(0.12, Crown * 0.04);
	const std::string* Leaf = FindTag(Tags, "leaf_type");
	if (Leaf != nullptr && *Leaf == "needleleaved")
	{
		return {"conifer", Crown, Height != 0.0 ? Height : Crown * 2.8, Trunk};
	}
	return {"broadleaf", Crown, Height != 0.0 ? Height : Crown * DefaultHeightRatio * Random.Uniform(0.9, 1.15), Trunk};
}

/** Evenly spaced points along a line, each jittered along it. */
std::vector<FWorldPoint> LinePoints(const FPolyline& Line, double Spacing, double Jitter, FRandom& Random)
{
	const double Length = PolylineLength(Line);
	if (Length < 1e-3)
	{
		return {};
	}
	const int Count = std::max(1, static_cast<int>(Length / Spacing));
	std::vector<double> Distances(Count);
	for (int Index = 0; Index < Count; ++Index)
	{
		Distances[Index] = (Index + 0.5) * Length / Count + Random.Uniform(-Jitter, Jitter) * Spacing;
	}
	std::vector<FWorldPoint> Points;
	for (const double Distance : Distances)
	{
		double Remaining = std::clamp(Distance, 0.0, Length);
		for (size_t Segment = 0; Segment + 1 < Line.size(); ++Segment)
		{
			const double SegmentLength = std::hypot(Line[Segment + 1].X - Line[Segment].X, Line[Segment + 1].Y - Line[Segment].Y);
			if (Remaining <= SegmentLength || Segment + 2 == Line.size())
			{
				const double Along = SegmentLength > 0.0 ? std::min(Remaining / SegmentLength, 1.0) : 0.0;
				Points.push_back({Line[Segment].X + (Line[Segment + 1].X - Line[Segment].X) * Along,
								  Line[Segment].Y + (Line[Segment + 1].Y - Line[Segment].Y) * Along});
				break;
			}
			Remaining -= SegmentLength;
		}
	}
	return Points;
}

FBox BoxAround(const FPolyline& Line)
{
	FBox Box{1e300, 1e300, -1e300, -1e300};
	for (const FWorldPoint& Point : Line)
	{
		Box.X0 = std::min(Box.X0, Point.X);
		Box.Y0 = std::min(Box.Y0, Point.Y);
		Box.X1 = std::max(Box.X1, Point.X);
		Box.Y1 = std::max(Box.Y1, Point.Y);
	}
	return Box;
}

/** Rejects new plants too close to existing ones, and trunks under existing crowns (a grid hash). */
class FOccupancy
{
public:
	bool Free(double X, double Y, double Radius) const
	{
		const int Reach = static_cast<int>(std::ceil(Radius / Cell));
		for (const FCrown& Other : Around(Points, X, Y, Reach))
		{
			if ((Other.X - X) * (Other.X - X) + (Other.Y - Y) * (Other.Y - Y) < Radius * Radius)
			{
				return false;
			}
		}
		return true;
	}

	/** True if the trunk is clear of every recorded crown (scaled) and no recorded trunk lies in the new crown. */
	bool CrownFree(double X, double Y, double Radius) const
	{
		const int Reach = static_cast<int>(std::ceil(std::max(Radius, MaximumCrownRadius) / Cell));
		for (const FCrown& Other : Around(Crowns, X, Y, Reach))
		{
			const double Needed = CrownOverlapFactor * std::max(Radius, Other.Radius);
			if ((Other.X - X) * (Other.X - X) + (Other.Y - Y) * (Other.Y - Y) < Needed * Needed)
			{
				return false;
			}
		}
		return true;
	}

	void Add(double X, double Y) { Points[Key(X, Y)].push_back({X, Y, 0.0}); }
	void AddCrown(double X, double Y, double Radius) { Crowns[Key(X, Y)].push_back({X, Y, Radius}); }

private:
	struct FCrown
	{
		double X;
		double Y;
		double Radius;
	};
	static constexpr double Cell = 4.0;
	std::unordered_map<int64_t, std::vector<FCrown>> Points;
	std::unordered_map<int64_t, std::vector<FCrown>> Crowns;

	static int64_t KeyOf(int64_t Column, int64_t Row) { return Row * 4000003 + Column; }
	static int64_t Key(double X, double Y)
	{
		return KeyOf(static_cast<int64_t>(std::floor(X / Cell)), static_cast<int64_t>(std::floor(Y / Cell)));
	}

	static std::vector<FCrown> Around(const std::unordered_map<int64_t, std::vector<FCrown>>& Grid, double X, double Y,
									  int Reach)
	{
		std::vector<FCrown> Found;
		const int64_t CentreColumn = static_cast<int64_t>(std::floor(X / Cell));
		const int64_t CentreRow = static_cast<int64_t>(std::floor(Y / Cell));
		for (int64_t Row = CentreRow - Reach; Row <= CentreRow + Reach; ++Row)
		{
			for (int64_t Column = CentreColumn - Reach; Column <= CentreColumn + Reach; ++Column)
			{
				const auto Bucket = Grid.find(KeyOf(Column, Row));
				if (Bucket != Grid.end())
				{
					Found.insert(Found.end(), Bucket->second.begin(), Bucket->second.end());
				}
			}
		}
		return Found;
	}
};

/** The placement of one tile: what plants avoid, the occupancy and the plants placed so far. */
class FPlanting
{
public:
	FPlanting(const FVegetationSources& InSources, const FBox& InWindow, const FBox& InExtent,
			  const FVegetationObstacles& Obstacles)
		: Sources(InSources), Window(InWindow), Extent(InExtent), Road(Obstacles.RoadGround),
		  Pavement(Obstacles.Pavement), Buildings(Obstacles.Buildings), Water(Obstacles.Water), PathArea(Obstacles.Paths)
	{
	}

	std::vector<FPlant> Plants;

	void PlaceStreetTrees();
	void PlaceOsmTrees();
	void PlaceTreeRowsAndHedges();
	void PlaceAreas();

private:
	/** What a scatter places: wood trees, the understorey at a wood's edge, scrub or park trees. */
	enum class EScatter
	{
		Wood,
		WoodEdge,
		Scrub,
		Park,
	};

	const FVegetationSources& Sources;
	FBox Window;
	FBox Extent;
	FPolygonQuery Road;
	FPolygonQuery Pavement;
	FPolygonQuery Buildings;
	FPolygonQuery Water;
	FPolygonQuery PathArea;
	FOccupancy Occupancy;

	bool InWindow(double X, double Y) const { return Window.X0 <= X && X < Window.X1 && Window.Y0 <= Y && Y < Window.Y1; }

	/** Distance to roads, pavements, buildings and water (0 inside), at most Limit. */
	double HardDistance(double X, double Y, double Limit) const
	{
		double Distance = Limit;
		for (const FPolygonQuery* Layer : {&Road, &Pavement, &Buildings, &Water})
		{
			Distance = std::min(Distance, Layer->Distance(X, Y, Distance));
		}
		return Distance;
	}

	bool InHard(double X, double Y) const { return HardDistance(X, Y, 1e-9) <= 0.0; }

	/** Where procedural plants may not go: near hard surfaces or paths. */
	bool Blocked(double X, double Y) const
	{
		return HardDistance(X, Y, HardClearance) < HardClearance || PathArea.Distance(X, Y, PathClearance) < PathClearance;
	}

	/** How far a free point is from the blocked area around hard surfaces and paths, at most Limit. */
	double DistanceToBlocked(double X, double Y, double Limit) const
	{
		const double FromHard = HardDistance(X, Y, Limit + HardClearance) - HardClearance;
		const double FromPaths = PathArea.Distance(X, Y, Limit + PathClearance) - PathClearance;
		return std::max(0.0, std::min({Limit, FromHard, FromPaths}));
	}

	double FacadeDistance(double X, double Y) const { return Buildings.Distance(X, Y, FacadeSearchLimit); }

	/** vegetation.py's add: the placement rules every plant goes through. */
	bool Add(const char* Model, double X, double Y, double Crown, double Height, double Trunk, const char* Source,
			 double MinimumGap)
	{
		const bool bInExtent = Extent.X0 <= X && X < Extent.X1 && Extent.Y0 <= Y && Y < Extent.Y1;
		if (!bInExtent || !InWindow(X, Y) || Pavement.Distance(X, Y, PavementTrunkClearance) < PavementTrunkClearance)
		{
			return false;
		}
		if (!Occupancy.Free(X, Y, MinimumGap))
		{
			return false;
		}
		const bool bTree = std::string(Model) != "shrub";
		const bool bRegister = std::string(Source) == "kataster";
		if (bTree)
		{
			if (!bRegister && (FacadeDistance(X, Y) < BuildingTrunkClearance || !Occupancy.CrownFree(X, Y, Crown / 2.0)))
			{
				return false;
			}
			FitCrownToFacades(X, Y, Crown, Height);
			if (Crown == 0.0 && !bRegister)
			{
				return false;
			}
			if (Crown == 0.0)
			{
				Crown = MinimumClampedCrown;
			}
			Occupancy.AddCrown(X, Y, Crown / 2.0);
		}
		Occupancy.Add(X, Y);
		Plants.push_back({Model, X, Y, Crown, Height, Trunk, Source});
		return true;
	}

	/** Shrinks a crown that would reach through a facade; crown 0 when too little is left. */
	void FitCrownToFacades(double X, double Y, double& Crown, double& Height) const
	{
		const double Allowed = 2.0 * (FacadeDistance(X, Y) + CrownFacadeOverlap);
		if (Crown <= Allowed)
		{
			return;
		}
		if (Allowed < MinimumClampedCrown)
		{
			Crown = 0.0;
			return;
		}
		Height *= std::sqrt(Allowed / Crown);
		Crown = Allowed;
	}

	/** A register position corrected for its error of about a metre: off the carriageway and out of buildings. */
	FWorldPoint StreetTreePosition(double X, double Y) const
	{
		const double OriginalX = X;
		const double OriginalY = Y;
		if (Road.Contains(X, Y))
		{
			if (const std::optional<FWorldPoint> Edge = Road.NearestOutlinePoint(X, Y, OutlineSearchLimit))
			{
				const FWorldPoint Moved = MovedBeyond(*Edge, {X, Y}, KeepOffRoad, true);
				X = Moved.X;
				Y = Moved.Y;
			}
		}
		if (FacadeDistance(X, Y) >= BuildingTrunkClearance)
		{
			return {X, Y};
		}
		// As vegetation.py, measured from the register position.
		const std::optional<FWorldPoint> Edge = Buildings.NearestOutlinePoint(OriginalX, OriginalY, OutlineSearchLimit);
		if (!Edge)
		{
			return {X, Y};
		}
		return MovedBeyond(*Edge, {OriginalX, OriginalY}, BuildingTrunkClearance, Buildings.Contains(OriginalX, OriginalY));
	}

	/** A point Distance from an edge point, away from (or, with bTowards, past it from) the reference point. */
	static FWorldPoint MovedBeyond(const FWorldPoint& Edge, const FWorldPoint& Reference, double Distance, bool bTowards)
	{
		double DirectionX = bTowards ? Edge.X - Reference.X : Reference.X - Edge.X;
		double DirectionY = bTowards ? Edge.Y - Reference.Y : Reference.Y - Edge.Y;
		const double Length = std::hypot(DirectionX, DirectionY);
		if (Length < 1e-6)
		{
			return Reference;
		}
		DirectionX /= Length;
		DirectionY /= Length;
		return {Edge.X + DirectionX * Distance, Edge.Y + DirectionY * Distance};
	}

	void PlaceInArea(int AreaIndex, const FPolygons& Clipped);
	void Scatter(EScatter Kind, const FPolygonQuery& Area, const FBox& Bounds, double Spacing, double Keep, uint64_t Seed,
				 const std::string& Leaf);
	void PlaceScattered(EScatter Kind, double X, double Y, FRandom& Random, const std::string& Leaf);
};

void FPlanting::PlaceStreetTrees()
{
	for (const int Item : Sources.TreeIndex().Query(Window))
	{
		const FStreetTree& Tree = Sources.Trees()[Item];
		if (!(Extent.X0 <= Tree.X && Tree.X < Extent.X1 && Extent.Y0 <= Tree.Y && Tree.Y < Extent.Y1))
		{
			continue;
		}
		const FWorldPoint Position = StreetTreePosition(Tree.X, Tree.Y);
		FRandom Random(SeedStreetTree, static_cast<uint64_t>(Tree.Id));
		const FTreeShape Shape = StreetTreeShape(Tree, Random);
		Add(Shape.Model, Position.X, Position.Y, Shape.Crown, Shape.Height, Shape.Trunk, "kataster", 0.8);
	}
}

void FPlanting::PlaceOsmTrees()
{
	const FOsmData& Osm = Sources.Osm();
	for (const int Item : Sources.PointIndex().Query(Window))
	{
		const FOsmPoint& Point = Osm.Points[Item];
		if (Road.Contains(Point.Position.X, Point.Position.Y))
		{
			continue;
		}
		FRandom Random(SeedOsmTree, static_cast<uint64_t>(Point.Id));
		const FTreeShape Shape = OsmTreeShape(Point.Tags, Random);
		Add(Shape.Model, Point.Position.X, Point.Position.Y, Shape.Crown, Shape.Height, Shape.Trunk, "osm_tree", 3.0);
	}
}

void FPlanting::PlaceTreeRowsAndHedges()
{
	const FOsmData& Osm = Sources.Osm();
	for (const int Item : Sources.RowIndex().Query(Window))
	{
		const FOsmWay& Row = Osm.TreeRows[Item];
		FRandom Random(SeedTreeRow, static_cast<uint64_t>(Row.Id));
		for (const FWorldPoint& Point : LinePoints(Row.Points, 9.0, 0.15, Random))
		{
			// Every point draws its shape, so a point's tree doesn't depend on which others are blocked.
			const FTreeShape Shape = OsmTreeShape(Row.Tags, Random);
			if (!Blocked(Point.X, Point.Y))
			{
				Add(Shape.Model, Point.X, Point.Y, Shape.Crown, Shape.Height, Shape.Trunk, "tree_row", 4.0);
			}
		}
	}
	for (const int Item : Sources.HedgeIndex().Query(Window))
	{
		const FOsmWay& Hedge = Osm.Hedges[Item];
		FRandom Random(SeedHedge, static_cast<uint64_t>(Hedge.Id));
		const double TaggedHeight = TagNumber(FindTag(Hedge.Tags, "height"));
		for (const FWorldPoint& Point : LinePoints(Hedge.Points, 1.6, 0.2, Random))
		{
			const double Height = TaggedHeight != 0.0 ? TaggedHeight : Random.Uniform(1.4, 2.2);
			const double Crown = Random.Uniform(1.6, 2.2);
			if (!InHard(Point.X, Point.Y))
			{
				Add("shrub", Point.X, Point.Y, Crown, Height, 0.0, "hedge", 0.9);
			}
		}
	}
}

void FPlanting::PlaceAreas()
{
	for (const auto& [AreaIndex, Polygon] : Sources.PlantedAreas().InWindow(Window))
	{
		PlaceInArea(AreaIndex, Polygon);
	}
}

void FPlanting::PlaceInArea(int AreaIndex, const FPolygons& Clipped)
{
	if (Clipped.empty())
	{
		return;
	}
	const FPolygonQuery Area(Clipped);
	const FBox Bounds = BoundsOf(Clipped);
	const FOsmArea& Osm = Sources.Osm().Areas[AreaIndex];
	const std::string* Landuse = FindTag(Osm.Tags, "landuse");
	const std::string* Natural = FindTag(Osm.Tags, "natural");
	const std::string* Leisure = FindTag(Osm.Tags, "leisure");
	const bool bWood = (Landuse != nullptr && *Landuse == "forest") || (Natural != nullptr && *Natural == "wood");
	const uint64_t AreaId = static_cast<uint64_t>(Osm.Id) * 2 + (Osm.bFromRelation ? 1 : 0);
	const std::string* LeafTag = FindTag(Osm.Tags, "leaf_type");
	const std::string Leaf = LeafTag != nullptr ? *LeafTag : "mixed";
	if (bWood)
	{
		Scatter(EScatter::Wood, Area, Bounds, 6.5, 1.0, Mix2(SeedWood, AreaId), Leaf);
		Scatter(EScatter::WoodEdge, Area, Bounds, 3.5, 0.6, Mix2(SeedWoodEdge, AreaId), Leaf);
		return;
	}
	if (Natural != nullptr && *Natural == "scrub")
	{
		Scatter(EScatter::Scrub, Area, Bounds, 3.0, 0.7, Mix2(SeedScrub, AreaId), Leaf);
		return;
	}
	const bool bPark = (Leisure != nullptr && *Leisure == "park")
		|| (Landuse != nullptr && (*Landuse == "cemetery" || *Landuse == "village_green"));
	if (bPark)
	{
		Scatter(EScatter::Park, Area, Bounds, 16.0, 0.45, Mix2(SeedPark, AreaId), Leaf);
	}
}

void FPlanting::Scatter(EScatter Kind, const FPolygonQuery& Area, const FBox& Bounds, double Spacing, double Keep,
						uint64_t Seed, const std::string& Leaf)
{
	const int64_t FirstColumn = static_cast<int64_t>(std::floor(Bounds.X0 / Spacing)) - 1;
	const int64_t LastColumn = static_cast<int64_t>(std::floor(Bounds.X1 / Spacing)) + 1;
	const int64_t FirstRow = static_cast<int64_t>(std::floor(Bounds.Y0 / Spacing)) - 1;
	const int64_t LastRow = static_cast<int64_t>(std::floor(Bounds.Y1 / Spacing)) + 1;
	for (int64_t Row = FirstRow; Row <= LastRow; ++Row)
	{
		for (int64_t Column = FirstColumn; Column <= LastColumn; ++Column)
		{
			// The lattice is fixed to the world, so every tile draws the same jitter for the same point.
			FRandom Random(Seed, static_cast<uint64_t>(Row) * 0x100000001ull + static_cast<uint64_t>(Column));
			const double X = (Column + Random.Uniform(-0.45, 0.45)) * Spacing;
			const double Y = (Row + Random.Uniform(-0.45, 0.45)) * Spacing;
			if (Keep < 1.0 && Random.Next01() >= Keep)
			{
				continue;
			}
			// Free: inside the area and clear of everything plants avoid.
			if (!InWindow(X, Y) || !Area.Contains(X, Y) || Blocked(X, Y))
			{
				continue;
			}
			// The understorey grows in the band along the free area's border.
			if (Kind == EScatter::WoodEdge
				&& std::min(Area.DistanceToOutline(X, Y, WoodEdgeWidth), DistanceToBlocked(X, Y, WoodEdgeWidth)) >= WoodEdgeWidth)
			{
				continue;
			}
			PlaceScattered(Kind, X, Y, Random, Leaf);
		}
	}
}

void FPlanting::PlaceScattered(EScatter Kind, double X, double Y, FRandom& Random, const std::string& Leaf)
{
	switch (Kind)
	{
	case EScatter::Wood:
	{
		const bool bConifer = Leaf == "needleleaved" || (Leaf == "mixed" && Random.Next01() < 0.35);
		const double Crown = Random.Uniform(5.0, 9.0);
		if (bConifer)
		{
			Add("conifer", X, Y, Crown * 0.8, Random.Uniform(16.0, 26.0), Crown * 0.05, "wood", 3.5);
			return;
		}
		Add("broadleaf", X, Y, Crown, Crown * Random.Uniform(1.8, 2.4), Crown * 0.045, "wood", 3.5);
		return;
	}
	case EScatter::WoodEdge:
		Add("shrub", X, Y, Random.Uniform(1.5, 3.0), Random.Uniform(1.2, 2.5), 0.0, "wood_edge", 1.5);
		return;
	case EScatter::Scrub:
		Add("shrub", X, Y, Random.Uniform(1.5, 3.5), Random.Uniform(1.0, 2.5), 0.0, "scrub", 1.5);
		return;
	case EScatter::Park:
	{
		FTreeShape Shape = OsmTreeShape({}, Random);
		// Park trees grow freely.
		Shape.Crown *= Random.Uniform(1.0, 1.4);
		Add(Shape.Model, X, Y, Shape.Crown, Shape.Crown * DefaultHeightRatio, Shape.Trunk, "park", 6.0);
		return;
	}
	}
}

bool IsPlantedArea(const FTags& Tags)
{
	const std::string* Landuse = FindTag(Tags, "landuse");
	const std::string* Natural = FindTag(Tags, "natural");
	const std::string* Leisure = FindTag(Tags, "leisure");
	return (Landuse != nullptr && (*Landuse == "forest" || *Landuse == "cemetery" || *Landuse == "village_green"))
		|| (Natural != nullptr && (*Natural == "wood" || *Natural == "scrub")) || (Leisure != nullptr && *Leisure == "park");
}
}

std::vector<FStreetTree> ReadStreetTrees(const std::string& Path)
{
	std::vector<FStreetTree> Trees;
	std::ifstream Input(Path);
	std::string Line;
	while (std::getline(Input, Line))
	{
		std::istringstream Fields(Line);
		std::string Id, East, North, Genus, Crown, Girth;
		std::getline(Fields, Id, '\t');
		std::getline(Fields, East, '\t');
		std::getline(Fields, North, '\t');
		std::getline(Fields, Genus, '\t');
		std::getline(Fields, Crown, '\t');
		std::getline(Fields, Girth, '\t');
		FStreetTree& Tree = Trees.emplace_back();
		Tree.Id = std::stoll(Id);
		Tree.X = std::stod(East) - OriginEast;
		Tree.Y = OriginNorth - std::stod(North);
		Tree.Genus = Genus;
		Tree.Crown = std::stod(Crown);
		Tree.Girth = std::stod(Girth);
	}
	return Trees;
}

FVegetationSources::FVegetationSources(const FOsmData& Osm, std::vector<FStreetTree> InStreetTrees)
	: OsmData(&Osm), StreetTrees(std::move(InStreetTrees))
{
	for (size_t Item = 0; Item < StreetTrees.size(); ++Item)
	{
		const FStreetTree& Tree = StreetTrees[Item];
		StreetTreeIndex.Insert({Tree.X, Tree.Y, Tree.X, Tree.Y}, static_cast<int>(Item));
	}
	for (size_t Item = 0; Item < Osm.Points.size(); ++Item)
	{
		const FOsmPoint& Point = Osm.Points[Item];
		const std::string* Natural = FindTag(Point.Tags, "natural");
		if (Natural != nullptr && *Natural == "tree")
		{
			TreePointIndex.Insert({Point.Position.X, Point.Position.Y, Point.Position.X, Point.Position.Y}, static_cast<int>(Item));
		}
	}
	for (size_t Item = 0; Item < Osm.TreeRows.size(); ++Item)
	{
		TreeRowIndex.Insert(BoxAround(Osm.TreeRows[Item].Points), static_cast<int>(Item));
	}
	for (size_t Item = 0; Item < Osm.Hedges.size(); ++Item)
	{
		HedgeLineIndex.Insert(BoxAround(Osm.Hedges[Item].Points), static_cast<int>(Item));
	}
	for (size_t Item = 0; Item < Osm.Areas.size(); ++Item)
	{
		const FOsmArea& Area = Osm.Areas[Item];
		if (!IsPlantedArea(Area.Tags))
		{
			continue;
		}
		std::vector<FPolygonWithHoles> Polygons;
		for (const FOsmPolygon& Polygon : Area.Polygons)
		{
			Polygons.push_back({Polygon.Rings});
		}
		PlantedAreaPieces.Add(static_cast<int>(Item), Clipper2Lib::Union(ToPaths(Polygons), Clipper2Lib::FillRule::EvenOdd, 4));
	}
}

double FVegetationSources::WindowMargin()
{
	return PlantingMargin;
}

std::vector<FPlant> FVegetationSources::PlantsInTile(const FBox& Tile, const FBox& Extent,
													 const FVegetationObstacles& Obstacles) const
{
	FPlanting Planting(*this, Obstacles.Window, Extent, Obstacles);
	Planting.PlaceStreetTrees();
	Planting.PlaceOsmTrees();
	Planting.PlaceTreeRowsAndHedges();
	Planting.PlaceAreas();
	std::vector<FPlant> InTile;
	for (const FPlant& Plant : Planting.Plants)
	{
		if (Tile.X0 <= Plant.X && Plant.X < Tile.X1 && Tile.Y0 <= Plant.Y && Plant.Y < Tile.Y1)
		{
			InTile.push_back(Plant);
		}
	}
	return InTile;
}
}
