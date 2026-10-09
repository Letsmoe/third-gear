#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "Gores.h"
#include "Layout.h"
#include "Lines.h"
#include "Network.h"

/**
 * The lines of a road network: every segment's kerbs and painted lines with their tapers and junction cuts, and the
 * guide lines through the junctions. Ties network.py, layout.py and junctions.py together (Python:
 * streets/road_lines.py).
 */
namespace WorldBuilder
{
// Speeds from which the long rural dashes are painted.
inline constexpr double RuralDashSpeed = 70.0;

/** The left and right kerb of a segment, each nothing when it is too short to draw. */
struct FKerbs
{
	std::optional<FStreetPolyline> Left;
	std::optional<FStreetPolyline> Right;
};

struct FRoadLines
{
	/** The ways the segments point into. */
	std::shared_ptr<const std::vector<FStreetWay>> Ways;
	std::shared_ptr<FSegmentNetwork> Network;
	/** One layout per segment id. */
	std::vector<FSegmentLayout> Layouts;
	/** Marking kind and points of the lines inside the junctions. */
	std::vector<FMarking> Guides;
	/** The gores of the dual carriageway splits. */
	std::vector<FGore> Gores;

	/** Every painted line: along the segments, then inside the junctions. */
	std::vector<FMarking> Painted() const;

	/** The left and right kerb of a segment as polylines, tapers included. */
	FKerbs Kerbs(const FSegmentLayout& Layout) const;
};

/**
 * The lines of the given ways. Sections: the cross-section of each way; Urban: whether each way lies in town (both by
 * way id); SignalPoints: where the traffic signals are, which decide where left turns get guide lines. The
 * corrections of short narrowings change the segments' sections, not the Sections passed in.
 */
FRoadLines BuildRoadLines(const std::shared_ptr<const std::vector<FStreetWay>>& Ways, const FSectionsByWay& Sections,
						  const std::unordered_map<int64_t, bool>& Urban, const std::vector<FStreetPoint>& SignalPoints);
}
