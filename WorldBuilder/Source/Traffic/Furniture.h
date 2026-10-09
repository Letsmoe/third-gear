#pragma once

#include "OsmData.h"
#include "PolygonSet.h"
#include "RoadGraph.h"
#include "TrafficTypes.h"

/**
 * Street furniture from OSM: traffic signal junctions with their approaches and phases, traffic signs, street lamps and
 * speed limits (Python: furniture.py).
 *
 * Everything is derived from the OSM points and the road graph of the street model:
 *
 * - A signalised junction is a cluster of junction nodes (OSM nodes with three or more road ends, merged when closer
 *   than JunctionMergeDistance) that has at least one highway=traffic_signals node on an arm or on the junction node
 *   itself. Every arm that vehicles can enter gets an approach with a stop line. Crossing signals on a plain road
 *   become their own small junction, so they get a pedestrian phase.
 * - Approaches that run (anti)parallel share a phase; crossing approaches get different phases. Timings follow German
 *   practice (amber 3 s at 50 km/h, all-red clearance).
 * - Signs: speed limit signs where a way's limit differs from its straight continuation, Halt and Vorfahrt gewaehren
 *   from highway=stop and give_way nodes, zebra crossing signs, and whatever traffic_sign=DE:* nodes name that we
 *   have a graphic for.
 * - Lamps: OSM street_lamp nodes, plus evenly spaced lamps along lit=yes roads where OSM has none.
 *
 * Coordinates: world metres, x east, y south. Right of a travel direction (dx, dy) is (-dy, dx).
 */
namespace WorldBuilder
{
/**
 * Derives all street furniture and the signal junctions. Data: the OSM points and footways; Graph: the ground ways of
 * the street model; RoadGround: the road surface; Buildings: the building footprints. Positions come without heights.
 */
FFurniture BuildFurniture(const FOsmData& Data, const FRoadGraph& Graph, const FPolygonSet& RoadGround,
						  const FPolygonSet& Buildings);
}
