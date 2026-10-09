#include "ExactFloatingPoint.h"
#include "BuildingRecords.h"

#include <algorithm>

#include "Buildings.h"
#include "ParallelFor.h"

namespace WorldBuilder
{
namespace
{
constexpr double MinimumFootprintArea = 4.0;
constexpr double SimplifyTolerance = 0.15;

/** The lowest ground height along the footprint's outline. */
double LowestGroundHeight(const FBuildingPolygon& Footprint, const FHeightSampler& SampleGround)
{
	double Lowest = SampleGround(Footprint.Outline.front().X, Footprint.Outline.front().Y);
	for (const FWorldPoint& Point : Footprint.Outline)
	{
		Lowest = std::min(Lowest, SampleGround(Point.X, Point.Y));
	}
	return Lowest;
}

/** The polygons of an OSM area, each made valid like osm.load does (a polygon can fall apart into several), then unioned when there are several outlines. */
std::vector<FBuildingPolygon> AreaGeometry(const FOsmArea& Area)
{
	std::vector<FBuildingPolygon> Pieces;
	for (const FOsmPolygon& Source : Area.Polygons)
	{
		if (Source.Rings.empty() || Source.Rings.front().size() < 3)
		{
			continue;
		}
		FBuildingPolygon Polygon;
		Polygon.Outline = Source.Rings.front();
		Polygon.Holes.assign(Source.Rings.begin() + 1, Source.Rings.end());
		if (IsSimplePolygon(Polygon))
		{
			Pieces.push_back(std::move(Polygon));
			continue;
		}
		for (FBuildingPolygon& Repaired : MakeValidPolygon(Polygon))
		{
			Pieces.push_back(std::move(Repaired));
		}
	}
	if (Area.Polygons.size() > 1)
	{
		return UnionPolygons(Pieces);
	}
	return Pieces;
}

/** The simplified footprint, or nothing when it is too small or stopped being one polygon. */
std::optional<FBuildingPolygon> SimplifiedFootprint(const FBuildingPolygon& Source)
{
	if (PolygonArea(Source) < MinimumFootprintArea)
	{
		return std::nullopt;
	}
	FBuildingPolygon Simplified = SimplifyPolygon(Source, SimplifyTolerance);
	if (!IsSimplePolygon(Simplified))
	{
		std::vector<FBuildingPolygon> Valid = MakeValidPolygon(Simplified);
		if (Valid.size() != 1)
		{
			return std::nullopt;
		}
		Simplified = std::move(Valid.front());
	}
	if (PolygonArea(Simplified) < MinimumFootprintArea)
	{
		return std::nullopt;
	}
	return Simplified;
}

/** Adds the usable footprints of one OSM area (it can fall apart into several polygons). */
void AppendAreaFootprints(const FOsmArea& Area, std::vector<FBuildingFootprint>& Footprints)
{
	for (const FBuildingPolygon& Part : AreaGeometry(Area))
	{
		std::optional<FBuildingPolygon> Simplified = SimplifiedFootprint(Part);
		if (Simplified.has_value())
		{
			Footprints.push_back({Area.Id, &Area.Tags, std::move(*Simplified)});
		}
	}
}

/** Everything for building number Index. */
FBuilding BuildOne(const FBuildingFootprint& Footprint, const FBuildingTyper& Typer, size_t Index, const FHeightSampler& SampleGround)
{
	FBuilding Building;
	Building.OsmId = Footprint.OsmId;
	Building.Footprint = Footprint.Polygon;
	Building.RepresentativePoint = RepresentativePoint(Footprint.Polygon);
	Building.Record = DecideBuildingRecord(Footprint.OsmId, *Footprint.Tags, Footprint.Polygon, SampleGround);
	Building.Type = Typer.Classify(Index);
	Building.Roof = RoofForBuilding(Footprint.Polygon, Building.Type.ClassId, RoofNames[static_cast<size_t>(Building.Type.RoofShape)],
		Building.Type.PitchDegrees);
	return Building;
}
}

std::vector<FBuildingFootprint> BuildingFootprints(const FOsmData& Data)
{
	std::vector<FBuildingFootprint> Footprints;
	for (const FOsmArea& Area : Data.Buildings)
	{
		AppendAreaFootprints(Area, Footprints);
	}
	return Footprints;
}

FBuildingRecord DecideBuildingRecord(int64_t OsmId, const FTags& Tags, const FBuildingPolygon& Footprint,
	const FHeightSampler& SampleGround)
{
	FBuildingRecord Record;
	Record.OsmId = OsmId;
	const FBuildingParams Params = BuildingParams(Tags, PolygonArea(Footprint));
	FacadeStyle(OsmId, Params.BuildingType, Record.Facade, Record.RoofSection);
	std::array<FWorldPoint, 4> Rectangle;
	const bool bHasRectangle = MinimumRotatedRectangle(Footprint, Rectangle);
	bool bRectangular = false;
	if (bHasRectangle)
	{
		const double RectangleArea = std::abs(SignedArea(FRing(Rectangle.begin(), Rectangle.end())));
		bRectangular = RectangleArea > 0.0 && PolygonArea(Footprint) / RectangleArea > 0.85 && Footprint.Holes.empty();
	}
	const bool bGabled = Params.Roof == "gabled" && bRectangular;
	Record.RoofShape = bGabled ? BuildingRoofGabled : BuildingRoofFlat;
	if (Record.RoofShape == BuildingRoofFlat && Record.RoofSection == "Roof_Tiles")
	{
		Record.RoofSection = "Roof_Flat";
	}
	Record.BaseZ = static_cast<float>(LowestGroundHeight(Footprint, SampleGround));
	Record.Tint = static_cast<uint8_t>(static_cast<int>(Hash01(OsmId, 1) * 255.0));
	Record.Variation = static_cast<uint8_t>(static_cast<int>(Hash01(OsmId, 2) * 255.0));
	Record.EaveHeight = static_cast<float>(Params.EaveHeight);
	if (bGabled)
	{
		Record.bHasRoofRectangle = true;
		Record.RoofRectangle = Rectangle;
	}
	return Record;
}

std::vector<FBuilding> BuildBuildings(const FOsmData& Data, const FHeightSampler& SampleGround, unsigned ThreadCount)
{
	const std::vector<FBuildingFootprint> Footprints = BuildingFootprints(Data);
	const FBuildingTyper Typer(Footprints, Data.Roads, Data.Footways, Data.Areas, ThreadCount);
	std::vector<FBuilding> Buildings(Footprints.size());
	ParallelFor(Footprints.size(), ResolveThreadCount(ThreadCount), [&](size_t Index) {
		Buildings[Index] = BuildOne(Footprints[Index], Typer, Index, SampleGround);
	});
	return Buildings;
}
}
