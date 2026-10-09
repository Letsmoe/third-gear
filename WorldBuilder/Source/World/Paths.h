#pragma once

#include <vector>

#include "ChunkedPolygons.h"
#include "Geometry.h"
#include "HeightGrid.h"
#include "OsmData.h"
#include "SpatialIndex.h"
#include "Terrain.h"

/**
 * Water bodies and the paved and unpaved surfaces off the road (footpaths, pedestrian zones, squares, car parks), as
 * Tools/osmimport/osmimport/paths.py.
 */
namespace WorldBuilder
{
/** The water bodies of a region, merged, each with its level, cut into chunks. */
struct FWaterBodies
{
	std::vector<double> Levels;
	FChunkedPolygons Pieces;

	/** The parts of the bodies inside a window, each with its level. */
	std::vector<FWaterBody> InWindow(const FBox& Window) const;
};

/**
 * Lakes, rivers and canals (water areas, and river and canal lines at their width) merged and cut to the extent, each
 * with the level of its surface measured in the terrain.
 */
FWaterBodies BuildWaterBodies(const FOsmData& Osm, const FTerrainGrid& Terrain, const FBox& Extent);

/** The footpath and square polygons of the region, each paved or not, found by bounding box. */
class FPathSurfaces
{
public:
	explicit FPathSurfaces(const FOsmData& Osm);

	/**
	 * The paved and unpaved surfaces within a window: everything not under roads, pavements, buildings or water.
	 * Unpaved surfaces give way to paved ones.
	 */
	void InWindow(const FBox& Window, const FPolygons& Blocked, const FPolygons& Water, FPolygons& Paved,
				  FPolygons& Unpaved) const;

private:
	/** Owner 1 is paved, 0 unpaved. */
	FChunkedPolygons Pieces;
};

/** A number from a tag value as paths.py's _float reads it ("2,5 m" is 2.5), or 0 when it isn't one. */
double TagNumber(const std::string* Value);
}
