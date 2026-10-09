#pragma once

#include <vector>

#include "Layout.h"
#include "Network.h"

/**
 * The gore of every dual carriageway split: the paved wedge between the two carriageways where they open apart, marked
 * as a hatched area (Sperrfläche, Zeichen 298) between their inner edge lines (Python: streets/gores.py).
 */
namespace WorldBuilder
{
struct FGore
{
	/** Polygon between the two inner kerbs. */
	FStreetPolyline Paved;
	/** Polygon between the two inner edge lines. */
	FStreetPolyline Hatched;
	/** Inner edge lines of the gore that aren't painted already. */
	std::vector<FStreetPolyline> Outline;
};

/** The gores of all splits in the network. */
std::vector<FGore> SplitGores(const FSegmentNetwork& Network, const std::vector<FSegmentLayout>& Layouts);
}
