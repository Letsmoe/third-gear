#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ChunkedPolygons.h"
#include "Geometry.h"
#include "OsmData.h"
#include "SpatialIndex.h"

/**
 * Trees and shrubs, as Tools/osmimport/osmimport/vegetation.py: street trees from Hamburg's register, OSM trees, tree
 * rows and hedges, and trees scattered in woods, parks and scrub. The rules are the same; the randomness is drawn per
 * feature (from its id) rather than from one sequence, and scattered trees sit on a lattice fixed to the world, so a
 * tile places the same plants whichever tiles are built with it.
 */
namespace WorldBuilder
{
struct FPlant
{
	/** "broadleaf", "conifer" or "shrub" (Scripts/vegetation_models.py). */
	const char* Model = "broadleaf";
	double X = 0.0;
	double Y = 0.0;
	double Crown = 0.0;
	double Height = 0.0;
	/** Trunk diameter; 0 means no collider. */
	double Trunk = 0.0;
	/** "kataster", "osm_tree", "tree_row", "hedge", "wood", "wood_edge", "scrub" or "park". */
	const char* Source = "";
};

/** One register tree: id, world position, German genus, crown diameter and trunk girth in cm (0 when unknown). */
struct FStreetTree
{
	int64_t Id = 0;
	double X = 0.0;
	double Y = 0.0;
	std::string Genus;
	double Crown = 0.0;
	double Girth = 0.0;
};

/** Reads the register table written by prepare_geodata.py (street_trees.tsv). */
std::vector<FStreetTree> ReadStreetTrees(const std::string& Path);

/** What plants keep clear of in a tile's window (all clipped to it). */
struct FVegetationObstacles
{
	FBox Window;
	FPolygons RoadGround;
	FPolygons Pavement;
	FPolygons Buildings;
	FPolygons Water;
	/** Paved and unpaved paths. */
	FPolygons Paths;
};

/** Every source of plants in the region, found by position. */
class FVegetationSources
{
public:
	FVegetationSources(const FOsmData& Osm, std::vector<FStreetTree> StreetTrees);

	/** The plants of one tile, inside the region's extent. */
	std::vector<FPlant> PlantsInTile(const FBox& Tile, const FBox& Extent, const FVegetationObstacles& Obstacles) const;

	/** How far around a tile the obstacles must reach, metres. */
	static double WindowMargin();

	const std::vector<FStreetTree>& Trees() const { return StreetTrees; }
	const FOsmData& Osm() const { return *OsmData; }
	const FSpatialIndex& TreeIndex() const { return StreetTreeIndex; }
	const FSpatialIndex& PointIndex() const { return TreePointIndex; }
	const FSpatialIndex& RowIndex() const { return TreeRowIndex; }
	const FSpatialIndex& HedgeIndex() const { return HedgeLineIndex; }
	/** The planted areas (woods, scrub, parks) in chunks, by their index in Osm().Areas. */
	const FChunkedPolygons& PlantedAreas() const { return PlantedAreaPieces; }

private:
	const FOsmData* OsmData;
	std::vector<FStreetTree> StreetTrees;
	FSpatialIndex StreetTreeIndex;
	FSpatialIndex TreePointIndex;
	FSpatialIndex TreeRowIndex;
	FSpatialIndex HedgeLineIndex;
	FChunkedPolygons PlantedAreaPieces;
};
}
