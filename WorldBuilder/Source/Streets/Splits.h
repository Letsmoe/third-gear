#pragma once

#include <optional>

#include "Network.h"

/**
 * Dual carriageway splits: where a two-way road divides into two one-way carriageways, or two join into one (Python:
 * streets/splits.py).
 *
 * OSM draws the two carriageways as one-way ways meeting the two-way road at one node. The one-way carriageway whose
 * traffic drives into the two-way road continues that road's lanes on one side, the one whose traffic comes out of it
 * the lanes on the other side; the centre line opens into the inner edges of the two carriageways.
 */
namespace WorldBuilder
{
struct FSplit
{
	FSegmentEnd TwoWay;
	/** The carriageway whose traffic comes to the node and drives on along the two-way road. */
	FSegmentEnd Incoming;
	/** The carriageway whose traffic comes from the two-way road and leaves the node. */
	FSegmentEnd Outgoing;
	/** How far from the node the two carriageways have separated. */
	double GoreLength = 0.0;
};

/**
 * The split at a junction node, or nothing when the node is not one: exactly one two-way road and two one-way
 * carriageways (one in, one out) carrying it on nearly straight; driveways and the like may join as well.
 */
std::optional<FSplit> FindSplit(const FSegmentNetwork& Network, int64_t Node);
}
