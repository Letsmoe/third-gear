#include "HeightGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Projection.h"

namespace WorldBuilder
{
namespace
{
/** dem.py's TERRAIN_GRID_HEADER: "<4sIddfII". */
#pragma pack(push, 1)
struct FTerrainGridHeader
{
	char Magic[4];
	uint32_t Version;
	double West;
	double North;
	float CellSize;
	uint32_t Columns;
	uint32_t Rows;
};
#pragma pack(pop)
constexpr uint32_t TerrainGridVersion = 1;

/**
 * Bilinear interpolation of a row-major grid at fractional cell coordinates measured from the first cell's centre,
 * clamped as numpy's version in dem.py (just inside the last cell centre).
 */
double Bilinear(const float* Heights, int Columns, int Rows, double FractionalColumn, double FractionalRow)
{
	FractionalColumn = std::clamp(FractionalColumn, 0.0, Columns - 1.000001);
	FractionalRow = std::clamp(FractionalRow, 0.0, Rows - 1.000001);
	const int Column = static_cast<int>(std::floor(FractionalColumn));
	const int Row = static_cast<int>(std::floor(FractionalRow));
	const double AlongColumn = FractionalColumn - Column;
	const double AlongRow = FractionalRow - Row;
	const float* Top = Heights + static_cast<size_t>(Row) * Columns + Column;
	const float* Bottom = Top + Columns;
	const double TopHeight = Top[0] * (1.0 - AlongColumn) + Top[1] * AlongColumn;
	const double BottomHeight = Bottom[0] * (1.0 - AlongColumn) + Bottom[1] * AlongColumn;
	return TopHeight * (1.0 - AlongRow) + BottomHeight * AlongRow;
}
}

double FHeightGrid::Sample(double X, double Y) const
{
	return Bilinear(Heights.data(), Columns, Rows, (X - X0) / CellSize - 0.5, (Y - Y0) / CellSize - 0.5);
}

FTerrainGrid::FTerrainGrid(const std::string& Path)
{
	const int File = open(Path.c_str(), O_RDONLY);
	if (File < 0)
	{
		throw std::runtime_error("cannot open terrain grid " + Path);
	}
	struct stat Status{};
	fstat(File, &Status);
	MappingSize = static_cast<size_t>(Status.st_size);
	Mapping = mmap(nullptr, MappingSize, PROT_READ, MAP_PRIVATE, File, 0);
	close(File);
	if (Mapping == MAP_FAILED || MappingSize < sizeof(FTerrainGridHeader))
	{
		throw std::runtime_error("cannot map terrain grid " + Path);
	}
	FTerrainGridHeader Header;
	std::memcpy(&Header, Mapping, sizeof(Header));
	if (std::memcmp(Header.Magic, "TGH5", 4) != 0 || Header.Version != TerrainGridVersion)
	{
		throw std::runtime_error(Path + " is not a terrain grid of version 1");
	}
	Heights = reinterpret_cast<const float*>(static_cast<const char*>(Mapping) + sizeof(Header));
	Columns = static_cast<int>(Header.Columns);
	Rows = static_cast<int>(Header.Rows);
	West = Header.West;
	North = Header.North;
	CellSize = Header.CellSize;
}

FTerrainGrid::~FTerrainGrid()
{
	if (Mapping != nullptr && Mapping != MAP_FAILED)
	{
		munmap(Mapping, MappingSize);
	}
}

double FTerrainGrid::Sample(double WorldX, double WorldY) const
{
	const double PointEast = WorldX + OriginEast;
	const double PointNorth = OriginNorth - WorldY;
	return Bilinear(Heights, Columns, Rows, (PointEast - West) / CellSize - 0.5, (North - PointNorth) / CellSize - 0.5);
}

FHeightGrid FTerrainGrid::FineWindow(double X0, double Y0, double X1, double Y1) const
{
	FHeightGrid Grid;
	Grid.X0 = std::floor(X0);
	Grid.Y0 = std::floor(Y0);
	Grid.Columns = static_cast<int>(std::ceil(X1) - Grid.X0);
	Grid.Rows = static_cast<int>(std::ceil(Y1) - Grid.Y0);
	Grid.CellSize = 1.0;
	Grid.Heights.resize(static_cast<size_t>(Grid.Columns) * Grid.Rows);
	for (int Row = 0; Row < Grid.Rows; ++Row)
	{
		for (int Column = 0; Column < Grid.Columns; ++Column)
		{
			Grid.At(Row, Column) = static_cast<float>(Sample(Grid.X0 + Column + 0.5, Grid.Y0 + Row + 0.5));
		}
	}
	return Grid;
}
}
