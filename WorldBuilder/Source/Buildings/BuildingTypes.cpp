#include "ExactFloatingPoint.h"
#include "BuildingTypes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "Buildings.h"
#include "ParallelFor.h"

namespace WorldBuilder
{
namespace
{
using FSet = std::set<std::string, std::less<>>;

// Tag groups of the typology.
const FSet OutbuildingTags = {"garage", "garages", "carport", "shed", "hut", "roof", "greenhouse", "allotment_house",
	"cabin", "service", "kiosk", "container", "toilets", "storage_tank", "gazebo", "pavilion", "boathouse", "ruins",
	"construction", "digester", "silo", "tank"};
const FSet PublicTags = {"school", "kindergarten", "university", "college", "hospital", "public", "civic", "government",
	"church", "chapel", "cathedral", "mosque", "synagogue", "temple", "train_station", "fire_station", "transportation",
	"stadium", "sports_hall", "sports_centre", "museum", "theatre", "townhall", "clinic", "religious", "monastery",
	"bridge", "castle", "police", "prison", "grandstand"};
const FSet PublicAmenities = {"school", "kindergarten", "university", "college", "hospital", "place_of_worship",
	"townhall", "fire_station", "police", "library", "theatre", "community_centre", "courthouse", "clinic",
	"social_facility", "arts_centre", "museum"};
const FSet IndustrialTags = {"industrial", "warehouse", "manufacture", "factory", "hangar", "depot", "storage", "logistics"};
const FSet CommercialTags = {"retail", "commercial", "office", "supermarket", "kiosk", "mall", "hotel"};
const FSet FarmTags = {"farm", "barn", "farm_auxiliary", "stable", "cowshed", "sty", "livestock"};
const FSet ResidentialTags = {"house", "detached", "semidetached_house", "terrace", "apartments", "residential", "yes",
	"dormitory", "bungalow", "duplex", "semi", "townhouse", "villa", "block"};
const FSet BigShopValues = {"mall", "supermarket", "department_store", "doityourself", "trade", "wholesale", "furniture",
	"electronics", "garden_centre"};
const FSet ShopAmenities = {"restaurant", "cafe", "fast_food", "bank", "pharmacy", "bar", "pub", "post_office", "dentist",
	"doctors", "ice_cream", "biergarten", "marketplace", "bureau_de_change"};

/** OSM roof:shape values the typology knows, with the shape they stand for. */
const std::map<std::string, std::string_view, std::less<>> OsmRoofShapes = {
	{"flat", "flat"}, {"gabled", "gabled"}, {"hipped", "hipped"}, {"half-hipped", "half_hipped"},
	{"mansard", "mansard"}, {"gambrel", "gambrel"}, {"pyramidal", "pyramidal"}, {"skillion", "skillion"},
	{"round", "round"}, {"dome", "round"}, {"saltbox", "gabled"}, {"double_saltbox", "hipped"},
	{"quadruple_saltbox", "hipped"}, {"side_hipped", "half_hipped"}, {"side_half-hipped", "half_hipped"},
	{"gabled_height_moved", "gabled"}, {"hip_and_gable", "half_hipped"}, {"butterfly", "gabled"}, {"sawtooth", "gabled"},
};

struct FPitchRange
{
	double Low;
	double High;
};

/** Nominal roof pitch range in degrees per class. */
const std::map<std::string_view, FPitchRange> ClassPitch = {
	{"gruenderzeit_clinker", {40, 50}}, {"brick_block_1920s", {45, 55}}, {"postwar_plaster", {30, 40}},
	{"terraced", {30, 45}}, {"semidetached", {35, 50}}, {"detached_postwar", {30, 45}}, {"villa", {45, 58}},
	{"commercial_groundfloor", {35, 48}}, {"vierlande_farmhouse", {48, 55}}, {"shed_garage", {18, 28}},
	{"public", {35, 50}}, {"modern", {20, 28}}, {"slab_block", {3, 5}}, {"industrial_hall", {3, 8}},
	{"retail_centre", {3, 5}}, {"halftimbered_town", {45, 55}},
};

struct FStoreyData
{
	double StoreyHeight;
	double GroundHeight;
	double Plinth;
};

/** class -> (storey height m, ground floor height m, plinth above terrain m) */
const std::map<std::string_view, FStoreyData> ClassStorey = {
	{"gruenderzeit_clinker", {3.3, 3.8, 0.9}}, {"brick_block_1920s", {3.0, 3.2, 0.5}}, {"postwar_plaster", {2.75, 2.75, 0.5}},
	{"slab_block", {2.8, 2.8, 0.3}}, {"terraced", {2.75, 2.75, 0.4}}, {"semidetached", {2.8, 2.8, 0.5}},
	{"detached_postwar", {2.75, 2.75, 0.4}}, {"villa", {3.4, 3.6, 0.8}}, {"modern", {2.8, 2.8, 0.2}},
	{"commercial_groundfloor", {3.2, 4.0, 0.1}}, {"vierlande_farmhouse", {2.3, 2.3, 0.5}}, {"shed_garage", {2.8, 2.8, 0.1}},
	{"industrial_hall", {7.0, 7.0, 0.6}}, {"public", {3.5, 3.8, 0.5}},
	{"retail_centre", {4.5, 5.0, 0.2}}, {"halftimbered_town", {2.9, 3.2, 0.5}},
};

using FWeightedNames = std::vector<std::pair<std::string_view, double>>;

/**
 * Shares per class of the roof shapes in Hamburg's LoD2 model (west half of bergedorf_core, buildings classified
 * without optional tags). The old town classes (Gruenderzeit, 1920s, villa, shop houses) are pitched far more often
 * than LoD2 says (street photos of Bergedorf, and the flat roofs LoD2 reports there are mostly roofs behind a parapet
 * that the street never sees), so their flat share is cut to what the photos show.
 */
const std::map<std::string_view, FWeightedNames> RoofPriors = {
	{"gruenderzeit_clinker", {{"flat", 22}, {"gabled", 34}, {"hipped", 26}, {"mansard", 18}}},
	{"brick_block_1920s", {{"flat", 28}, {"hipped", 42}, {"gabled", 30}}},
	{"postwar_plaster", {{"flat", 55}, {"gabled", 27}, {"hipped", 14}, {"skillion", 4}}},
	{"slab_block", {{"flat", 45}, {"gabled", 40}, {"hipped", 15}}},
	{"terraced", {{"flat", 45}, {"gabled", 40}, {"hipped", 10}, {"skillion", 5}}},
	{"semidetached", {{"gabled", 60}, {"flat", 22}, {"hipped", 10}, {"skillion", 8}}},
	{"detached_postwar", {{"gabled", 49}, {"flat", 29}, {"hipped", 15}, {"skillion", 5}, {"pyramidal", 2}}},
	{"villa", {{"flat", 8}, {"hipped", 45}, {"gabled", 25}, {"half_hipped", 14}, {"mansard", 8}}},
	{"modern", {{"flat", 48}, {"gabled", 48}, {"skillion", 4}}},
	{"commercial_groundfloor", {{"flat", 35}, {"gabled", 30}, {"hipped", 22}, {"mansard", 9}, {"pyramidal", 4}}},
	{"vierlande_farmhouse", {{"half_hipped", 50}, {"hipped", 35}, {"gabled", 15}}},
	{"shed_garage", {{"flat", 79}, {"gabled", 11}, {"skillion", 7}, {"hipped", 3}}},
	{"industrial_hall", {{"flat", 77}, {"gabled", 19}, {"skillion", 4}}},
	{"public", {{"flat", 70}, {"gabled", 20}, {"hipped", 10}}},
	{"retail_centre", {{"flat", 92}, {"gabled", 8}}},
	{"halftimbered_town", {{"gabled", 70}, {"hipped", 25}, {"half_hipped", 5}}},
};

struct FStoreyPrior
{
	std::vector<std::pair<int, double>> Levels;
	double AtticProbability;
};

/** class -> (storey weights, attic habitable probability): the storey counts of LoD2 per class. */
const std::map<std::string_view, FStoreyPrior> StoreyPriors = {
	{"gruenderzeit_clinker", {{{1, 21}, {2, 27}, {3, 52}, {4, 17}, {5, 7}, {6, 2}}, 0.85}},
	{"brick_block_1920s", {{{2, 13}, {3, 21}, {4, 7}, {5, 7}, {6, 5}}, 0.6}},
	{"postwar_plaster", {{{1, 24}, {2, 41}, {3, 53}, {4, 21}, {5, 11}, {6, 13}}, 0.8}},
	{"slab_block", {{{2, 5}, {3, 17}, {4, 5}, {5, 2}, {6, 3}, {7, 2}}, 0.0}},
	{"terraced", {{{1, 25}, {2, 15}, {3, 3}}, 0.5}},
	{"semidetached", {{{1, 65}, {2, 34}, {3, 6}}, 0.9}},
	{"detached_postwar", {{{1, 172}, {2, 137}, {3, 41}, {4, 8}}, 0.8}},
	{"villa", {{{2, 6}, {3, 3}, {4, 1}}, 0.7}},
	{"modern", {{{2, 3}, {3, 11}, {4, 5}, {5, 2}}, 0.0}},
	{"commercial_groundfloor", {{{1, 20}, {2, 22}, {3, 22}, {4, 14}, {5, 2}}, 0.4}},
	{"vierlande_farmhouse", {{{1, 1}}, 0.5}},
	{"shed_garage", {{{1, 1}}, 0.0}},
	{"industrial_hall", {{{1, 1}}, 0.0}},
	{"public", {{{1, 5}, {2, 9}, {3, 4}, {4, 5}, {5, 3}}, 0.2}},
	{"retail_centre", {{{1, 6}, {2, 3}, {3, 1}}, 0.0}},
	{"halftimbered_town", {{{2, 6}, {3, 3}}, 0.95}},
};

/** Share of flat roofs in LoD2 by footprint area (same sample as RoofPriors): points of (log2 of the area in m2, share). */
const std::vector<std::pair<double, double>> FlatShareByArea = {{3.5, 0.97}, {4.5, 0.90}, {5.5, 0.85}, {6.5, 0.70},
	{7.5, 0.36}, {8.5, 0.48}, {9.5, 0.66}, {10.5, 0.80}, {11.5, 0.88}, {12.5, 0.93}};
constexpr double OverallFlatShare = 0.52;
constexpr double AreaEvidenceWeight = 0.6;
/** The old town classes follow the street photos, so the footprint area moves them less. */
constexpr double TownAreaEvidenceWeight = 0.25;
const FSet TownClasses = {"gruenderzeit_clinker", "brick_block_1920s", "villa", "commercial_groundfloor", "halftimbered_town"};

// Estate detection: this many identical footprints nearby mean a planned estate.
constexpr int EstateRepeats = 6;
constexpr double DensityRadius = 100.0;
constexpr double EstateRadius = 120.0;
constexpr double PartyWallMinLength = 2.0;
constexpr double PartyWallTolerance = 0.35;

/** True when the set has the value. */
bool Contains(const FSet& Set, std::string_view Value)
{
	return Set.find(Value) != Set.end();
}

/** True when the value is one of the options. */
bool IsOneOf(std::string_view Value, std::initializer_list<std::string_view> Options)
{
	return std::find(Options.begin(), Options.end(), Value) != Options.end();
}

/** The tag value, or an empty view when the tag is missing. */
std::string_view TagOrEmpty(const FTags& Tags, const char* Key)
{
	const std::string* Value = FindTag(Tags, Key);
	return Value != nullptr ? std::string_view(*Value) : std::string_view();
}

/** True when the tag is there, whatever its value. */
bool HasTag(const FTags& Tags, const char* Key)
{
	return FindTag(Tags, Key) != nullptr;
}

/** The dot product of two vectors. */
double Dot(const FWorldPoint& A, const FWorldPoint& B)
{
	return A.X * B.X + A.Y * B.Y;
}

/** The vector from B to A. */
FWorldPoint Subtract(const FWorldPoint& A, const FWorldPoint& B)
{
	return {A.X - B.X, A.Y - B.Y};
}

/** The vector scaled to length 1; (1, 0) when it is shorter than 1e-9. */
FWorldPoint Unit(const FWorldPoint& Vector)
{
	const double Length = std::hypot(Vector.X, Vector.Y);
	if (Length < 1e-9)
	{
		return {1.0, 0.0};
	}
	return {Vector.X / Length, Vector.Y / Length};
}

/** Python's x % m for a positive modulus. */
double PositiveModulo(double Value, double Modulus)
{
	double Result = std::fmod(Value, Modulus);
	if (Result < 0.0)
	{
		Result += Modulus;
	}
	return Result;
}

/** Direction of a world vector as a yaw in Unreal's sense (x east, y south, positive turns toward +y). */
double YawDegrees(const FWorldPoint& Vector)
{
	return PositiveModulo(std::atan2(Vector.Y, Vector.X) * 180.0 / std::numbers::pi, 360.0);
}

/** The year in the middle of a century written 'C19' or '~C19' (nothing else around it); none for any other text. */
std::optional<int> ParseCentury(const std::string& Value)
{
	size_t Index = 0;
	while (Index < Value.size() && std::isspace(static_cast<unsigned char>(Value[Index])))
	{
		++Index;
	}
	if (Index < Value.size() && Value[Index] == '~')
	{
		++Index;
	}
	const bool bHasCentury = Index + 2 < Value.size() && Value[Index] == 'C' && std::isdigit(static_cast<unsigned char>(Value[Index + 1]))
		&& std::isdigit(static_cast<unsigned char>(Value[Index + 2]));
	if (!bHasCentury)
	{
		return std::nullopt;
	}
	size_t Tail = Index + 3;
	while (Tail < Value.size() && std::isspace(static_cast<unsigned char>(Value[Tail])))
	{
		++Tail;
	}
	if (Tail != Value.size())
	{
		return std::nullopt;
	}
	const int Century = (Value[Index + 1] - '0') * 10 + (Value[Index + 2] - '0');
	return (Century - 1) * 100 + 50;
}

/** The first run of four digits in the text; none if there is none. */
std::optional<int> ParseFourDigitYear(const std::string& Value)
{
	std::string Digits;
	for (char Character : Value)
	{
		if (!std::isdigit(static_cast<unsigned char>(Character)))
		{
			Digits.clear();
			continue;
		}
		Digits += Character;
		if (Digits.size() == 4)
		{
			return std::stoi(Digits);
		}
	}
	return std::nullopt;
}

/** First four-digit year in a start_date value such as '1898', '1890-1905' or 'C19'; none if there is none. */
std::optional<int> ParseYear(const std::string& Value)
{
	const std::optional<int> Century = ParseCentury(Value);
	if (Century.has_value())
	{
		return Century;
	}
	return ParseFourDigitYear(Value);
}

/** Index of the option a deterministic draw from the weights lands on. */
template <typename FWeightList>
size_t WeightedPickIndex(int64_t OsmId, int Salt, const FWeightList& Options)
{
	double Total = 0.0;
	for (const auto& Option : Options)
	{
		Total += Option.second;
	}
	double Draw = Hash01(OsmId, Salt) * Total;
	for (size_t Index = 0; Index < Options.size(); ++Index)
	{
		Draw -= Options[Index].second;
		if (Draw <= 0.0)
		{
			return Index;
		}
	}
	return Options.size() - 1;
}

/** The name a deterministic draw from the weights lands on. */
std::string_view WeightedPick(int64_t OsmId, int Salt, const FWeightedNames& Options)
{
	return Options[WeightedPickIndex(OsmId, Salt, Options)].first;
}

/** A deterministic value between Low and High. */
double RangePick(int64_t OsmId, int Salt, double Low, double High)
{
	return Low + (High - Low) * Hash01(OsmId, Salt);
}

/** Linear interpolation through the points, clamped at both ends (numpy.interp). */
double Interpolate(double X, const std::vector<std::pair<double, double>>& Points)
{
	if (X <= Points.front().first)
	{
		return Points.front().second;
	}
	if (X >= Points.back().first)
	{
		return Points.back().second;
	}
	for (size_t Index = 1; Index < Points.size(); ++Index)
	{
		if (X <= Points[Index].first)
		{
			const double Fraction = (X - Points[Index - 1].first) / (Points[Index].first - Points[Index - 1].first);
			return Points[Index - 1].second + Fraction * (Points[Index].second - Points[Index - 1].second);
		}
	}
	return Points.back().second;
}

/** Python's round() for a float: halves go to the even neighbour. */
int RoundHalfEven(double Value)
{
	return static_cast<int>(std::nearbyint(Value));
}

/** Geometry facts of one building footprint. */
struct FFootprintShape
{
	double Area = 0.0;
	FWorldPoint Centroid;
	FWorldPoint LongAxis;
	FWorldPoint ShortAxis;
	double LongLength = 0.0;
	double ShortLength = 0.0;
	double Rectangularity = 0.0;
	int CornerCount = 0;
	double Aspect = 0.0;

