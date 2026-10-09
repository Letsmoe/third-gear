#include "Region.h"

#include <algorithm>
#include <array>

#include "Projection.h"

namespace WorldBuilder
{
namespace
{
const std::array<FRegion, 3> Regions = {{
	{"bergedorf_core", -1000.0, 1000.0, -1000.0, 1000.0},
	{"bergedorf_test", -250.0, 250.0, -250.0, 250.0},
	{"hamburg", -32500.0, 8750.0, -30000.0, 11250.0, 250.0, "hamburg"},
}};
}

std::vector<FTileBounds> FRegion::Tiles() const
{
	std::vector<FTileBounds> Result;
	int IndexY = 0;
	for (double Y = YMin; Y < YMax; Y += TileSize, ++IndexY)
	{
		int IndexX = 0;
		for (double X = XMin; X < XMax; X += TileSize, ++IndexX)
		{
			Result.push_back({IndexX, IndexY, X, Y, std::min(X + TileSize, XMax), std::min(Y + TileSize, YMax)});
		}
	}
	return Result;
}

FLonLatBox FRegion::LonLatBounds(double Margin) const
{
	// geo.py takes the corners (west, south) and (east, north) of the UTM box, not the box around all four.
	const double EastMin = OriginEast + XMin - Margin;
	const double NorthMin = OriginNorth - YMax - Margin;
	const double EastMax = OriginEast + XMax + Margin;
	const double NorthMax = OriginNorth - YMin + Margin;
	const FLonLat SouthWest = UtmToLonLat(EastMin, NorthMin);
	const FLonLat NorthEast = UtmToLonLat(EastMax, NorthMax);
	return {SouthWest.Longitude, SouthWest.Latitude, NorthEast.Longitude, NorthEast.Latitude};
}

const FRegion* FindRegion(const std::string& Name)
{
	for (const FRegion& Region : Regions)
	{
		if (Region.Name == Name)
		{
			return &Region;
		}
	}
	return nullptr;
}

std::vector<std::string> RegionNames()
{
	std::vector<std::string> Names;
	for (const FRegion& Region : Regions)
	{
		Names.push_back(Region.Name);
	}
	return Names;
}
}
