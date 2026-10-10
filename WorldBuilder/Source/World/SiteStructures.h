#pragma once

#include <vector>

#include "FarStructures.h"
#include "Geometry.h"
#include "KitSpecs.h"
#include "OsmData.h"
#include "PolygonSet.h"
#include "RoadGraph.h"
#include "Structures.h"

/**
 * The kit's small structures, placed per tile (the STRU section of the tiles):
 *
 * - Bus stops: highway=bus_stop gets the red mast 0.5 m beyond the kerb, facing the oncoming traffic; shelter=yes adds
 *   a shelter beside it, open toward the road (glass in town, timber in the country).
 * - Bins: amenity=waste_basket gets a street bin on its post, facing the nearest road.
 * - Petrol stations: an amenity=fuel area becomes canopy, islands, pylon, shop and car wash laid out in its oriented
 *   rectangle with the front toward the road (Tools/buildingkit/kit/petrol.py's station_layout), and the OSM buildings
 *   inside it are dropped. A fuel node outside any area gets a standard plot behind the nearest road when it is clear.
 * - Greenhouses: building=greenhouse footprints that are nearly rectangular become Venlo glasshouses or foil tunnels
 *   (kit/greenhouses.py's glasshouse_parts and tunnel_parts); the OSM buildings are dropped.
 */
namespace WorldBuilder
{
/** A structure piece and how its height is found: on the pavement or the ground at the tile, or the Z it carries. */
struct FSitePlacement
{
	FStructurePlacement Placement;
	/** True when Z is the pavement or the conformed ground, found when the tile is written. */
	bool bFootHeight = false;
};

struct FSiteStructures
{
	std::vector<FSitePlacement> Placements;
	/** The footprints of OSM buildings the structures replace; buildings with their representative point in one are dropped. */
	FPolygons Replaced;
};

/** Counts per kind, for the build log. */
struct FSiteCounts
{
	int BusMasts = 0;
	int Shelters = 0;
	int Bins = 0;
	int Stations = 0;
	int Greenhouses = 0;
};

/**
 * Places the stops, bins, stations and greenhouses of the OSM data. Graph is the ground road graph, RoadGround the road
 * surface, Buildings the OSM footprints, Ground the terrain height. Counts, if given, receives how many of each.
 */
FSiteStructures BuildSiteStructures(const FOsmData& Osm, const FRoadGraph& Graph, const FPolygonSet& RoadGround,
									const FPolygonSet& Buildings, const FKitSpecs& Specs, const FGroundSampler& Ground,
									FSiteCounts* Counts = nullptr);
}