	/** Measures the polygon: area, centroid, the rectangle around it and how well it fills it. */
	explicit FFootprintShape(const FBuildingPolygon& Polygon)
	{
		Area = PolygonArea(Polygon);
		Centroid = PolygonCentroid(Polygon);
		std::array<FWorldPoint, 4> Corners;
		if (!MinimumRotatedRectangle(Polygon, Corners))
		{
			const FBox2 Box = BoundingBox(Polygon);
			Corners = {{{Box.MinX, Box.MinY}, {Box.MaxX, Box.MinY}, {Box.MaxX, Box.MaxY}, {Box.MinX, Box.MaxY}}};
		}
		const FWorldPoint First = Subtract(Corners[1], Corners[0]);
		const FWorldPoint Second = Subtract(Corners[2], Corners[1]);
		const double FirstLength = std::hypot(First.X, First.Y);
		const double SecondLength = std::hypot(Second.X, Second.Y);
		if (FirstLength >= SecondLength)
		{
			LongAxis = Unit(First);
			ShortAxis = Unit(Second);
			LongLength = FirstLength;
			ShortLength = SecondLength;
		}
		else
		{
			LongAxis = Unit(Second);
			ShortAxis = Unit(First);
			LongLength = SecondLength;
			ShortLength = FirstLength;
		}
		const double RectangleArea = std::max(LongLength * ShortLength, 1e-6);
		Rectangularity = std::min(1.0, Area / RectangleArea);
		CornerCount = static_cast<int>(Polygon.Outline.size());
		Aspect = LongLength / std::max(ShortLength, 0.1);
	}
};

/** One street polyline with the attributes the rules look at. */
struct FStreet
{
	std::string Highway;
	std::string Name;
};

/** Streets of one kind with a grid over their segments. */
struct FStreetSet
{
	std::vector<FStreet> Streets;
	std::vector<FMeasuredLine> Lines;
	std::vector<FBox2> Boxes;
	FSegmentGrid Grid;
	/** Distances closer than this are the same distance. */
	static constexpr double TieTolerance = 1e-9;

	/** Indexes the streets; the set must not move afterwards. */
	void Finish()
	{
		for (const FMeasuredLine& Line : Lines)
		{
			FBox2 Box;
			for (const FWorldPoint& Point : Line.Points)
			{
				Box.Add(Point);
			}
			Boxes.push_back(Box);
		}
		Grid.Build(Lines, 50.0);
	}

	/**
	 * The street closest to the polygon within MaxDistance: its index and distance. Streets at the same distance (two
	 * ways that meet at the nearest point) go to the one whose bounding box is closer to the building's, which is
	 * the one GEOS's nearest neighbour search reaches first; then to the lower index.
	 */
	bool Nearest(const FBuildingPolygon& Polygon, const FBox2& Box, double MaxDistance, size_t& Index, double& Distance) const
	{
		std::vector<std::pair<size_t, double>> Candidates;
		double Best = MaxDistance;
		Grid.Query(Box, MaxDistance, [&](size_t Line, const FWorldPoint& A, const FWorldPoint& B) {
			const double Candidate = PolygonSegmentDistance(Polygon, Box, A, B, Best);
			if (Candidate > Best)
			{
				return;
			}
			Best = Candidate;
			Candidates.emplace_back(Line, Candidate);
		});
		bool bFound = false;
		double BestBoxDistance = 0.0;
		for (const auto& [Line, Candidate] : Candidates)
		{
			if (Candidate > Best + TieTolerance)
			{
				continue;
			}
			const double BoxDistance = Boxes[Line].DistanceTo(Box);
			const bool bBetter = !bFound || BoxDistance < BestBoxDistance || (BoxDistance == BestBoxDistance && Line < Index);
			if (bBetter)
			{
				bFound = true;
				Index = Line;
				BestBoxDistance = BoxDistance;
			}
		}
		Distance = Best;
		return bFound;
	}

