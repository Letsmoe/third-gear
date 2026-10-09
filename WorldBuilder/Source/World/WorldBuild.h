#pragma once

#include <string>

#include "Region.h"

/** The build of a region's world tiles, as Tools/osmimport/build_world.py. */
namespace WorldBuilder
{
struct FBuildOptions
{
	/** The data root (geodata/ and world/ below it). */
	std::string DataRoot;
	/** Folder under world/ to write to; the region's name when empty. */
	std::string OutputName;
	/** Worker threads for the tiles; 0 means one per core. */
	unsigned Threads = 0;
};

/**
 * The data root: $THIRD_GEAR_DATA if set, otherwise the checkout's External link (Tools/bootstrap/data_root.py).
 */
std::string DefaultDataRoot();

/** Builds every tile of the region and world.json; prints the time of each stage. Throws on errors. */
void BuildRegion(const FRegion& Region, const FBuildOptions& Options);
}
