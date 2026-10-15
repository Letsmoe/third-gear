#pragma once

#include <optional>

#include "RoadLines.h"

/**
 * Stop lines on the street model: across the lanes of one direction where the street model draws them, so a stop line
 * at a split covers its own carriageway and not the gore beside it, and one at a junction sits where the lane lines
 * stop.
 */
namespace WorldBuilder
{
struct FPlacedStopLine
{
	/** On the way's centre line, where the stop line crosses it. */
	FStreetPoint Point;
	/** The direction of travel there. */
	FStreetPoint Direction;
	/** The ends of the line, on the left and the right of traffic. */
	FStreetPoint Left;
	FStreetPoint Right;
};

/**
 * The stop line for traffic on a way, travelling along its node order (Travel +1) or against it (-1), near a point.
 * It runs across that direction's lanes: from the centre line, or the left edge of a one-way carriageway, to the right
 * edge of the lanes or of the cycle lane beside them. It never lies inside the junction ahead, and with bAtMouth it
 * moves to where the lane lines stop before that junction. Nothing when no segment of the way is near.
 */
std::optional<FPlacedStopLine> PlaceStopLine(const FRoadLines& Lines, const FStreetWay& Way, const FStreetPoint& Near,
											 int Travel, bool bAtMouth);
}