	/** True when a street accepted by the predicate lies within Distance of the polygon. */
	template <typename FPredicate>
	bool AnyWithin(const FBuildingPolygon& Polygon, const FBox2& Box, double Distance, FPredicate Accept) const
	{
		bool bFound = false;
		Grid.Query(Box, Distance, [&](size_t Line, const FWorldPoint& A, const FWorldPoint& B) {
			if (bFound || PolygonSegmentDistance(Polygon, Box, A, B, Distance) > Distance)
			{
				return;
			}
			bFound = Accept(Line);
		});
		return bFound;
	}
};

/** A landuse polygon (possibly several parts). */
struct FLanduseArea
{
	std::string Use;
	double Area = 0.0;
	std::vector<FBuildingPolygon> Parts;
};

/** A party wall neighbour: its index, the length of the shared wall and a point on it. */
struct FContact
{
	size_t Other;
	double Length;
	FWorldPoint Point;
};

/** Grid over points for "all within a radius" queries. */
class FPointGrid
{
public:
	void Build(const std::vector<FWorldPoint>& InPoints, double CellSize)
	{
		Points = &InPoints;
		Cell = CellSize;
		Cells.clear();
		for (size_t Index = 0; Index < InPoints.size(); ++Index)
		{
			Cells[KeyOf(CellOf(InPoints[Index].X), CellOf(InPoints[Index].Y))].push_back(static_cast<uint32_t>(Index));
		}
	}

	/** Indices of the points no further than Radius from Center (Radius up to the cell size). */
	void Query(const FWorldPoint& Center, double Radius, std::vector<uint32_t>& Result) const
	{
		Result.clear();
		const int64_t CenterX = CellOf(Center.X);
		const int64_t CenterY = CellOf(Center.Y);
		for (int64_t CellY = CenterY - 1; CellY <= CenterY + 1; ++CellY)
		{
			for (int64_t CellX = CenterX - 1; CellX <= CenterX + 1; ++CellX)
			{
				AppendCell(CellX, CellY, Center, Radius, Result);
			}
		}
		std::sort(Result.begin(), Result.end());
	}

private:
	/** Adds the points of one cell that are within Radius of Center. */
	void AppendCell(int64_t CellX, int64_t CellY, const FWorldPoint& Center, double Radius, std::vector<uint32_t>& Result) const
	{
		const auto Found = Cells.find(KeyOf(CellX, CellY));
		if (Found == Cells.end())
		{
			return;
		}
		for (uint32_t Index : Found->second)
		{
			const FWorldPoint& Point = (*Points)[Index];
			if (std::hypot(Point.X - Center.X, Point.Y - Center.Y) <= Radius)
			{
				Result.push_back(Index);
			}
		}
	}

	int64_t CellOf(double Value) const { return static_cast<int64_t>(std::floor(Value / Cell)); }
	static int64_t KeyOf(int64_t CellX, int64_t CellY) { return CellX * 1000003 + CellY; }

	const std::vector<FWorldPoint>* Points = nullptr;
	double Cell = 100.0;
	std::unordered_map<int64_t, std::vector<uint32_t>> Cells;
};

/** Pieces of boundaries inside the band, as lines of points. */
using FLineList = std::vector<std::vector<FWorldPoint>>;

/** A point on a segment where it crosses the band's outline, with its place along the segment as a fraction. */
struct FSegmentNode
{
	double Fraction;
	FWorldPoint Point;
};

/** Adds the places where the segment crosses an edge of the ring to the nodes. */
void AddRingNodes(const FWorldPoint& Start, const FWorldPoint& End, const FRing& Ring, std::vector<FSegmentNode>& Nodes)
{
	const FWorldPoint Direction = Subtract(End, Start);
	const double LengthSquared = Dot(Direction, Direction);
	for (size_t Index = 0; Index < Ring.size() && LengthSquared > 0.0; ++Index)
	{
		const FWorldPoint& EdgeStart = Ring[Index];
		const FWorldPoint& EdgeEnd = Ring[(Index + 1) % Ring.size()];
		FWorldPoint Crossing;
		if (SegmentDistance(Start, End, EdgeStart, EdgeEnd) != 0.0 || !LineIntersectionPoint(Start, End, EdgeStart, EdgeEnd, Crossing))
		{
			continue;
		}
		const double Fraction = Dot(Subtract(Crossing, Start), Direction) / LengthSquared;
		if (Fraction > 0.0 && Fraction < 1.0)
		{
			Nodes.push_back({Fraction, Crossing});
		}
	}
}

/** The ends of the segment and every crossing with the band's outlines, in order along the segment. */
std::vector<FSegmentNode> SegmentNodes(const FWorldPoint& Start, const FWorldPoint& End, const std::vector<FBuildingPolygon>& Band)
{
	std::vector<FSegmentNode> Nodes = {{0.0, Start}};
	for (const FBuildingPolygon& Piece : Band)
	{
		AddRingNodes(Start, End, Piece.Outline, Nodes);
		for (const FRing& Hole : Piece.Holes)
		{
			AddRingNodes(Start, End, Hole, Nodes);
		}
	}
	Nodes.push_back({1.0, End});
	std::sort(Nodes.begin(), Nodes.end(), [](const FSegmentNode& A, const FSegmentNode& B) { return A.Fraction < B.Fraction; });
	return Nodes;
}

/** True when the point is in the band or on its edge. */
bool IsInBand(const std::vector<FBuildingPolygon>& Band, const FWorldPoint& Point)
{
	for (const FBuildingPolygon& Piece : Band)
	{
		if (LocatePoint(Piece, Point) != EPointLocation::Outside)
		{
			return true;
		}
	}
	return false;
}

/**
 * Adds the parts of one segment inside the band to the lines; Open is the line that was running into the segment's
 * start, or nullptr. Returns true when the last part runs to the segment's end, so the next segment can continue it.
 */
bool ClipSegmentToBand(const FWorldPoint& Start, const FWorldPoint& End, const std::vector<FBuildingPolygon>& Band,
	std::vector<FWorldPoint>*& Open, FLineList& Lines)
{
	const std::vector<FSegmentNode> Nodes = SegmentNodes(Start, End, Band);
	bool bContinuesToEnd = false;
	for (size_t Node = 0; Node + 1 < Nodes.size(); ++Node)
	{
		const FWorldPoint& From = Nodes[Node].Point;
		const FWorldPoint& To = Nodes[Node + 1].Point;
		const FWorldPoint Middle = {0.5 * (From.X + To.X), 0.5 * (From.Y + To.Y)};
		const bool bInside = Nodes[Node + 1].Fraction > Nodes[Node].Fraction && IsInBand(Band, Middle);
		bContinuesToEnd = bInside && Node + 2 == Nodes.size();
		if (!bInside)
		{
			Open = nullptr;
			continue;
		}
		if (Open == nullptr)
		{
			Lines.emplace_back();
			Open = &Lines.back();
			Open->push_back(From);
		}
		Open->push_back(To);
	}
	return bContinuesToEnd;
}

/**
 * Adds the parts of one ring inside the band to the lines, joined across vertices but not across the ring's start
 * point, which overlay leaves a node.
 */
void ClipRingToBand(const FRing& Ring, const std::vector<FBuildingPolygon>& Band, FLineList& Lines)
{
	std::vector<FWorldPoint>* Open = nullptr;
	for (size_t Index = 0; Index < Ring.size(); ++Index)
	{
		if (!ClipSegmentToBand(Ring[Index], Ring[(Index + 1) % Ring.size()], Band, Open, Lines))
		{
			Open = nullptr;
		}
	}
}

/** The total length of the lines and the centroid of them as a line (the middles of the segments, weighted by length). */
double LinesLengthAndCentroid(const FLineList& Lines, FWorldPoint& Centroid)
{
	double Length = 0.0;
	double CentroidX = 0.0;
	double CentroidY = 0.0;
	for (const std::vector<FWorldPoint>& Line : Lines)
	{
		for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
		{
			const double SegmentLength = std::hypot(Line[Index + 1].X - Line[Index].X, Line[Index + 1].Y - Line[Index].Y);
			Length += SegmentLength;
			CentroidX += SegmentLength * 0.5 * (Line[Index].X + Line[Index + 1].X);
			CentroidY += SegmentLength * 0.5 * (Line[Index].Y + Line[Index + 1].Y);
		}
	}
	Centroid = {CentroidX / Length, CentroidY / Length};
	return Length;
}

/** GEOS's interior point of lines: the interior vertex closest to the centroid, else the closest end point. */
FWorldPoint InteriorPointOfLines(const FLineList& Lines, const FWorldPoint& Centroid)
{
	FWorldPoint Best;
	double Closest = 1e300;
	auto Consider = [&](const FWorldPoint& Candidate) {
		const double Away = std::hypot(Candidate.X - Centroid.X, Candidate.Y - Centroid.Y);
		if (Away < Closest)
		{
			Closest = Away;
			Best = Candidate;
		}
	};
	for (const std::vector<FWorldPoint>& Line : Lines)
	{
		for (size_t Index = 1; Index + 1 < Line.size(); ++Index)
		{
			Consider(Line[Index]);
		}
	}
	if (Closest < 1e300)
	{
		return Best;
	}
	for (const std::vector<FWorldPoint>& Line : Lines)
	{
		Consider(Line.front());
		Consider(Line.back());
	}
	return Best;
}

/**
 * The boundary of Other inside the band of Distance around Polygon: its total length and GEOS's representative point
 * of those lines.
 */
bool SharedBoundary(const FBuildingPolygon& Polygon, const FBuildingPolygon& Other, double Distance, double& Length,
	FWorldPoint& Point)
{
	const std::vector<FBuildingPolygon> Band = BufferRound(Polygon, Distance);
	FLineList Lines;
	ClipRingToBand(Other.Outline, Band, Lines);
	for (const FRing& Hole : Other.Holes)
	{
		ClipRingToBand(Hole, Band, Lines);
	}
	FWorldPoint Centroid;
	Length = LinesLengthAndCentroid(Lines, Centroid);
	if (Length <= 0.0)
	{
		return false;
	}
	Point = InteriorPointOfLines(Lines, Centroid);
	return true;
}
}

int ClassIdOf(std::string_view ClassName)
{
	for (size_t Index = 0; Index < ClassNames.size(); ++Index)
	{
		if (ClassNames[Index] == ClassName)
		{
			return static_cast<int>(Index);
		}
	}
	return -1;
}

int RoofIdOf(std::string_view RoofName)
{
	for (size_t Index = 0; Index < RoofNames.size(); ++Index)
	{
		if (RoofNames[Index] == RoofName)
		{
			return static_cast<int>(Index);
		}
	}
	return -1;
}

/** The nearest street to a building, with the street's direction there. */
struct FStreetInfo
{
	const FStreet* Street = nullptr;
	double Distance = 0.0;
	FWorldPoint Tangent;
	FWorldPoint Nearest;
};

struct FBuildingTyper::FImpl
{
	const std::vector<FBuildingFootprint>& Items;
	std::vector<FFootprintShape> Shapes;
	std::vector<FBox2> Boxes;
	std::vector<FWorldPoint> Centroids;
	std::vector<double> Areas;
	FStrTree Tree;
	FStreetSet MainStreets;
	FStreetSet ServiceStreets;
	FStreetSet PedestrianStreets;
	std::vector<FLanduseArea> Landuse;
	std::vector<FBox2> LanduseBoxes;
	FStrTree LanduseTree;
	std::vector<double> BuiltRatio;
	std::vector<std::vector<FContact>> Contacts;
	std::vector<size_t> Component;
	std::unordered_map<size_t, int> GroupSizes;
	std::vector<int> Repeats;

