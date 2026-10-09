#pragma once

#include <string>
#include <vector>

#include "OsmData.h"

/** The build regions of Tools/osmimport/osmimport/geo.py, in world metres (x east, y south). */
namespace WorldBuilder
{
struct FTileBounds
{
	int IndexX = 0;
	int IndexY = 0;
	double X0 = 0.0;
	double Y0 = 0.0;
	double X1 = 0.0;
	double Y1 = 0.0;
};

struct FRegion
{
	std::string Name;
	double XMin = 0.0;
	double XMax = 0.0;
	double YMin = 0.0;
	double YMax = 0.0;
	double TileSize = 250.0;
	/** The OSM extract under geodata/osm/ that covers the region. */
	std::string OsmExtract = "bergedorf";

	/** Every tile of the region, row by row from the north-west corner. */
	std::vector<FTileBounds> Tiles() const;

	/** The longitude and latitude box around the region and a margin in metres, as geo.py's Area.wgs_bounds. */
	FLonLatBox LonLatBounds(double Margin) const;
};

/** The region with this name, or nullptr. */
const FRegion* FindRegion(const std::string& Name);

/** The names of all regions, for messages. */
std::vector<std::string> RegionNames();
}
