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

/** The tall structures seen from far away: pylons with their lines, and wind turbines. */
struct FFarStructureSources
{
	/** power=tower nodes. */
	std::vector<FOsmPoint> Towers;
	/** Wind turbines: power=generator nodes with generator:source=wind. */
	std::vector<FOsmPoint> WindTurbines;
	/** power=line ways, with their node ids only (Points stays empty; the towers carry the positions). */
	std::vector<FOsmWay> PowerLines;
};

/** Reads the towers, turbines and power lines in a box, in one pass without node locations for ways. */
FFarStructureSources ReadFarStructureSources(const std::string& PbfPath, const FLonLatBox& Box);
}
