#pragma once

#include <optional>
#include <vector>

#include "OsmData.h"
#include "Raster.h"
#include "SpatialIndex.h"

/**
 * Ground cover per terrain vertex from OSM land use, as Tools/osmimport/osmimport/landcover.py: blend weights for
 * M_TerrainMaster, meadow, field soil and forest floor; all zero is mown lawn.
 */
namespace WorldBuilder
{
enum class ECoverClass
{
	Meadow = 0,
	Field = 1,
	Forest = 2,
};

/** The cover class of an area from its tags, or nothing for areas that leave the ground as lawn. */
std::optional<ECoverClass> ClassifyCover(const FOsmArea& Area);

class FLandCover
{
public:
	explicit FLandCover(const std::vector<FOsmArea>& Areas);

	/**
	 * Weights (three per vertex, row by row) for a vertex grid: vertex (row, column) at
	 * (X0 + column * CellSize, Y0 + row * CellSize), each class blurred over BlurMetres.
	 */
	std::vector<float> Weights(double X0, double Y0, int Columns, int Rows, double CellSize,
							   double BlurMetres = 3.0) const;

private:
	struct FClassPolygons
	{
		std::vector<FPolygons> Polygons;
		FSpatialIndex Index;
	};
	FClassPolygons Classes[3];
};
}
