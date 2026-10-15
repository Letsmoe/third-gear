#pragma once

#include "Region.h"
#include "WorldBuild.h"

/** The road model of a region written out for the street viewer, next to the road heights the tiles use today. */
namespace WorldBuilder
{
/**
 * Builds the region's road model and writes <data root>/world/<region or output name>/road_model.json: every chain
 * touching the region with its OSM line, smooth centreline, ground, today's road height, designed height and cross
 * fall.
 */
void WriteRoadModelPreview(const FRegion& Region, const FBuildOptions& Options);
}
