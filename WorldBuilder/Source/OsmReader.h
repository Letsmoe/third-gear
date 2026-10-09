#pragma once

#include <string>

#include "OsmData.h"

namespace WorldBuilder
{
/**
 * Reads the features the world needs from an OSM PBF file: points, ways by kind, and buildings and other areas
 * assembled from closed ways and multipolygon relations. Features with no node inside the box are left out.
 */
FOsmData ReadOsm(const std::string& PbfPath, const FLonLatBox& Box = FLonLatBox());
}
