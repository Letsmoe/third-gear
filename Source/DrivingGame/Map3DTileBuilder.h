#pragma once

#include "CoreMinimal.h"

class FMap3DMeshBuilder;
struct FWorldTileData;

/** How much of a tile the 3D map builds: far tiles only get the shapes that read from a distance. */
enum class EMap3DDetail : uint8
{
	/** Land, water, road and footway surfaces and plain boxed buildings. */
	Far,
	/** Everything of Far, plus lane markings, roofs, trees and traffic lights. */
	Near,
};

namespace Map3D
{
/**
 * Fills a mesh with the map shapes of one tile, in metres relative to the tile corner, standing on a flat ground at
 * height 0: green areas, water, footways and roads as flat layers a few centimetres apart, buildings extruded from the
 * footprint up to the eaves with the roof on top (skeleton faces from the tile where it has them, a gabled or hipped roof
 * over the roof rectangle, else flat), and for Near detail markings, trees and traffic lights. Safe on worker threads.
 */
void BuildTile(const FWorldTileData& Tile, EMap3DDetail Detail, FMap3DMeshBuilder& OutMesh);
}
