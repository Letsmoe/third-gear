#pragma once

#include <string>
#include <vector>

#include "Geometry.h"
#include "HeightGrid.h"
#include "OsmData.h"
#include "PolygonSet.h"
#include "Roads.h"
#include "SpatialIndex.h"

/**
 * The road surfaces of roads.py: each way's carriageway from its segments (tapered where the street model tapers it),
 * the gores, the kerb radii at junctions, the surface material by road, bridges, pavements, and the painted lines
 * kept on the road. Everything a tile needs is kept as pieces answered as their union (FPolygonSet), so no polygon
 * of the whole region is ever merged.
 */
namespace WorldBuilder
{
/** A painted line clipped to the road, with its kind (a MARKING_STYLE key). */
struct FMarkingLine
{
	std::string Kind;
	FPolyline Points;
};

/** A bridge deck: its polygon and the straight ramp between the road heights at its two ends. */
struct FBridgeDeck
{
	FPolygons Polygon;
	FWorldPoint Start;
	FWorldPoint End;
	double StartHeight = 0.0;
	double EndHeight = 0.0;
};

/** The surface of one window: the ground road surface by material, and the pavement. */
struct FWindowRoads
{
	FPolygons Ground;
	FPolygons Asphalt;
	FPolygons Pavers;
	FPolygons Cobble;
	FPolygons Pavement;
};

class FRoadSurfaces
{
public:
	/**
	 * Builds the surfaces of the street model. Bridge ramps need the road height at their ends, from the terrain
	 * around them; Buildings is the footprints pavements stay clear of.
	 */
	FRoadSurfaces(const FStreetModel& Model, const FTerrainGrid& Terrain, const FPolygonSet& Buildings, unsigned Threads);

	/** The ground road surface as pieces answered as their union (Python's net.ground). */
	const FPolygonSet& Ground() const { return GroundPieces; }

	/** The road surfaces and the pavement of a window, clipped to it. */
	FWindowRoads InWindow(const FBox& Window) const;

	/** The bridges overlapping a box. */
	std::vector<const FBridgeDeck*> BridgesNear(const FBox& Box) const;

	/** The painted lines kept on the road (roads.build_markings), longer than 2 m, overlapping a box. */
	std::vector<const FMarkingLine*> MarkingsNear(const FBox& Box) const;

	/** Every painted line kept on the road. */
	const std::vector<FMarkingLine>& Markings() const { return MarkingLines; }

private:
	FPolygonSet GroundPieces;
	FPolygonSet AsphaltPieces;
	FPolygonSet PaverPieces;
	FPolygonSet CobblePieces;
	FPolygonSet PavementBands;
	const FPolygonSet* BuildingPieces = nullptr;
	std::vector<FBridgeDeck> Bridges;
	FSpatialIndex BridgeIndex;
	std::vector<FMarkingLine> MarkingLines;
	FSpatialIndex MarkingIndex;
};

/** The road height field of roads.py at one point, from the terrain and the road surface around it. */
double RoadHeightAt(const FTerrainGrid& Terrain, const FPolygonSet& Ground, double X, double Y);

/** world.json's start pose: a spot on a decent road near the origin, half a lane right of the centre, facing along it. */
struct FStartPose
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	double Yaw = 0.0;
	std::string Road;
};
FStartPose FindStart(const FStreetModel& Model, const FTerrainGrid& Terrain, const FPolygonSet& Ground);
}
