#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Polyline.h"

namespace WorldBuilder
{
/**
 * Points in square cells, so a query within a radius looks at the few cells around it instead of every point. The
 * street furniture's pairwise checks (lamp gaps, junction merging, stacked stop lines) use it to stay linear.
 */
class FPointGrid
{
public:
	explicit FPointGrid(double InCellSize) : CellSize(InCellSize) {}

	/** Adds a point; Item is what a query returns for it. */
	void Add(const FStreetPoint& Point, int Item)
	{
		Cells[CellKey(CellIndex(Point.X), CellIndex(Point.Y))].push_back({Point, Item});
	}

	/** The items of all points within Radius of a place (inclusive), in no particular order. */
	std::vector<int> Within(const FStreetPoint& Centre, double Radius) const
	{
		std::vector<int> Items;
		for (int64_t Row = CellIndex(Centre.Y - Radius); Row <= CellIndex(Centre.Y + Radius); ++Row)
		{
			for (int64_t Column = CellIndex(Centre.X - Radius); Column <= CellIndex(Centre.X + Radius); ++Column)
			{
				const auto Cell = Cells.find(CellKey(Column, Row));
				if (Cell == Cells.end())
				{
					continue;
				}
				for (const FEntry& Entry : Cell->second)
				{
					if (Norm(Entry.Point - Centre) <= Radius)
					{
						Items.push_back(Entry.Item);
					}
				}
			}
		}
		return Items;
	}

	/** True when any point lies strictly closer than Radius to a place. */
	bool AnyCloserThan(const FStreetPoint& Centre, double Radius) const
	{
		for (int64_t Row = CellIndex(Centre.Y - Radius); Row <= CellIndex(Centre.Y + Radius); ++Row)
		{
			for (int64_t Column = CellIndex(Centre.X - Radius); Column <= CellIndex(Centre.X + Radius); ++Column)
			{
				const auto Cell = Cells.find(CellKey(Column, Row));
				if (Cell == Cells.end())
				{
					continue;
				}
				for (const FEntry& Entry : Cell->second)
				{
					if (Norm(Entry.Point - Centre) < Radius)
					{
						return true;
					}
				}
			}
		}
		return false;
	}

private:
	struct FEntry
	{
		FStreetPoint Point;
		int Item = 0;
	};

	double CellSize;
	std::unordered_map<int64_t, std::vector<FEntry>> Cells;

	int64_t CellIndex(double Coordinate) const
	{
		return static_cast<int64_t>(std::floor(Coordinate / CellSize));
	}

	static int64_t CellKey(int64_t Column, int64_t Row)
	{
		return Row * 1000003 + Column;
	}
};
}
