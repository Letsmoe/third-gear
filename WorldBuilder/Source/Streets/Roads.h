#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "CrossSection.h"
#include "Gores.h"
#include "Lines.h"
#include "Network.h"
#include "OsmData.h"
#include "RoadLines.h"

/**
 * The street parts of roads.py: which ways are streets at ground level, whether buildings line them, their sections,
 * the redrawn dual carriageways, the lines and the markings (Python: roads.build up to the surface polygons, and
 * build_markings without its final clip to the road surface).
 */
namespace WorldBuilder
{
/** The buildings of the area, to tell whether a road runs through town. */
class FBuildingIndex
{
public:
	explicit FBuildingIndex(const std::vector<FOsmArea>& Buildings);

	/** True when the area has no buildings at all (the street model then treats every road as urban). */
	bool IsEmpty() const;

	/** True when a building lies within Distance of the polyline. */
	bool IsNear(const FStreetPolyline& Line, double Distance) const;

private:
	/** One building polygon: outline first, then holes. */
	struct FPolygon
	{
		std::vector<FStreetPolyline> Rings;
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
	};

	std::vector<FPolygon> Polygons;
	std::unordered_map<int64_t, std::vector<int>> Cells;

	bool IsPolygonNear(const FPolygon& Polygon, const FStreetPolyline& Line, double Distance) const;
};

/** True for a road in a tunnel or below ground (layer < 0), which has no surface lines. */
bool IsTunnel(const FStreetWay& Way);

/** True for a road on the ground: no bridge, no tunnel (building passages count as ground), layer 0 or more. */
bool IsGround(const FStreetWay& Way);

/** True when buildings line the road: slower than 70 km/h and a building within 35 m of the way. */
bool IsUrban(const FStreetWay& Way, const FBuildingIndex& Buildings);

/**
 * Ways made of a single segment take its cross-section after the corrections of the lines (in place), so furniture,
 * parking and AI lanes see the same road as the surface.
 */
void TakeCorrectedSections(const FRoadLines& Lines, FSectionsByWay& Sections,
						   std::unordered_map<int64_t, double>& Widths);

/** The gores' hatching and their outlines where the carriageways paint no edge line. */
std::vector<FMarking> GoreMarkings(const std::vector<FGore>& Gores);

/** Straight stripes across a hatched polygon at HatchAngleDegrees to its long axis, HatchSpacing apart. */
std::vector<FStreetPolyline> HatchStripes(const FStreetPolyline& Area);

/** What the street model makes of a region's roads. */
struct FStreetModel
{
	/** Cross-section of every road way that is not a tunnel, after the corrections, by way id. */
	FSectionsByWay Sections;
	/** Kerb to kerb width of those ways, by way id. */
	std::unordered_map<int64_t, double> Widths;
	/** Whether buildings line each of those ways, by way id. */
	std::unordered_map<int64_t, bool> Urban;
	/** The ground ways with their dual carriageways redrawn; the segments of Lines point into them. */
	std::shared_ptr<const std::vector<FStreetWay>> GroundWays;
	/** The bridge ways. */
	std::vector<FStreetWay> BridgeWays;
	FRoadLines Lines;
	/** Every painted line and the gores' hatching, before the clip to the road surface. */
	std::vector<FMarking> Markings;
};

/** Builds the street model of the roads in the OSM data. */
FStreetModel BuildStreetModel(const FOsmData& Data);
}
