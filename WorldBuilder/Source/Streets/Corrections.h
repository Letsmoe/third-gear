#pragma once

#include <optional>

#include "Network.h"

/**
 * Fixes for OSM mistakes the model can recognise from the network around a road (Python: streets/corrections.py).
 *
 * A short road piece narrower than the road on both sides of it, which continue it straight on and are the same kind
 * of road, is mis-tagged (a lane count missing on a short way, say): it takes the narrower neighbour's cross-section.
 */
namespace WorldBuilder
{
// Repeats of the narrowing fix, so a run of several short mis-tagged pieces is filled in from both ends.
inline constexpr int NarrowingPasses = 3;

/**
 * Gives every short piece narrower than both its straight neighbours the narrower neighbour's cross-section, in
 * place; returns how many pieces changed.
 */
int FixShortNarrowings(FSegmentNetwork& Network);

/**
 * The segment end that continues this one straight on at its node: the other end at a continuation, the arm going
 * straight on at a junction (driveways and the like don't count), or nothing.
 */
std::optional<FSegmentEnd> StraightNeighbour(const FSegmentNetwork& Network, const FSegmentEnd& End);

/** True for driveways and similar arms, which don't interrupt the lines of the road they join. */
bool IsMinor(const FSegmentNetwork& Network, const FSegmentEnd& End);

/** How many degrees a path from one arm into the other turns (0 for a straight continuation). */
double Deflection(const FSegmentNetwork& Network, const FSegmentEnd& First, const FSegmentEnd& Second);
}
