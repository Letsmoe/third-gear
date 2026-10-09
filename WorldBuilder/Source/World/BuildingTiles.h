#pragma once

#include "BuildingRecords.h"
#include "HeightGrid.h"
#include "TileWriter.h"

namespace WorldBuilder
{
/**
 * Writes one building's BLDG, BTYP and ROOF records (worldtile.py's add_building, add_building_type and add_roof).
 * The base height is the lowest conformed ground along the outline; outline points beyond the tile's window, where
 * Ground has no data, take the plain terrain, as the record's own base height does.
 */
void WriteBuilding(FTileWriter& Writer, const FBuilding& Building, const FHeightGrid& Ground, const FBox& Window,
				   const FBox& TileBox);
}
