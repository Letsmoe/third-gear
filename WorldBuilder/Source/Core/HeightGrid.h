#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

/** Heights on regular grids, as Tools/osmimport/osmimport/dem.py's HeightGrid. */
namespace WorldBuilder
{
/**
 * Heights on a regular grid of square cells. Cell (row, column) has its centre at
 * x = X0 + (column + 0.5) * CellSize, y = Y0 + (row + 0.5) * CellSize; row 0 is the northern edge (smallest y).
 */
struct FHeightGrid
{
	std::vector<float> Heights;
	int Columns = 0;
	int Rows = 0;
	double X0 = 0.0;
	double Y0 = 0.0;
	double CellSize = 1.0;

	float& At(int Row, int Column) { return Heights[static_cast<size_t>(Row) * Columns + Column]; }
	float At(int Row, int Column) const { return Heights[static_cast<size_t>(Row) * Columns + Column]; }

	/** Bilinear height at a point, clamped to the grid's cell centres at the border. */
	double Sample(double X, double Y) const;
};

/**
 * The 5 m terrain grid file written by Tools/bootstrap/prepare_geodata.py (dem.py's TERRAIN_GRID), memory-mapped.
 * Its coordinates are UTM eastings and northings.
 */
class FTerrainGrid
{
public:
	/** Opens the grid file; throws std::runtime_error when it is missing or not a terrain grid. */
	explicit FTerrainGrid(const std::string& Path);
	~FTerrainGrid();
	FTerrainGrid(const FTerrainGrid&) = delete;
	FTerrainGrid& operator=(const FTerrainGrid&) = delete;

	/** Bilinear height at a world point, as dem.py's HeightGrid.sample on the coarse grid. */
	double Sample(double WorldX, double WorldY) const;

	/**
	 * The 1 m grid of a world box (whole metres), each cell's height interpolated from the 5 m grid at its centre:
	 * what dem.build_mosaic returns for an area.
	 */
	FHeightGrid FineWindow(double X0, double Y0, double X1, double Y1) const;

private:
	const float* Heights = nullptr;
	void* Mapping = nullptr;
	size_t MappingSize = 0;
	int Columns = 0;
	int Rows = 0;
	double West = 0.0;
	double North = 0.0;
	double CellSize = 5.0;
};
}