	explicit FImpl(const std::vector<FBuildingFootprint>& InItems) : Items(InItems) {}

	void BuildShapes();
	void BuildStreets(const std::vector<FOsmWay>& Roads, const std::vector<FOsmWay>& Footways);
	void BuildLanduse(const std::vector<FOsmArea>& AreaList);
	void BuildDensity(unsigned ThreadCount);
	/** Adds Second to the contacts of First if the two touch along a party wall. */
	void AddContact(size_t First, size_t Second);
	void BuildContacts(unsigned ThreadCount);
	void BuildComponents();
	void BuildEstates(unsigned ThreadCount);
	const FLanduseArea* LanduseAt(const FWorldPoint& Point) const;
	std::optional<FStreetInfo> NearestStreet(size_t Index) const;
};

namespace
{
/** Adds a way as a street polyline. */
void AddStreet(FStreetSet& Set, const FOsmWay& Way, const std::string& Highway)
{
	FStreet Street;
	Street.Highway = Highway;
	const std::string* Name = FindTag(Way.Tags, "name");
	Street.Name = Name != nullptr ? *Name : std::string();
	Set.Streets.push_back(std::move(Street));
	FMeasuredLine Line;
	Line.Points = Way.Points;
	Line.ComputeLength();
	Set.Lines.push_back(std::move(Line));
}
}

/** Measures every footprint and indexes their boxes. */
void FBuildingTyper::FImpl::BuildShapes()
{
	Shapes.reserve(Items.size());
	for (const FBuildingFootprint& Item : Items)
	{
		Shapes.emplace_back(Item.Polygon);
		Boxes.push_back(BoundingBox(Item.Polygon));
		Centroids.push_back(Shapes.back().Centroid);
		Areas.push_back(Shapes.back().Area);
	}
	Tree.Build(Boxes);
}

/** Streets by class: main (drivable except service), service ways, and pedestrian streets. */
void FBuildingTyper::FImpl::BuildStreets(const std::vector<FOsmWay>& Roads, const std::vector<FOsmWay>& Footways)
{
	for (const FOsmWay& Way : Roads)
	{
		if (Way.Points.size() < 2)
		{
			continue;
		}
		const std::string* Highway = FindTag(Way.Tags, "highway");
		const std::string HighwayName = Highway != nullptr ? *Highway : std::string();
		AddStreet(HighwayName == "service" ? ServiceStreets : MainStreets, Way, HighwayName);
	}
	for (const FOsmWay& Way : Footways)
	{
		const std::string* Highway = FindTag(Way.Tags, "highway");
		if (Highway != nullptr && *Highway == "pedestrian" && Way.Points.size() >= 2)
		{
			AddStreet(PedestrianStreets, Way, "pedestrian");
		}
	}
	MainStreets.Finish();
	ServiceStreets.Finish();
	PedestrianStreets.Finish();
}

/** Landuse polygons sorted so the smallest containing one wins. */
void FBuildingTyper::FImpl::BuildLanduse(const std::vector<FOsmArea>& AreaList)
{
	static const FSet Wanted = {"residential", "commercial", "retail", "industrial", "farmland", "farmyard", "allotments",
		"garages", "railway", "cemetery", "meadow", "orchard"};
	std::vector<FLanduseArea> Unsorted;
	for (const FOsmArea& Area : AreaList)
	{
		const std::string* Use = FindTag(Area.Tags, "landuse");
		if (Use == nullptr || !Contains(Wanted, *Use))
		{
			continue;
		}
		FLanduseArea Landuse;
		Landuse.Use = *Use;
		for (const FOsmPolygon& Polygon : Area.Polygons)
		{
			FBuildingPolygon Part;
			Part.Outline = Polygon.Rings.front();
			Part.Holes.assign(Polygon.Rings.begin() + 1, Polygon.Rings.end());
			Landuse.Area += PolygonArea(Part);
			Landuse.Parts.push_back(std::move(Part));
		}
		Unsorted.push_back(std::move(Landuse));
	}
	std::stable_sort(Unsorted.begin(), Unsorted.end(), [](const FLanduseArea& A, const FLanduseArea& B) { return A.Area < B.Area; });
	Landuse = std::move(Unsorted);
	for (const FLanduseArea& Area : Landuse)
	{
		FBox2 Box;
		for (const FBuildingPolygon& Part : Area.Parts)
		{
			const FBox2 PartBox = BoundingBox(Part);
			Box.Add({PartBox.MinX, PartBox.MinY});
			Box.Add({PartBox.MaxX, PartBox.MaxY});
		}
		LanduseBoxes.push_back(Box);
	}
	LanduseTree.Build(LanduseBoxes);
}

/** Share of the ground covered by buildings within DensityRadius of every building. */
void FBuildingTyper::FImpl::BuildDensity(unsigned ThreadCount)
{
	FPointGrid Grid;
	Grid.Build(Centroids, EstateRadius);
	const double Disc = std::numbers::pi * DensityRadius * DensityRadius;
	BuiltRatio.assign(Items.size(), 0.0);
	ParallelFor(Items.size(), ThreadCount, [&](size_t Index) {
		std::vector<uint32_t> Neighbours;
		Grid.Query(Centroids[Index], DensityRadius, Neighbours);
		double Sum = 0.0;
		for (uint32_t Other : Neighbours)
		{
			Sum += Areas[Other];
		}
		BuiltRatio[Index] = Sum / Disc;
	});
}

/** Adds building Second to the party wall contacts of First when they touch along at least PartyWallMinLength. */
void FBuildingTyper::FImpl::AddContact(size_t First, size_t Second)
{
	const FBuildingPolygon& Polygon = Items[First].Polygon;
	const FBuildingPolygon& OtherPolygon = Items[Second].Polygon;
	if (PolygonDistance(Polygon, OtherPolygon, PartyWallTolerance) > PartyWallTolerance)
	{
		return;
	}
	double Length = 0.0;
	FWorldPoint Point;
	if (SharedBoundary(Polygon, OtherPolygon, PartyWallTolerance, Length, Point) && Length >= PartyWallMinLength)
	{
		Contacts[First].push_back({Second, Length, Point});
	}
}

/** Party walls: for every building the touching neighbours as (index, shared length, a point on the shared wall). */
void FBuildingTyper::FImpl::BuildContacts(unsigned ThreadCount)
{
	Contacts.assign(Items.size(), {});
	ParallelFor(Items.size(), ThreadCount, [&](size_t First) {
		std::vector<uint32_t> Candidates;
		Tree.Query(Boxes[First].Expanded(PartyWallTolerance), Candidates);
		for (uint32_t Second : Candidates)
		{
			if (Second != First)
			{
				AddContact(First, Second);
			}
		}
	});
}

/** Connected groups of party-wall neighbours ignoring tiny outbuildings; Component holds the group id per building. */
void FBuildingTyper::FImpl::BuildComponents()
{
	std::vector<size_t> Parent(Items.size());
	for (size_t Index = 0; Index < Parent.size(); ++Index)
	{
		Parent[Index] = Index;
	}
	auto Find = [&Parent](size_t Index) {
		while (Parent[Index] != Index)
		{
			Parent[Index] = Parent[Parent[Index]];
			Index = Parent[Index];
		}
		return Index;
	};
	for (size_t Index = 0; Index < Items.size(); ++Index)
	{
		if (Areas[Index] < 35.0)
		{
			continue;
		}
		for (const FContact& Contact : Contacts[Index])
		{
			if (Areas[Contact.Other] >= 35.0)
			{
				Parent[Find(Index)] = Find(Contact.Other);
			}
		}
	}
	Component.resize(Items.size());
	for (size_t Index = 0; Index < Items.size(); ++Index)
	{
		Component[Index] = Find(Index);
	}
	for (size_t Index = 0; Index < Items.size(); ++Index)
	{
		if (Areas[Index] >= 35.0)
		{
			++GroupSizes[Component[Index]];
		}
	}
}

/** Counts identical footprints (same area and aspect) within 120 m: planned estates repeat one plan. */
void FBuildingTyper::FImpl::BuildEstates(unsigned ThreadCount)
{
	Repeats.assign(Items.size(), 0);
	FPointGrid Grid;
	Grid.Build(Centroids, EstateRadius);
	ParallelFor(Items.size(), ThreadCount, [&](size_t Index) {
		std::vector<uint32_t> Neighbours;
		Grid.Query(Centroids[Index], EstateRadius, Neighbours);
		if (Areas[Index] < 35.0 || Neighbours.size() < static_cast<size_t>(EstateRepeats))
		{
			return;
		}
		int Count = 0;
		for (uint32_t Other : Neighbours)
		{
			const bool bSameArea = std::abs(Areas[Other] - Areas[Index]) < 0.04 * Areas[Index];
			const bool bSameAspect = std::abs(Shapes[Other].Aspect - Shapes[Index].Aspect) < 0.06 * Shapes[Index].Aspect;
			if (bSameArea && bSameAspect)
			{
				++Count;
			}
		}
		Repeats[Index] = Count - 1;
	});
}

namespace
{
/** True when any part of the landuse area contains the point. */
bool LanduseContains(const FLanduseArea& Area, const FWorldPoint& Point)
{
	for (const FBuildingPolygon& Part : Area.Parts)
	{
		if (LocatePoint(Part, Point) == EPointLocation::Inside)
		{
			return true;
		}
	}
	return false;
}
}

/** The smallest landuse area containing the point, or nullptr. */
const FLanduseArea* FBuildingTyper::FImpl::LanduseAt(const FWorldPoint& Point) const
{
	FBox2 Box;
	Box.Add(Point);
	std::vector<uint32_t> Hits;
	LanduseTree.Query(Box, Hits);
	std::sort(Hits.begin(), Hits.end()); // the areas are sorted by size, so the first one that contains the point wins
	for (uint32_t Hit : Hits)
	{
		if (LanduseContains(Landuse[Hit], Point))
		{
			return &Landuse[Hit];
		}
	}
	return nullptr;
}

/** The closest street of the three kinds and its direction there; main streets and pedestrian streets before service ways. */
std::optional<FStreetInfo> FBuildingTyper::FImpl::NearestStreet(size_t Index) const
{
	constexpr double MaxDistance = 120.0;
	constexpr double ServicePreference = 12.0;
	const FBuildingPolygon& Polygon = Items[Index].Polygon;
	const FBox2& Box = Boxes[Index];
	struct FKind
	{
		const FStreetSet* Set;
		double Preference;
	};
	const FKind Kinds[] = {{&MainStreets, 0.0}, {&PedestrianStreets, 0.0}, {&ServiceStreets, ServicePreference}};
	const FStreetSet* BestSet = nullptr;
	size_t BestStreet = 0;
	double BestDistance = 0.0;
	for (const FKind& Kind : Kinds)
	{
		size_t Street = 0;
		double Distance = 0.0;
		if (!Kind.Set->Nearest(Polygon, Box, MaxDistance, Street, Distance))
		{
			continue;
		}
		const bool bBestIsService = BestSet != nullptr && BestSet->Streets[BestStreet].Highway == "service";
		if (BestSet == nullptr || Distance + Kind.Preference < BestDistance + (bBestIsService ? ServicePreference : 0.0))
		{
			BestSet = Kind.Set;
			BestStreet = Street;
			BestDistance = Distance;
		}
	}
	if (BestSet == nullptr)
	{
		return std::nullopt;
	}
	const FMeasuredLine& Line = BestSet->Lines[BestStreet];
	const double Along = Line.Project(Centroids[Index]);
	FStreetInfo Info;
	Info.Street = &BestSet->Streets[BestStreet];
	Info.Distance = BestDistance;
	Info.Nearest = Line.Interpolate(Along);
	const FWorldPoint Ahead = Line.Interpolate(std::min(Along + 1.0, Line.Length));
	const FWorldPoint Behind = Line.Interpolate(std::max(Along - 1.0, 0.0));
	Info.Tangent = Unit(Subtract(Ahead, Behind));
	return Info;
}

FBuildingTyper::FBuildingTyper(const std::vector<FBuildingFootprint>& InFootprints, const std::vector<FOsmWay>& Roads,
	const std::vector<FOsmWay>& Footways, const std::vector<FOsmArea>& Areas, unsigned ThreadCount)
	: Impl(std::make_unique<FImpl>(InFootprints))
{
	Impl->BuildShapes();
	Impl->BuildStreets(Roads, Footways);
	Impl->BuildLanduse(Areas);
	Impl->BuildDensity(ResolveThreadCount(ThreadCount));
	Impl->BuildContacts(ResolveThreadCount(ThreadCount));
	Impl->BuildComponents();
	Impl->BuildEstates(ResolveThreadCount(ThreadCount));
}

FBuildingTyper::~FBuildingTyper() = default;

namespace
{
/** Axis and extents of a building's frame: the facade axis t, the depth axis n and the party wall sides. */
struct FFrame
{
	FWorldPoint T;
	FWorldPoint N;
	bool bTIsLongAxis = true;
	double Width = 0.0;
	double Depth = 0.0;
	bool bClosedLeft = false;
	bool bClosedRight = false;
	int PartyCount = 0;
};
}

/** Everything the rules look at for one building. */
struct FBuildingFacts
{
	const FBuildingTyper::FImpl& Typer;
	size_t Index;
	int64_t OsmId;
	const FTags& Tags;
	const FFootprintShape& Footprint;
	std::optional<FStreetInfo> Street;
	FFrame Frame;

