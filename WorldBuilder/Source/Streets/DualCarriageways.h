#pragma once

#include <vector>

#include "Network.h"

/**
 * Short dual carriageway sections redrawn parallel: a correction of the OSM geometry (Python:
 * streets/dual_carriageways.py).
 *
 * Where a two-way road splits into two one-way carriageways that meet again a little further on (around a median
 * island before a junction, say), OSM draws both carriageways from single nodes at either end, and the measured lines
 * bend and wobble between. Real carriageways there run parallel: this redraws both around their common axis at their
 * measured distance, opening over the gore at the split and over a few metres at the far end.
 *
 * The section's end nodes stay where they are; the nodes in between move, in every way that uses them, and the
 * carriageways get extra shape points so their curves are smooth.
 */
namespace WorldBuilder::DualCarriageways
{
// Shape points added along a redrawn carriageway, and the search length for the far end of a section.
inline constexpr double PointSpacing = 2.0;
inline constexpr double MaxSectionLength = 300.0;
// Sections whose carriageways are this far apart (median, centre line to centre line) are redrawn.
inline constexpr double SeparationLow = 4.0;
inline constexpr double SeparationHigh = 30.0;
// The two carriageways' lengths may differ by this share at most.
inline constexpr double MaxLengthDifference = 0.3;
// The common axis is taken straight when it bows less than this from the line between the end nodes.
inline constexpr double MaxStraightBow = 3.0;
inline constexpr int AxisSmoothingPasses = 20;

/** The ways with every short dual carriageway section redrawn parallel (new ways for changed ways). */
std::vector<FStreetWay> Straighten(const std::vector<FStreetWay>& Ways, const FSectionsByWay& Sections);
}
