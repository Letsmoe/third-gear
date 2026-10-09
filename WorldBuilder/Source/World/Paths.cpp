#include "Paths.h"

#include "PolygonQuery.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

namespace WorldBuilder
{
namespace
{
const std::set<std::string> PavedSurfaces = {"paved", "asphalt", "paving_stones", "sett", "concrete",
											 "concrete:plates", "cobblestone", "bricks", "paving_stones:30", "metal",
											 "wood"};
/** Smallest water body kept, square metres. */
constexpr double MinimumWaterArea = 20.0;
/** Water levels are read on a grid of this spacing inside the body, banks excluded, ...*/
constexpr double LevelSampleSpacing = 2.0;
constexpr double BankExclusion = 2.0;
/** ...but from at most this many samples, so the Elbe doesn't take millions. */
constexpr double MaximumLevelSamples = 200000.0;

/** Footway widths by highway value, metres; anything else is 1.5 m. */
double FootWidth(const std::string& Highway)
{
	if (Highway == "pedestrian") return 8.0;
	if (Highway == "footway" || Highway == "cycleway") return 2.0;
	if (Highway == "bridleway") return 2.5;
	if (Highway == "track") return 3.0;
	return 1.5;
}

/** River widths by waterway value, metres; 0 for waterways that aren't drawn as water. */
double RiverWidth(const std::string& Waterway)
{
	if (Waterway == "river") return 12.0;
	if (Waterway == "canal") return 8.0;
	if (Waterway == "stream") return 2.0;
	return 0.0;
}

bool TagIs(const FTags& Tags, const char* Key, const char* Value)
{
	const std::string* Found = FindTag(Tags, Key);
	return Found != nullptr && *Found == Value;
}

bool TagIn(const FTags& Tags, const char* Key, std::initializer_list<const char*> Values)
{
	const std::string* Found = FindTag(Tags, Key);
	if (Found == nullptr)
	{
		return false;
	}
	return std::any_of(Values.begin(), Values.end(), [&](const char* Value) { return *Found == Value; });
}

/** An OSM area's polygons as one even-odd normalised path set. */
FPolygons AreaPaths(const FOsmArea& Area)
{
	std::vector<FPolygonWithHoles> Polygons;
	for (const FOsmPolygon& Polygon : Area.Polygons)
	{
		Polygons.push_back({Polygon.Rings});
	}
	return Clipper2Lib::Union(ToPaths(Polygons), Clipper2Lib::FillRule::EvenOdd, 4);
}

bool IsWaterArea(const FTags& Tags)
{
	return TagIs(Tags, "natural", "water") || TagIn(Tags, "waterway", {"riverbank", "dock"})
		|| FindTag(Tags, "water") != nullptr;
}

/** A point inside the polygons: the middle of the widest inside run across their bounds' middle row. */
FWorldPoint InsidePoint(const FPolygons& Polygons)
{
	const FBox Bounds = BoundsOf(Polygons);
	const double Y = (Bounds.Y0 + Bounds.Y1) / 2.0;
	std::vector<double> Crossings;
	for (const Clipper2Lib::PathD& Ring : Polygons)
	{
		for (size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const Clipper2Lib::PointD& Start = Ring[Index];
			const Clipper2Lib::PointD& End = Ring[(Index + 1) % Ring.size()];
			if ((Start.y <= Y) != (End.y <= Y))
			{
				Crossings.push_back(Start.x + (Y - Start.y) / (End.y - Start.y) * (End.x - Start.x));
			}
		}
	}
	std::sort(Crossings.begin(), Crossings.end());
	FWorldPoint Best{(Bounds.X0 + Bounds.X1) / 2.0, Y};
	double Widest = -1.0;
	for (size_t Pair = 0; Pair + 1 < Crossings.size(); Pair += 2)
	{
		if (Crossings[Pair + 1] - Crossings[Pair] > Widest)
		{
			Widest = Crossings[Pair + 1] - Crossings[Pair];
			Best = {(Crossings[Pair] + Crossings[Pair + 1]) / 2.0, Y};
		}
	}
	return Best;
}

/**
 * The level of a water body: the survey has few, low points over water and interpolates a flat surface there, so a
 * low percentile of the heights inside, banks excluded.
 */
double WaterLevel(const FPolygons& Polygon, const FTerrainGrid& Terrain)
{
	FPolygons Inner = OffsetPolygons(Polygon, -BankExclusion, 4);
	if (Inner.empty())
	{
		Inner = Polygon;
	}
	const FBox Bounds = BoundsOf(Inner);
	const double Area = (Bounds.X1 - Bounds.X0) * (Bounds.Y1 - Bounds.Y0);
	const double Spacing = std::max(LevelSampleSpacing, std::sqrt(Area / MaximumLevelSamples));
	const FPolygonQuery InnerQuery(Inner);
	std::vector<double> Heights;
	for (double Y = Bounds.Y0; Y < Bounds.Y1; Y += Spacing)
	{
		for (double X = Bounds.X0; X < Bounds.X1; X += Spacing)
		{
			if (InnerQuery.Contains(X, Y))
			{
				Heights.push_back(Terrain.Sample(X, Y));
			}
		}
	}
	if (Heights.empty())
	{
		const FWorldPoint Inside = InsidePoint(Inner);
		return Terrain.Sample(Inside.X, Inside.Y);
	}
	// numpy's default percentile: linear between the two nearest ranks.
	std::sort(Heights.begin(), Heights.end());
	const double Rank = 0.25 * (Heights.size() - 1);
	const size_t Lower = static_cast<size_t>(std::floor(Rank));
	const size_t Upper = std::min(Lower + 1, Heights.size() - 1);
	return Heights[Lower] + (Heights[Upper] - Heights[Lower]) * (Rank - Lower);
}
}

double TagNumber(const std::string* Value)
{
	if (Value == nullptr)
	{
		return 0.0;
	}
	std::string Cleaned;
	for (const char Character : *Value)
	{
		if (Character == 'm' || Character == ' ')
		{
			continue;
		}
		Cleaned.push_back(Character == ',' ? '.' : Character);
	}
	char* End = nullptr;
	const double Number = std::strtod(Cleaned.c_str(), &End);
	if (Cleaned.empty() || End != Cleaned.c_str() + Cleaned.size() || !std::isfinite(Number))
	{
		return 0.0;
	}
	return Number;
}

std::vector<FWaterBody> FWaterBodies::InWindow(const FBox& Window) const
{
	std::vector<FWaterBody> Result;
	for (auto& [Owner, Polygon] : Pieces.InWindow(Window))
	{
		Result.push_back({std::move(Polygon), Levels[Owner]});
	}
	return Result;
}

FWaterBodies BuildWaterBodies(const FOsmData& Osm, const FTerrainGrid& Terrain, const FBox& Extent)
{
	FPolygons Water;
	for (const FOsmArea& Area : Osm.Areas)
	{
		if (IsWaterArea(Area.Tags))
		{
			const FPolygons Paths = AreaPaths(Area);
			Water.insert(Water.end(), Paths.begin(), Paths.end());
		}
	}
	for (const FOsmWay& Way : Osm.Waterways)
	{
		const std::string* Kind = FindTag(Way.Tags, "waterway");
		const double DefaultWidth = Kind == nullptr ? 0.0 : RiverWidth(*Kind);
		const std::string* Tunnel = FindTag(Way.Tags, "tunnel");
		if (DefaultWidth <= 0.0 || (Tunnel != nullptr && *Tunnel != "no"))
		{
			continue;
		}
		const double TaggedWidth = TagNumber(FindTag(Way.Tags, "width"));
		const double Width = TaggedWidth != 0.0 ? TaggedWidth : DefaultWidth;
		// Ditches and narrow streams stay terrain dips.
		if (*Kind == "stream" && Width < 3.0)
		{
			continue;
		}
		const FPolygons River = BufferPolyline(Way.Points, Width / 2.0, ECapStyle::Flat, 8);
		Water.insert(Water.end(), River.begin(), River.end());
	}

	FWaterBodies Result;
	const FPolygons Merged = ClipToBox(UnionOf(Water), Extent);
	for (const FPolygonWithHoles& Polygon : ToPolygons(Merged))
	{
		if (PolygonArea(Polygon) < MinimumWaterArea)
		{
			continue;
		}
		const FPolygons Paths = ToPaths({Polygon});
		Result.Pieces.Add(static_cast<int>(Result.Levels.size()), Paths);
		Result.Levels.push_back(WaterLevel(Paths, Terrain));
	}
	return Result;
}

FPathSurfaces::FPathSurfaces(const FOsmData& Osm)
{
	for (const FOsmWay& Way : Osm.Footways)
	{
		const FTags& Tags = Way.Tags;
		const std::string* Highway = FindTag(Tags, "highway");
		const std::string* Tunnel = FindTag(Tags, "tunnel");
		if (TagIs(Tags, "highway", "steps") || TagIn(Tags, "footway", {"sidewalk", "crossing"})
			|| (Tunnel != nullptr && *Tunnel != "no"))
		{
			continue;
		}
		if (TagIn(Tags, "access", {"private", "no"}) && TagIs(Tags, "highway", "track"))
		{
			continue;
		}
		const double TaggedWidth = TagNumber(FindTag(Tags, "width"));
		const double Width = TaggedWidth != 0.0 ? TaggedWidth : FootWidth(*Highway);
		const std::string* Surface = FindTag(Tags, "surface");
		const bool bPaved = Surface != nullptr ? PavedSurfaces.count(*Surface) > 0
											   : TagIn(Tags, "highway", {"pedestrian", "footway", "cycleway"});
		Pieces.Add(bPaved ? 1 : 0, BufferPolyline(Way.Points, Width / 2.0, ECapStyle::Round, 3));
	}
	for (const FOsmArea& Area : Osm.Areas)
	{
		const FTags& Tags = Area.Tags;
		const bool bPathArea = TagIn(Tags, "highway", {"pedestrian", "footway", "service"})
			|| FindTag(Tags, "area:highway") != nullptr || TagIs(Tags, "place", "square") || TagIs(Tags, "amenity", "parking");
		if (!bPathArea || TagIn(Tags, "parking", {"underground", "multi-storey", "rooftop"}))
		{
			continue;
		}
		const std::string* Surface = FindTag(Tags, "surface");
		Pieces.Add(Surface == nullptr || PavedSurfaces.count(*Surface) > 0 ? 1 : 0, AreaPaths(Area));
	}
}

void FPathSurfaces::InWindow(const FBox& Window, const FPolygons& Blocked, const FPolygons& Water, FPolygons& Paved,
							 FPolygons& Unpaved) const
{
	FPolygons PavedPieces;
	FPolygons UnpavedPieces;
	for (auto& [Owner, Polygon] : Pieces.InWindow(Window))
	{
		(Owner == 1 ? PavedPieces : UnpavedPieces) = std::move(Polygon);
	}
	const FPolygons PavedArea = Difference(PavedPieces, Blocked);
	const FPolygons UnpavedArea = Difference(Difference(UnpavedPieces, Blocked), PavedArea);
	Paved = Difference(PavedArea, Water);
	Unpaved = Difference(UnpavedArea, Water);
}
}