	double Area = 0.0;
	double BuiltRatio = 0.0;
	int Repeats = 0;
	std::string Landuse;
	int RowSize = 1;
	std::string BuildingTag;
	std::optional<int> Year;
	std::optional<double> Levels;
	double StreetDistance = 999.0;
	std::string StreetName;
	bool bNearPedestrian = false;
	double FreeDistance = 40.0;

	FBuildingFacts(const FBuildingTyper::FImpl& InTyper, size_t InIndex, const std::optional<FStreetInfo>& InStreet,
		const FFrame& InFrame);

	bool IsClosedRow() const { return Frame.bClosedLeft && Frame.bClosedRight; }
	bool IsAttached() const { return Frame.bClosedLeft || Frame.bClosedRight; }
	bool IsUrban() const { return BuiltRatio >= 0.22; }
	bool HasLevels() const { return Levels.has_value(); }
	/** Python's "levels or fallback": the fallback when the tag is missing or zero. */
	double LevelsOr(double Fallback) const { return IsTruthy(Levels) ? *Levels : Fallback; }
	/** The random id the row's roof draws are made with. */
	int64_t RoofDrawKey() const;
	std::string_view Draw(int Salt, const FWeightedNames& Options) const { return WeightedPick(RoofDrawKey(), Salt, Options); }
	double Uniform(int Salt) const { return Hash01(OsmId, Salt); }
	double FreeDistanceToOthers() const;
};

namespace
{
/**
 * Facade axis t (along the street or the party walls) and depth axis n pointing from the street into the back, with
 * the sides that have a party wall.
 */
FFrame ComputeFrame(const FBuildingTyper::FImpl& Typer, size_t Index, const std::optional<FStreetInfo>& Street)
{
	const FFootprintShape& Footprint = Typer.Shapes[Index];
	const FWorldPoint Axes[2] = {Footprint.LongAxis, Footprint.ShortAxis};
	double Offsets[2] = {0.0, 0.0};
	FFrame Frame;
	for (const FContact& Contact : Typer.Contacts[Index])
	{
		if (Typer.Areas[Contact.Other] < 20.0)
		{
			continue;
		}
		++Frame.PartyCount;
		const FWorldPoint Direction = Subtract(Contact.Point, Footprint.Centroid);
		for (int AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
		{
			Offsets[AxisIndex] += std::abs(Dot(Direction, Axes[AxisIndex])) * Contact.Length;
		}
	}
	int Chosen = 0;
	if (Frame.PartyCount > 0)
	{
		Chosen = Offsets[0] >= Offsets[1] ? 0 : 1;
	}
	else if (Street.has_value() && Street->Distance < 40.0)
	{
		Chosen = std::abs(Dot(Axes[0], Street->Tangent)) >= std::abs(Dot(Axes[1], Street->Tangent)) ? 0 : 1;
	}
	Frame.T = Axes[Chosen];
	Frame.bTIsLongAxis = Chosen == 0;
	Frame.N = {-Frame.T.Y, Frame.T.X};
	if (Street.has_value())
	{
		const FWorldPoint TowardStreet = Subtract(Street->Nearest, Footprint.Centroid);
		if (Dot(Frame.N, TowardStreet) > 0.0)
		{
			Frame.N = {-Frame.N.X, -Frame.N.Y}; // n points away from the street, into the building's back
		}
	}
	Frame.Width = Frame.bTIsLongAxis ? Footprint.LongLength : Footprint.ShortLength;
	Frame.Depth = Frame.bTIsLongAxis ? Footprint.ShortLength : Footprint.LongLength;
	// Left and right as seen from the street looking at the facade: facing -n, right is -n rotated.
	const FWorldPoint RightAxis = {Frame.N.Y, -Frame.N.X};
	for (const FContact& Contact : Typer.Contacts[Index])
	{
		if (Typer.Areas[Contact.Other] < 20.0)
		{
			continue;
		}
		const FWorldPoint Offset = Subtract(Contact.Point, Footprint.Centroid);
		if (std::abs(Dot(Offset, Frame.T)) < 0.3 * Frame.Width)
		{
			continue;
		}
		if (Dot(Offset, RightAxis) > 0.0)
		{
			Frame.bClosedRight = true;
		}
		else
		{
			Frame.bClosedLeft = true;
		}
	}
	return Frame;
}
}

FBuildingFacts::FBuildingFacts(const FBuildingTyper::FImpl& InTyper, size_t InIndex,
	const std::optional<FStreetInfo>& InStreet, const FFrame& InFrame)
	: Typer(InTyper), Index(InIndex), OsmId(InTyper.Items[InIndex].OsmId), Tags(*InTyper.Items[InIndex].Tags),
	  Footprint(InTyper.Shapes[InIndex]), Street(InStreet), Frame(InFrame)
{
	Area = Footprint.Area;
	BuiltRatio = Typer.BuiltRatio[Index];
	Repeats = Typer.Repeats[Index];
	const FLanduseArea* Use = Typer.LanduseAt(Footprint.Centroid);
	Landuse = Use != nullptr ? Use->Use : std::string();
	const auto Group = Typer.GroupSizes.find(Typer.Component[Index]);
	RowSize = Group != Typer.GroupSizes.end() ? Group->second : 1;
	const std::string* BuildingValue = FindTag(Tags, "building");
	BuildingTag = BuildingValue != nullptr ? *BuildingValue : "yes";
	const std::string* StartDate = FindTag(Tags, "start_date");
	if (StartDate != nullptr)
	{
		Year = ParseYear(*StartDate);
	}
	Levels = ParseTagNumber(FindTag(Tags, "building:levels"));
	if (Street.has_value())
	{
		StreetDistance = Street->Distance;
		StreetName = Street->Street->Name;
	}
	bNearPedestrian = Typer.PedestrianStreets.AnyWithin(Typer.Items[Index].Polygon, Typer.Boxes[Index], 25.0,
		[](size_t) { return true; });
	FreeDistance = FreeDistanceToOthers();
}

/** Distance to the nearest other main building (a proxy for the plot size), at most 40 m. */
double FBuildingFacts::FreeDistanceToOthers() const
{
	std::vector<uint32_t> Found;
	Typer.Tree.Query(Typer.Boxes[Index].Expanded(40.0), Found);
	double Best = 40.0;
	for (uint32_t Other : Found)
	{
		if (Other == Index || Typer.Areas[Other] < 45.0)
		{
			continue;
		}
		Best = std::min(Best, PolygonDistance(Typer.Items[Index].Polygon, Typer.Items[Other].Polygon, Best));
	}
	return Best;
}

/** The id the roof draws are made with: the same for a whole row, so attached buildings share one roof type. */
int64_t FBuildingFacts::RoofDrawKey() const
{
	if (RowSize >= 2 && IsAttached())
	{
		return Typer.Items[Typer.Component[Index]].OsmId;
	}
	return OsmId;
}

namespace
{
struct FClassDecision
{
	std::string_view ClassName;
	int Flags = 0;
	int TagBits = 0;
};

/** A class decision from its three parts. */
FClassDecision Decision(std::string_view ClassName, int Flags, int TagBits)
{
	return {ClassName, Flags, TagBits};
}

/** Retail, commercial and office buildings: big single storey sheds are halls, the rest shop-and-office houses. */
FClassDecision CommercialOrHall(const FBuildingFacts& Facts, int Flags, int TagBits)
{
	const bool bBig = Facts.Area >= 500.0 && Facts.LevelsOr(1.0) <= 3.0;
	if (bBig && IsOneOf(Facts.BuildingTag, {"retail", "supermarket", "mall"}))
	{
		return Decision("retail_centre", Flags, TagBits);
	}
	if (bBig || Facts.Area >= 1200.0)
	{
		return Decision("industrial_hall", Flags, TagBits);
	}
	return Decision("commercial_groundfloor", Flags | FlagShopGroundFloor, TagBits);
}

/** Farm buildings: the house and the barn are both class 11 of the typology; auxiliary buildings are sheds. */
FClassDecision Farm(const FBuildingFacts& Facts, int Flags, int TagBits)
{
	if (IsOneOf(Facts.BuildingTag, {"farm_auxiliary", "stable", "cowshed", "sty", "livestock", "barn"}))
	{
		if (Facts.Area < 60.0)
		{
			return Decision("shed_garage", Flags, TagBits);
		}
		return Decision("vierlande_farmhouse", Flags | FlagBarn, TagBits);
	}
	return Decision("vierlande_farmhouse", Flags, TagBits);
}

/** building=yes without anything else: sheds, halls and farm buildings by size and surroundings; false for a housing candidate. */
bool GuessUntagged(const FBuildingFacts& Facts, std::string_view& ClassName, int& ExtraFlags)
{
	const double Area = Facts.Area;
	ExtraFlags = 0;
	if (Area < 35.0)
	{
		ClassName = "shed_garage";
		return true;
	}
	if (Area < 70.0 && Facts.FreeDistance > 3.0 && Facts.RowSize == 1 && !IsTruthy(Facts.Levels))
	{
		ClassName = "shed_garage";
		return true;
	}
	if (Area < 60.0 && !Facts.HasLevels() && Facts.StreetDistance > 25.0)
	{
		ClassName = "shed_garage";
		return true;
	}
	if (Area >= 600.0 && IsOneOf(Facts.Landuse, {"industrial", "commercial", "retail"}))
	{
		ClassName = "industrial_hall";
		return true;
	}
	if (Area >= 1500.0 && Facts.LevelsOr(1.0) <= 2.0)
	{
		ClassName = "industrial_hall";
		return true;
	}
	std::string LowerName = Facts.StreetName;
	std::transform(LowerName.begin(), LowerName.end(), LowerName.begin(), [](unsigned char Character) { return std::tolower(Character); });
	const bool bDeich = LowerName.find("deich") != std::string::npos;
	if (IsOneOf(Facts.Landuse, {"farmyard", "farmland"}) && Area >= 120.0 && Facts.BuiltRatio < 0.1)
	{
		const bool bBarn = Facts.Footprint.Aspect >= 1.8 && !Facts.HasLevels();
		ClassName = "vierlande_farmhouse";
		ExtraFlags = bBarn ? FlagBarn : 0;
		return true;
	}
	if (bDeich && Facts.BuiltRatio < 0.1 && Area >= 200.0 && Facts.Footprint.Aspect >= 1.8)
	{
		ClassName = "vierlande_farmhouse";
		return true;
	}
	return false;
}

/** Half-timbered town house of the old centre: dated before 1870, or a low gabled house in a closed front on a pedestrian street. */
bool IsOldTownHouse(const FBuildingFacts& Facts)
{
	if (Facts.Area > 320.0 || Facts.Area < 50.0)
	{
		return false;
	}
	if (Facts.Year.has_value() && *Facts.Year < 1870)
	{
		return Facts.IsUrban() || Facts.bNearPedestrian;
	}
	if (!(Facts.bNearPedestrian && (Facts.IsAttached() || Facts.RowSize >= 2)))
	{
		return false;
	}
	const bool bLow = Facts.LevelsOr(2.0) <= 3.0;
	const std::string* RoofShape = FindTag(Facts.Tags, "roof:shape");
	const bool bPitched = RoofShape == nullptr || *RoofShape == "gabled" || *RoofShape == "hipped";
	return bLow && bPitched && !Facts.Year.has_value() && Facts.Uniform(18) < 0.7;
}

/** A building in a closed frontage on a pedestrian street or along a retail landuse is a shop house. */
bool IsShopStreetFrontage(const FBuildingFacts& Facts)
{
	if (!Facts.IsAttached() && Facts.RowSize < 2)
	{
		return false;
	}
	if (Facts.Area < 70.0 || Facts.Area > 450.0)
	{
		return false;
	}
	if (Facts.bNearPedestrian && Facts.StreetDistance < 15.0 && Facts.BuiltRatio > 0.2)
	{
		return true;
	}
	return Facts.Landuse == "retail" && Facts.StreetDistance < 12.0;
}

/** A pre-1918 house: villa when it stands free on a big plot and has an irregular plan. */
std::string_view GruenderzeitOrVilla(const FBuildingFacts& Facts)
{
	const bool bFree = Facts.FreeDistance >= 8.0 && !Facts.IsAttached();
	const bool bIrregular = Facts.Footprint.Rectangularity < 0.85 || Facts.Footprint.CornerCount >= 8;
	if (bFree && bIrregular && 120.0 <= Facts.Area && Facts.Area <= 420.0)
	{
		return "villa";
	}
	return "gruenderzeit_clinker";
}

/** Class from a start_date, for housing-sized buildings; empty when the date does not decide. */
std::string_view ClassByYear(const FBuildingFacts& Facts, int Year)
{
	const double Area = Facts.Area;
	if (Year >= 1985)
	{
		return Area > 90.0 ? "modern" : std::string_view();
	}
	if (Year <= 1918)
	{
		return GruenderzeitOrVilla(Facts);
	}
	if (Year <= 1939)
	{
		if (Area >= 400.0 || Facts.Footprint.LongLength >= 30.0)
		{
			return "brick_block_1920s";
		}
		return std::string_view();
	}
	if (Year <= 1979)
	{
		if (Area >= 250.0 && Facts.LevelsOr(0.0) >= 4.0)
		{
			return "slab_block";
		}
		return Area >= 100.0 ? "postwar_plaster" : std::string_view();
	}
	return std::string_view();
}

/** Free-standing big block in an open building pattern: long thin slab or a point block. */
bool IsTowerOrSlab(const FBuildingFacts& Facts)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	if (!IsOneOf(Facts.BuildingTag, {"apartments", "residential", "yes", "dormitory"}))
	{
		return false;
	}
	if ((Facts.IsAttached() || Facts.RowSize > 1) && Facts.BuildingTag != "apartments")
	{
		return false;
	}
	if (Facts.HasLevels() && *Facts.Levels >= 8.0)
	{
		return true;
	}
	const bool bOpenPattern = Facts.BuiltRatio < 0.30 && (Facts.FreeDistance >= 6.0 || Facts.StreetDistance >= 9.0);
	if (!bOpenPattern)
	{
		return false;
	}
	const bool bSlab = Footprint.LongLength >= 28.0 && 9.0 <= Footprint.ShortLength && Footprint.ShortLength <= 18.0;
	const bool bPoint = Footprint.Aspect < 1.5 && Footprint.ShortLength >= 16.0 && Facts.LevelsOr(0.0) >= 6.0;
	const bool bTallEnough = !Facts.HasLevels() || *Facts.Levels >= 4.0;
	return (bSlab || bPoint) && bTallEnough && Facts.Area >= 280.0;
}

/** Narrow deep units in a row of at least three. */
bool IsRowHouse(const FBuildingFacts& Facts)
{
	if (Facts.RowSize < 3 || Facts.Frame.Width > 10.5 || Facts.Frame.Depth < 4.5 || Facts.Area > 130.0)
	{
		return false;
	}
	if (Facts.HasLevels() && *Facts.Levels >= 4.0)
	{
		return false;
	}
	return !(Facts.IsUrban() && Facts.Frame.Width >= 7.0 && Facts.LevelsOr(0.0) >= 3.0);
}

/** A house in a street frontage in a built-up area: Gruenderzeit, 1920s block or later infill. */
FClassDecision UrbanBlockClass(const FBuildingFacts& Facts, int Flags, int TagBits, bool bHeritage)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	const std::string* RoofTag = FindTag(Facts.Tags, "roof:shape");
	if (bHeritage)
	{
		return Decision("gruenderzeit_clinker", Flags, TagBits);
	}
	const bool bLongBrickBlock = Footprint.LongLength >= 30.0 && 9.0 <= Footprint.ShortLength && Footprint.ShortLength <= 15.0;
	if (RoofTag != nullptr && *RoofTag == "flat" && Facts.LevelsOr(3.0) >= 3.0)
	{
		return Decision(Facts.Draw(11, {{"postwar_plaster", 5}, {"modern", 2}, {"slab_block", 1}}), Flags, TagBits);
	}
	if (bLongBrickBlock)
	{
		return Decision(Facts.Draw(12, {{"brick_block_1920s", 6}, {"postwar_plaster", 3}, {"gruenderzeit_clinker", 1}}), Flags, TagBits);
	}
	const double Centrality = std::min(1.0, Facts.BuiltRatio / 0.4);
	const FWeightedNames Weights = {{"gruenderzeit_clinker", 4 + 4 * Centrality}, {"brick_block_1920s", 2},
		{"postwar_plaster", 3 - Centrality}, {"modern", 0.6}};
	return Decision(Facts.Draw(13, Weights), Flags, TagBits);
}

/** Single family houses: villa on a large plot with a rich plan, farmhouse on the dykes, otherwise detached. */
FClassDecision DetachedOrVilla(const FBuildingFacts& Facts, int Flags, int TagBits, bool bHeritage)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	const double Area = Facts.Area;
	std::string LowerName = Facts.StreetName;
	std::transform(LowerName.begin(), LowerName.end(), LowerName.begin(), [](unsigned char Character) { return std::tolower(Character); });
	const bool bDeich = LowerName.find("deich") != std::string::npos;
	const bool bRural = Facts.BuiltRatio < 0.08;
	if (bDeich && Area >= 140.0 && Footprint.Aspect >= 1.5 && bRural)
	{
		return Decision("vierlande_farmhouse", Flags, TagBits);
	}
	if (Area >= 150.0 && Facts.FreeDistance >= 10.0 && (Footprint.Rectangularity < 0.84 || Footprint.CornerCount >= 10))
	{
		return Decision("villa", Flags, TagBits);
	}
	if (bHeritage && Area >= 120.0 && Facts.FreeDistance >= 6.0)
	{
		return Decision("villa", Flags, TagBits);
	}
	if (Facts.IsAttached() && Facts.RowSize == 2)
	{
		return Decision("semidetached", Flags, TagBits);
	}
	if (Area >= 100.0 && Facts.BuiltRatio > 0.10 && Facts.Uniform(17) < 0.2 && Footprint.Aspect >= 1.2)
	{
		return Decision("postwar_plaster", Flags, TagBits);
	}
	return Decision("detached_postwar", Flags, TagBits);
}

