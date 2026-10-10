#pragma once

#include <vector>

#include "FarStructures.h"
#include "KitSpecs.h"
#include "OsmData.h"
#include "RoadSurfaces.h"
#include "SiteStructures.h"

/**
 * The structure under and beside the bridges: the kit's segment pieces (Tools/buildingkit/kit/bridges.py) laid along
 * each deck.
 *
 * - Road bridges (the street model's bridge ways, whose flat decks are in the SURF section): a slab under the deck, an
 *   edge beam and a railing along both edges, guard rails on the main roads, an abutment with two wing walls at each end,
 *   and column piers every 20 m on long decks that stand high over something.
 * - Footbridges (footway, path, cycleway and pedestrian ways with bridge=*): the deck itself, planks on steel girders or
 *   on timber stringers and piles, railings, and bank seats. These ways get no draped path surface (see IsFootBridge).
 *
 * Railway bridges are not handled.
 */
namespace WorldBuilder
{
/** True for a footway, path, cycleway or pedestrian way with a bridge tag: BuildBridgeStructures gives it a deck. */
bool IsFootBridge(const FTags& Tags);

/** The pieces of every bridge, with absolute heights. Ground is the terrain height. */
std::vector<FSitePlacement> BuildBridgeStructures(const FOsmData& Osm, const FStreetModel& Model, const FRoadSurfaces& Roads,
												  const FKitSpecs& Specs, const FGroundSampler& Ground,
												  int* RoadBridgeCount = nullptr, int* FootBridgeCount = nullptr);
}
