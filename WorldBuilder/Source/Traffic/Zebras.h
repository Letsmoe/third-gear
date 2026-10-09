#pragma once

#include <optional>
#include <vector>

#include "PolygonSet.h"
#include "TrafficTypes.h"

/** Zebra crossings and stop lines as markings (Python: build_world.py's zebra_* and stop_line_geometry). */
namespace WorldBuilder
{
inline constexpr double ZebraBarWidth = 0.5;
inline constexpr double ZebraBarLength = 3.5;
inline constexpr double ZebraBarLengthWideRoad = 4.0;
inline constexpr double ZebraWideRoad = 7.5;
inline constexpr double ZebraWidthSlack = 1.0;
inline constexpr double StopLineWidth = 0.5;

/** A striped span of a zebra: the carriageway interval across the road and the interval its bars cover. */
struct FZebraSpan
{
	double Low = 0.0;
	double High = 0.0;
	double BarLow = 0.0;
	double BarHigh = 0.0;
};

/** The white bars of one zebra crossing. */
struct FZebraBars
{
	/** Centre lines of the bars, each ZebraBarWidth wide. */
	std::vector<FStreetPolyline> Bars;
	/** The convex outline of the whole crossing, which the other markings are cut by. */
	FPolygons Outline;
	std::vector<FZebraSpan> Spans;
};

/**
 * Lateral intervals (low, high), metres across the road from the crossing node, where the crossing line runs over the
 * carriageway within reach of the node. A split carriageway gives one interval per side of the island. The intervals
 * end ZebraWidthSlack beyond the crossed road's own width: where a slip lane opens into a junction the carriageway goes
 * on across the whole junction, but the zebra only crosses the slip lane.
 */
std::vector<std::pair<double, double>> ZebraSpans(const FZebra& Zebra, const FPolygonSet& Carriageway);

/**
 * The white bars of one zebra crossing (Fussuebergang): 0.5 m bars with 0.5 m gaps, parallel to the road, filling the
 * carriageway from kerb to kerb.
 */
FZebraBars MakeZebraBars(const FZebra& Zebra, const FPolygonSet& Carriageway);

/**
 * Centre line of the painted stop line (Haltlinie) of a signal approach, just before the signal's line of sight, or
 * nothing when the approach is too narrow for one.
 */
std::optional<FStreetPolyline> StopLineGeometry(const FApproachRecord& Approach);
}