/** Apartment buildings and large houses that are not in a closed frontage. */
FClassDecision ApartmentOrBigHouse(const FBuildingFacts& Facts, int Flags, int TagBits, bool bHeritage)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	if (bHeritage && Facts.FreeDistance >= 6.0)
	{
		return Decision("villa", Flags, TagBits);
	}
	if (Facts.Area >= 420.0 && Facts.FreeDistance >= 8.0 && Facts.LevelsOr(0.0) <= 2.0 && Facts.BuiltRatio < 0.12
		&& (Facts.Landuse.empty() || IsOneOf(Facts.Landuse, {"farmland", "farmyard"})))
	{
		return Decision("vierlande_farmhouse", Flags | FlagBarn, TagBits);
	}
	const std::string* RoofTag = FindTag(Facts.Tags, "roof:shape");
	if (RoofTag != nullptr && *RoofTag == "flat" && Facts.LevelsOr(3.0) >= 3.0)
	{
		return Decision(Facts.Draw(14, {{"modern", 5}, {"postwar_plaster", 3}}), Flags, TagBits);
	}
	const bool bLongBlock = Footprint.LongLength >= 28.0 && Footprint.ShortLength <= 16.0;
	if (bLongBlock && Facts.IsUrban() && Facts.Area >= 300.0)
	{
		return Decision(Facts.Draw(15, {{"brick_block_1920s", 4}, {"postwar_plaster", 4}, {"slab_block", 1}}), Flags, TagBits);
	}
	if (Facts.Area >= 140.0 && Footprint.Rectangularity < 0.82 && Footprint.CornerCount >= 8 && Facts.FreeDistance >= 8.0)
	{
		return Decision("villa", Flags, TagBits);
	}
	if ((Facts.HasLevels() && *Facts.Levels >= 4.0) || Facts.Area >= 320.0)
	{
		return Decision(Facts.Draw(16, {{"postwar_plaster", 5}, {"brick_block_1920s", 2}, {"modern", 1}}), Flags, TagBits);
	}
	if (Facts.Area >= 130.0 && Facts.BuildingTag == "apartments")
	{
		return Decision("postwar_plaster", Flags, TagBits);
	}
	return DetachedOrVilla(Facts, Flags, TagBits, bHeritage);
}

