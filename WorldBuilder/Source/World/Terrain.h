#pragma once

#include <vector>

#include "HeightGrid.h"
#include "Raster.h"
#include "RoadHeights.h"

/**
 * The terrain under and around roads, as Tools/osmimport/osmimport/terrain.py's conform and roads.py's
 * _road_height_field, on one tile's window instead of the whole area.
 */
namespace WorldBuilder
{
/** Kerb height of pavements above the road, metres (roads.py's KERB_HEIGHT). */
constexpr double KerbHeight = 0.12;

/** A lake, river or canal with the level of its surface (paths.py's WaterBody). */
struct FWaterBody
{
	FPolygons Polygon;
	double Level = 0.0;
};

/**
 * The smooth road surface height: a normalised Gaussian of the terrain inside the road outline, extended outward
 * from the road by the nearest value. Where the window has no road at all, the terrain itself.
 */
FHeightGrid RoadHeightField(const FHeightGrid& Terrain, const FPolygons& RoadGround);

/**
 * The road surface height from the road model (issue #113): its designed height inside the road outline, the
 * smoothed terrain of RoadHeightField where no road of the model reaches, extended outward from the road by the
 * nearest value.
 */
FHeightGrid DesignedRoadHeightField(const FHeightGrid& Terrain, const FPolygons& RoadGround, const FRoadHeights& Roads);

/**
 * The terrain pressed under roads and raised under pavements, blended back to the natural ground over a few metres;
 * water bodies get a bed that deepens away from the bank.
 */
FHeightGrid ConformTerrain(const FHeightGrid& Terrain, const FHeightGrid& RoadHeight, const FPolygons& RoadGround,
						   const FPolygons& Pavement, const std::vector<FWaterBody>& Water);
}
