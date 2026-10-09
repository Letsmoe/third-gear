#pragma once

#include <cstdint>
#include <vector>

#include "clipper2/clipper.h"

/**
 * Grid operations the Python build gets from rasterio and scipy.ndimage, on one tile's window at a time:
 * rasterising polygons, Gaussian smoothing and the Euclidean distance transform.
 */
namespace WorldBuilder
{
/** Polygons in world metres: outlines and holes as Clipper2 paths, filled by the even-odd rule. */
using FPolygons = Clipper2Lib::PathsD;

/** The placement of a grid: cell (row, column) has its centre at X0 + (column + 0.5) * CellSize, Y0 + (row + 0.5) * CellSize. */
struct FGridFrame
{
	int Columns = 0;
	int Rows = 0;
	double X0 = 0.0;
	double Y0 = 0.0;
	double CellSize = 1.0;

	size_t CellCount() const { return static_cast<size_t>(Columns) * Rows; }
};

using FMask = std::vector<uint8_t>;

/**
 * Marks the cells of the polygons. Without bAllTouched a cell counts when its centre is inside (rasterio's default);
 * with it, every cell the outline passes through counts as well (rasterio's all_touched=True).
 */
FMask RasterizePolygons(const FGridFrame& Frame, const FPolygons& Polygons, bool bAllTouched);

/** Gaussian smoothing as scipy.ndimage.gaussian_filter: sigma in cells, truncated at 4 sigma, mirrored edges. */
void GaussianFilter(std::vector<double>& Values, int Columns, int Rows, double Sigma);

/** For every cell, the distance to the nearest feature cell (in metres) and that cell's index. */
struct FDistanceField
{
	std::vector<double> Distance;
	std::vector<int64_t> Nearest;
};

/**
 * Exact Euclidean distance transform, as scipy.ndimage.distance_transform_edt(~IsFeature, return_indices=True):
 * feature cells have distance 0 and are their own nearest. Without any feature cell every distance is infinite
 * and every nearest index -1.
 */
FDistanceField DistanceToFeatures(const FMask& IsFeature, int Columns, int Rows, double CellSize);
}