/** Housing: the order of the checks is from the most specific evidence to the default house. */
FClassDecision Residential(const FBuildingFacts& Facts, bool bShop, int Flags, int TagBits)
{
	const std::string_view Tag = Facts.BuildingTag;
	const bool bHeritage = HasTag(Facts.Tags, "heritage");
	if (Tag == "terrace")
	{
		if (Facts.HasLevels() && *Facts.Levels >= 4.0 && Facts.Frame.Width >= 7.0)
		{
			return Decision("gruenderzeit_clinker", Flags, TagBits | TagClass);
		}
		return Decision("terraced", Flags, TagBits | TagClass);
	}
	if (IsOneOf(Tag, {"semidetached_house", "duplex", "semi"}))
	{
		return Decision("semidetached", Flags, TagBits | TagClass);
	}
	if (IsOldTownHouse(Facts))
	{
		return Decision("halftimbered_town", Flags | ((bShop || IsShopStreetFrontage(Facts)) ? FlagShopGroundFloor : 0), TagBits);
	}
	if (bShop || IsShopStreetFrontage(Facts))
	{
		return Decision("commercial_groundfloor", Flags | FlagShopGroundFloor, TagBits);
	}
	if (Facts.Year.has_value())
	{
		const std::string_view ByDate = ClassByYear(Facts, *Facts.Year);
		if (!ByDate.empty())
		{
			return Decision(ByDate, Flags, TagBits);
		}
	}
	if (IsTowerOrSlab(Facts))
	{
		const bool bTower = Facts.Footprint.Aspect < 1.8 && Facts.Area >= 220.0;
		return Decision("slab_block", Flags | (bTower ? FlagTower : 0), TagBits);
	}
	if (Facts.Repeats >= EstateRepeats && IsOneOf(Tag, {"apartments", "residential", "house", "yes"})
		&& Facts.LevelsOr(3.0) >= 3.0 && Facts.Area > 90.0)
	{
		return Decision("modern", Flags, TagBits);
	}
	if (Facts.RowSize == 2 && 6.0 <= Facts.Frame.Width && Facts.Frame.Width <= 10.0 && Facts.Area <= 160.0
		&& Facts.LevelsOr(2.0) <= 3.0 && !Facts.IsUrban())
	{
		return Decision("semidetached", Flags, TagBits);
	}
	if (IsRowHouse(Facts))
	{
		return Decision("terraced", Flags, TagBits);
	}
	if ((Facts.IsAttached() || Facts.RowSize >= 2) && Facts.IsUrban() && Facts.Area >= 60.0)
	{
		return UrbanBlockClass(Facts, Flags, TagBits, bHeritage);
	}
	if (Tag == "apartments" || (Facts.HasLevels() && *Facts.Levels >= 3.0) || Facts.Area >= 260.0)
	{
		return ApartmentOrBigHouse(Facts, Flags, TagBits, bHeritage);
	}
	return DetachedOrVilla(Facts, Flags, TagBits, bHeritage);
}

/** The class name, flag bits and tag bits for a building; the tag rules come first, then geometry. */
FClassDecision DecideClass(const FBuildingFacts& Facts)
{
	const std::string_view Tag = Facts.BuildingTag;
	int Flags = 0;
	int TagBits = 0;
	if (IsTruthy(Facts.Year.has_value() ? std::optional<double>(*Facts.Year) : std::nullopt))
	{
		TagBits |= TagStartDate;
	}
	if (IsTruthy(Facts.Levels))
	{
		TagBits |= TagLevels;
	}
	const std::string* Amenity = FindTag(Facts.Tags, "amenity");
	const bool bShop = HasTag(Facts.Tags, "shop") || (Amenity != nullptr && Contains(ShopAmenities, *Amenity))
		|| HasTag(Facts.Tags, "office");
	const std::string* ShopValue = FindTag(Facts.Tags, "shop");
	const bool bBigShop = (ShopValue != nullptr && Contains(BigShopValues, *ShopValue)) || Facts.Landuse == "retail";
	if (bBigShop && Facts.Area >= 450.0 && !Contains(OutbuildingTags, Tag) && !Contains(PublicTags, Tag))
	{
		return Decision("retail_centre", Flags, TagBits | (Tag != "yes" ? TagClass : 0));
	}
	if (Contains(OutbuildingTags, Tag))
	{
		return Decision("shed_garage", Flags, TagBits | TagClass);
	}
	if (Contains(PublicTags, Tag) || (Amenity != nullptr && Contains(PublicAmenities, *Amenity)))
	{
		return Decision("public", Flags, TagBits | TagClass);
	}
	if (Contains(IndustrialTags, Tag))
	{
		return Decision("industrial_hall", Flags, TagBits | TagClass);
	}
	if (Contains(CommercialTags, Tag))
	{
		return CommercialOrHall(Facts, Flags, TagBits | TagClass);
	}
	if (Contains(FarmTags, Tag))
	{
		return Farm(Facts, Flags, TagBits | TagClass);
	}
	if (!Contains(ResidentialTags, Tag))
	{
		return Decision(Facts.Area < 60.0 ? "shed_garage" : "public", Flags, TagBits);
	}
	// Untagged or residential from here on.
	if (Tag == "yes")
	{
		std::string_view GuessedClass;
		int ExtraFlags = 0;
		if (GuessUntagged(Facts, GuessedClass, ExtraFlags))
		{
			return Decision(GuessedClass, Flags | ExtraFlags, TagBits);
		}
	}
	return Residential(Facts, bShop, Flags, TagBits);
}

/** The logit of a probability, which is held between 0.02 and 0.98 first. */
double LogitOf(double Probability)
{
	Probability = std::min(std::max(Probability, 0.02), 0.98);
	return std::log(Probability / (1.0 - Probability));
}

/** Chance of a flat roof: the class prior moved by how the footprint area changes it (big footprints are flat). */
double FlatRoofProbability(const FBuildingFacts& Facts, std::string_view ClassName)
{
	const FWeightedNames& Prior = RoofPriors.at(ClassName);
	double FlatWeight = 0.0;
	double TotalWeight = 0.0;
	for (const auto& [Name, Weight] : Prior)
	{
		TotalWeight += Weight;
		if (Name == "flat")
		{
			FlatWeight += Weight;
		}
	}
	const double ClassShare = FlatWeight / TotalWeight;
	const double LogArea = std::log2(std::max(Facts.Area, 8.0));
	const double AreaShare = Interpolate(LogArea, FlatShareByArea);
	const double Weight = Contains(TownClasses, ClassName) ? TownAreaEvidenceWeight : AreaEvidenceWeight;
	const double Shift = Weight * (LogitOf(AreaShare) - LogitOf(OverallFlatShare));
	return 1.0 / (1.0 + std::exp(-(LogitOf(ClassShare) + Shift)));
}

/** Roof shape: the OSM tag when it says something, otherwise a draw from the class prior, fitted to the plan. */
std::string_view RoofShapeName(const FBuildingFacts& Facts, std::string_view ClassName, bool& bFromTag)
{
	bFromTag = false;
	const std::string* Tag = FindTag(Facts.Tags, "roof:shape");
	if (Tag != nullptr)
	{
		const auto Known = OsmRoofShapes.find(*Tag);
		if (Known != OsmRoofShapes.end())
		{
			bFromTag = true;
			return Known->second;
		}
	}
	const int64_t Key = Facts.RoofDrawKey();
	if (Hash01(Key, 30) < FlatRoofProbability(Facts, ClassName))
	{
		return "flat";
	}
	FWeightedNames Pitched;
	for (const auto& Option : RoofPriors.at(ClassName))
	{
		if (Option.first != "flat")
		{
			Pitched.push_back(Option);
		}
	}
	std::string_view Name = WeightedPick(Key, 31, Pitched);
	const FFootprintShape& Footprint = Facts.Footprint;
	if (Name == "gabled" && Footprint.Aspect < 1.2 && Footprint.ShortLength < 16.0)
	{
		Name = (Footprint.Aspect < 1.08 && Footprint.ShortLength < 11.0) ? "pyramidal" : "hipped";
	}
	if (Footprint.ShortLength < 3.2 && Name != "skillion")
	{
		Name = "skillion";
	}
	return Name;
}

/** Roof pitch in degrees: the roof:angle tag, else a draw inside the class range. */
double RoofPitch(const FBuildingFacts& Facts, std::string_view ClassName, std::string_view RoofName)
{
	const std::optional<double> Tagged = ParseTagNumber(FindTag(Facts.Tags, "roof:angle"));
	if (Tagged.has_value() && 0.0 <= *Tagged && *Tagged <= 80.0)
	{
		return *Tagged;
	}
	if (RoofName == "flat")
	{
		return 0.0;
	}
	if (RoofName == "skillion")
	{
		return RangePick(Facts.OsmId, 32, 5, 15);
	}
	FPitchRange Range = ClassPitch.at(ClassName);
	if (IsOneOf(ClassName, {"shed_garage", "industrial_hall", "slab_block", "modern"}) && IsOneOf(RoofName, {"gabled", "hipped"}))
	{
		if (ClassName == "shed_garage")
		{
			Range = {18, 28};
		}
		else
		{
			Range = ClassName == "industrial_hall" ? FPitchRange{5, 25} : FPitchRange{20, 30};
		}
	}
	if (RoofName == "mansard")
	{
		return RangePick(Facts.OsmId, 32, 60, 70);
	}
	return RangePick(Facts.OsmId, 32, Range.Low, Range.High);
}

/** Adjusts the drawn storey count to the footprint: slabs and big blocks are taller than small houses. */
int FitStoreys(const FBuildingFacts& Facts, std::string_view ClassName, int Storeys)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	if (ClassName == "slab_block")
	{
		if (Footprint.Aspect < 1.6 && Footprint.ShortLength >= 16.0)
		{
			return 10 + static_cast<int>(Facts.Uniform(35) * 6); // point block
		}
		if (Footprint.LongLength >= 60.0)
		{
			return std::max(Storeys, 5);
		}
	}
	if (IsOneOf(ClassName, {"detached_postwar", "semidetached", "terraced"}) && Facts.Area < 70.0)
	{
		return 1;
	}
	if (ClassName == "industrial_hall")
	{
		return 1;
	}
	return Storeys;
}

/** Storeys, attic levels and tag bits from the tags when present, otherwise from the class prior. */
void ComputeStoreys(const FBuildingFacts& Facts, std::string_view ClassName, std::string_view RoofName, double Pitch,
	int& Storeys, int& Attic, int& TagBits)
{
	const FStoreyData& Data = ClassStorey.at(ClassName);
	const FStoreyPrior& Prior = StoreyPriors.at(ClassName);
	TagBits = 0;
	const bool bPitched = RoofName != "flat" && RoofName != "skillion";
	const std::optional<double> AtticTag = ParseTagNumber(FindTag(Facts.Tags, "roof:levels"));
	if (AtticTag.has_value())
	{
		Attic = bPitched ? static_cast<int>(std::min(std::max(*AtticTag, 0.0), 2.0)) : 0;
		TagBits |= TagRoofLevels;
	}
	else
	{
		Attic = (bPitched && Facts.Uniform(33) < Prior.AtticProbability) ? 1 : 0;
	}
	const std::optional<double> HeightTag = ParseTagNumber(FindTag(Facts.Tags, "height"));
	if (IsTruthy(Facts.Levels))
	{
		Storeys = std::max(1, RoundHalfEven(*Facts.Levels));
		TagBits |= TagLevels;
	}
	else if (IsTruthy(HeightTag) && 2.0 <= *HeightTag && *HeightTag <= 200.0)
	{
		const double Rise = bPitched ? Facts.Footprint.ShortLength / 2.0 * std::tan(Pitch * std::numbers::pi / 180.0) : 0.0;
		const double Eave = *HeightTag - Rise;
		Storeys = std::max(1, RoundHalfEven((Eave - Data.Plinth - Data.GroundHeight) / Data.StoreyHeight) + 1);
		TagBits |= TagHeight;
	}
	else
	{
		std::vector<std::pair<int, double>> Levels = Prior.Levels;
		Storeys = Levels[WeightedPickIndex(Facts.OsmId, 34, Levels)].first;
		Storeys = FitStoreys(Facts, ClassName, Storeys);
	}
}

/** Ridge direction (degrees, 0..180): along the row for attached buildings, else along the long side. */
double RidgeYaw(const FBuildingFacts& Facts)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	FWorldPoint Axis;
	if (Facts.IsAttached() || Facts.RowSize >= 3)
	{
		Axis = Facts.Frame.T;
	}
	else
	{
		Axis = Footprint.LongAxis;
		const std::string_view Orientation = TagOrEmpty(Facts.Tags, "roof:orientation");
		if (Orientation == "across")
		{
			Axis = Footprint.ShortAxis;
		}
		else if (Facts.Street.has_value() && Facts.StreetDistance < 35.0 && Footprint.Aspect < 1.5 && Orientation != "along")
		{
			Axis = Facts.Frame.T; // near-square plan: parallel to the street
		}
	}
	return PositiveModulo(YawDegrees(Axis), 180.0);
}

/** True when a second street of a clearly different direction passes within 22 m of the footprint of an attached building. */
bool IsCorner(const FBuildingFacts& Facts)
{
	if (!Facts.Street.has_value() || !Facts.IsAttached() || Facts.StreetDistance > 20.0)
	{
		return false;
	}
	const FBuildingTyper::FImpl& Typer = Facts.Typer;
	const FWorldPoint Centroid = Facts.Footprint.Centroid;
	return Typer.MainStreets.AnyWithin(Typer.Items[Facts.Index].Polygon, Typer.Boxes[Facts.Index], 22.0, [&](size_t Line) {
		const FMeasuredLine& Street = Typer.MainStreets.Lines[Line];
		const double Along = Street.Project(Centroid);
		const FWorldPoint Tangent = Unit(Subtract(Street.Interpolate(std::min(Along + 1.0, Street.Length)),
			Street.Interpolate(std::max(Along - 1.0, 0.0))));
		return std::abs(Dot(Tangent, Facts.Street->Tangent)) < 0.6;
	});
}

/** Fills roof, storeys, heights and orientation for the chosen class. */
FBuildingType CompleteType(const FBuildingFacts& Facts, std::string_view ClassName, int Flags, int TagBits)
{
	const FFootprintShape& Footprint = Facts.Footprint;
	bool bRoofFromTag = false;
	const std::string_view RoofName = RoofShapeName(Facts, ClassName, bRoofFromTag);
	if (bRoofFromTag)
	{
		TagBits |= TagRoofShape;
	}
	const double Pitch = RoofPitch(Facts, ClassName, RoofName);
	int Storeys = 0;
	int Attic = 0;
	int ExtraBits = 0;
	ComputeStoreys(Facts, ClassName, RoofName, Pitch, Storeys, Attic, ExtraBits);
	TagBits |= ExtraBits;
	const FStoreyData& Data = ClassStorey.at(ClassName);
	double StoreyHeight = Data.StoreyHeight;
	double GroundHeight = Data.GroundHeight;
	const double Plinth = Data.Plinth;
	double Eave = 0.0;
	if (ClassName == "industrial_hall")
	{
		const double HallHeight = 6.0 + 4.0 * Facts.Uniform(36);
		const std::optional<double> HeightTag = ParseTagNumber(FindTag(Facts.Tags, "height"));
		Eave = IsTruthy(HeightTag) ? Plinth + *HeightTag : Plinth + HallHeight;
		StoreyHeight = GroundHeight = Eave - Plinth;
	}
	else
	{
		Eave = Plinth + GroundHeight + (Storeys - 1) * StoreyHeight;
	}
	if (ClassName == "shed_garage")
	{
		Eave = Plinth + 2.6 + 0.5 * Facts.Uniform(37);
		StoreyHeight = GroundHeight = Eave - Plinth;
	}
	if (ClassName == "vierlande_farmhouse" && (Flags & FlagBarn) != 0)
	{
		Eave = Plinth + 3.2 + 1.2 * Facts.Uniform(38);
		Storeys = 1;
	}
	if (Attic != 0)
	{
		Flags |= FlagAtticHabitable;
	}
	if (Facts.Frame.bClosedLeft)
	{
		Flags |= FlagClosedLeft;
	}
	if (Facts.Frame.bClosedRight)
	{
		Flags |= FlagClosedRight;
	}
	if (Footprint.Rectangularity < 0.85 || Footprint.CornerCount > 6)
	{
		Flags |= FlagComplexFootprint;
	}
	if (IsCorner(Facts))
	{
		Flags |= FlagCorner;
	}
	if (ClassName == "commercial_groundfloor")
	{
		Flags |= FlagShopGroundFloor;
	}
	FBuildingType Type;
	Type.ClassId = ClassIdOf(ClassName);
	Type.RoofShape = RoofIdOf(RoofName);
	Type.PitchDegrees = Pitch;
	Type.Storeys = Storeys;
	Type.AtticLevels = Attic;
	Type.StoreyHeight = StoreyHeight;
	Type.GroundHeight = GroundHeight;
	Type.Plinth = Plinth;
	Type.EaveHeight = Eave;
	Type.RidgeYaw = RidgeYaw(Facts);
	Type.FrontYaw = YawDegrees({-Facts.Frame.N.X, -Facts.Frame.N.Y});
	Type.Flags = Flags;
	Type.TagBits = TagBits;
	return Type;
}
}

FBuildingType FBuildingTyper::Classify(size_t Index) const
{
	const std::optional<FStreetInfo> Street = Impl->NearestStreet(Index);
	const FFrame Frame = ComputeFrame(*Impl, Index, Street);
	const FBuildingFacts Facts(*Impl, Index, Street, Frame);
	const FClassDecision Decided = DecideClass(Facts);
	return CompleteType(Facts, Decided.ClassName, Decided.Flags, Decided.TagBits);
}
}
