#include "WorldBuild.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "HeightGrid.h"
#include "LandCover.h"
#include "OsmReader.h"
#include "Paths.h"
#include "Terrain.h"
#include "TileWriter.h"
#include "Vegetation.h"

#ifndef WORLD_BUILDER_REPO_ROOT
#define WORLD_BUILDER_REPO_ROOT "."
#endif

namespace WorldBuilder
{
namespace
{
/** Vertex spacing of the tiles' terrain grid, metres. */
constexpr double GridCell = 1.0;
/** The window around a tile that its rasters cover, so smoothing and distances agree across tile edges. */
constexpr double TileMargin = 32.0;
/** OSM features are read within this distance around the region, as osm.load's margin. */
constexpr double OsmMargin = 150.0;
/** The terrain is built this far around the region (dem.build_mosaic's margin), which bounds the water too. */
constexpr double TerrainMargin = 100.0;
/** Paths sit this far above the terrain (build_world.py's PATH_LIFT). */
constexpr float PathLift = 0.04f;

const auto BuildStart = std::chrono::steady_clock::now();

/** Prints a message with the seconds since the build started. */
void Log(const std::string& Message)
{
	const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - BuildStart).count();
	std::printf("[%6.1fs] %s\n", Seconds, Message.c_str());
	std::fflush(stdout);
}

/** Building footprints by bounding box. */
struct FBuildingFootprints
{
	std::vector<FPolygons> Footprints;
	FSpatialIndex Index;

	explicit FBuildingFootprints(const std::vector<FOsmArea>& Buildings)
	{
		for (const FOsmArea& Building : Buildings)
		{
			std::vector<FPolygonWithHoles> Polygons;
			for (const FOsmPolygon& Polygon : Building.Polygons)
			{
				Polygons.push_back({Polygon.Rings});
			}
			FPolygons Paths = Clipper2Lib::Union(ToPaths(Polygons), Clipper2Lib::FillRule::EvenOdd, 4);
			Index.Insert(BoundsOf(Paths), static_cast<int>(Footprints.size()));
			Footprints.push_back(std::move(Paths));
		}
	}

	/** The union of the footprints, clipped to a window. */
	FPolygons InWindow(const FBox& Window) const
	{
		FPolygons Pieces;
		for (const int Item : Index.Query(Window))
		{
			const FPolygons Clipped = ClipToBox(Footprints[Item], Window);
			Pieces.insert(Pieces.end(), Clipped.begin(), Clipped.end());
		}
		return UnionOf(Pieces);
	}
};

/** Everything read once for the region, shared read-only by the tile workers. */
struct FRegionInputs
{
	FOsmData Osm;
	FBox Extent;
	const FTerrainGrid* Terrain = nullptr;
	const FLandCover* Cover = nullptr;
	const FWaterBodies* Water = nullptr;
	const FPathSurfaces* Paths = nullptr;
	const FBuildingFootprints* Buildings = nullptr;
	const FVegetationSources* Vegetation = nullptr;
};

/** The union of the water bodies near a window, clipped to it. */
FPolygons WaterInWindow(const std::vector<const FWaterBody*>& Bodies, const FBox& Window)
{
	FPolygons Pieces;
	for (const FWaterBody* Body : Bodies)
	{
		const FPolygons Clipped = ClipToBox(Body->Polygon, Window);
		Pieces.insert(Pieces.end(), Clipped.begin(), Clipped.end());
	}
	return UnionOf(Pieces);
}

FPolygons Combined(std::initializer_list<const FPolygons*> Parts)
{
	FPolygons All;
	for (const FPolygons* Part : Parts)
	{
		All.insert(All.end(), Part->begin(), Part->end());
	}
	return UnionOf(All);
}

/** A yaw for a plant from its position, so it doesn't change with the order plants are placed in. */
float PlantYaw(const FPlant& Plant)
{
	const uint64_t Bits = static_cast<uint64_t>(std::llround(Plant.X * 100.0)) * 0x9E3779B97F4A7C15ull
		^ static_cast<uint64_t>(std::llround(Plant.Y * 100.0)) * 0xBF58476D1CE4E5B9ull;
	return static_cast<float>((Bits >> 11) % 36000) / 100.0f;
}

/** The plants of a tile into its VEGE section, standing on the conformed ground. */
void WritePlants(FTileWriter& Writer, const std::vector<FPlant>& Plants, const FHeightGrid& Ground)
{
	const FBox& Box = Writer.Bounds();
	for (const FPlant& Plant : Plants)
	{
		FRecordWriter Record;
		Record.U16(Writer.Name(Plant.Model));
		Record.Pad(2);
		Record.F32(static_cast<float>(Plant.X - Box.X0));
		Record.F32(static_cast<float>(Plant.Y - Box.Y0));
		Record.F32(static_cast<float>(Ground.Sample(Plant.X, Plant.Y)));
		Record.F32(PlantYaw(Plant));
		Record.F32(static_cast<float>(Plant.Crown));
		Record.F32(static_cast<float>(Plant.Height));
		Record.F32(static_cast<float>(Plant.Trunk));
		Writer.AddRecord("VEGE", std::move(Record.Bytes));
	}
}

/** The terrain and road heights and land cover on the tile's vertex grid (edges shared with neighbours). */
void WriteGrid(FTileWriter& Writer, const FHeightGrid& Ground, const FHeightGrid& RoadHeight, const FLandCover& Cover)
{
	const FBox& Box = Writer.Bounds();
	const int Columns = static_cast<int>(std::round((Box.X1 - Box.X0) / GridCell)) + 1;
	const int Rows = static_cast<int>(std::round((Box.Y1 - Box.Y0) / GridCell)) + 1;
	std::vector<float> Terrain(static_cast<size_t>(Columns) * Rows);
	std::vector<float> Road(Terrain.size());
	for (int Row = 0; Row < Rows; ++Row)
	{
		for (int Column = 0; Column < Columns; ++Column)
		{
			const double X = Box.X0 + Column * GridCell;
			const double Y = Box.Y0 + Row * GridCell;
			Terrain[static_cast<size_t>(Row) * Columns + Column] = static_cast<float>(Ground.Sample(X, Y));
			Road[static_cast<size_t>(Row) * Columns + Column] = static_cast<float>(RoadHeight.Sample(X, Y));
		}
	}
	const std::vector<float> Weights = Cover.Weights(Box.X0, Box.Y0, Columns, Rows, GridCell);
	Writer.SetGrid(Terrain, Road, Weights, Columns, Rows, static_cast<float>(GridCell));
}

/** One tile, written to its file. */
void BuildTile(const FTileBounds& Tile, const FRegionInputs& Inputs, const std::filesystem::path& OutputDirectory)
{
	const FBox Box{Tile.X0, Tile.Y0, Tile.X1, Tile.Y1};
	const FBox Window{Box.X0 - TileMargin, Box.Y0 - TileMargin, Box.X1 + TileMargin, Box.Y1 + TileMargin};
	const FHeightGrid Terrain = Inputs.Terrain->FineWindow(Window.X0, Window.Y0, Window.X1, Window.Y1);
	// Roads and pavements come with the street model; until then they are empty.
	const FPolygons RoadGround;
	const FPolygons Pavement;
	const std::vector<const FWaterBody*> Water = Inputs.Water->Near(Window);
	const FPolygons WaterArea = WaterInWindow(Water, Window);
	const FPolygons Buildings = Inputs.Buildings->InWindow(Window);
	FPolygons Paved;
	FPolygons Unpaved;
	Inputs.Paths->InWindow(Window, Combined({&RoadGround, &Pavement, &Buildings}), WaterArea, Paved, Unpaved);

	const FHeightGrid RoadHeight = RoadHeightField(Terrain, RoadGround);
	const FHeightGrid Ground = ConformTerrain(Terrain, RoadHeight, RoadGround, Pavement, Water);
	FTileWriter Writer(Box);
	WriteGrid(Writer, Ground, RoadHeight, *Inputs.Cover);
	Writer.AddSurface("Path_Paved", Paved, EHeightMode::Terrain, {PathLift});
	Writer.AddSurface("Path_Gravel", Unpaved, EHeightMode::Terrain, {PathLift});
	for (const FWaterBody* Body : Water)
	{
		Writer.AddSurface("Water", Body->Polygon, EHeightMode::Constant, {static_cast<float>(Body->Level)});
	}

	const double Reach = FVegetationSources::WindowMargin();
	FVegetationObstacles Obstacles;
	Obstacles.Window = {Box.X0 - Reach, Box.Y0 - Reach, Box.X1 + Reach, Box.Y1 + Reach};
	Obstacles.RoadGround = RoadGround;
	Obstacles.Pavement = Pavement;
	Obstacles.Buildings = Buildings;
	Obstacles.Water = WaterArea;
	Obstacles.Paths = Combined({&Paved, &Unpaved});
	WritePlants(Writer, Inputs.Vegetation->PlantsInTile(Box, Inputs.Extent, Obstacles), Ground);
	const std::string Name = "tile_" + std::to_string(Tile.IndexX) + "_" + std::to_string(Tile.IndexY) + ".tgtile";
	Writer.Write((OutputDirectory / Name).string());
}

/** Runs the tiles on worker threads, each taking the next tile until none is left. */
void BuildTiles(const std::vector<FTileBounds>& Tiles, const FRegionInputs& Inputs,
				const std::filesystem::path& OutputDirectory, unsigned Threads)
{
	std::atomic<size_t> Next{0};
	std::vector<std::thread> Workers;
	for (unsigned Worker = 0; Worker < Threads; ++Worker)
	{
		Workers.emplace_back([&]() {
			for (size_t Index = Next++; Index < Tiles.size(); Index = Next++)
			{
				BuildTile(Tiles[Index], Inputs, OutputDirectory);
			}
		});
	}
	for (std::thread& Worker : Workers)
	{
		Worker.join();
	}
}

/** world.json: the tile index and attribution, as build_world.py writes it (start pose and traffic still to come). */
void WriteWorldJson(const FRegion& Region, const std::vector<FTileBounds>& Tiles, const std::filesystem::path& Path)
{
	std::ofstream Output(Path);
	char Buffer[256];
	Output << "{\n \"format\": " << TileFormatVersion << ",\n \"region\": \"" << Region.Name << "\",\n";
	std::snprintf(Buffer, sizeof(Buffer), " \"origin_utm32\": [%.1f, %.1f],\n \"tile_size_m\": %.1f,\n", OriginEast,
				  OriginNorth, Region.TileSize);
	Output << Buffer;
	std::snprintf(Buffer, sizeof(Buffer), " \"bounds_m\": [%.1f, %.1f, %.1f, %.1f],\n \"tiles\": [\n", Region.XMin,
				  Region.YMin, Region.XMax, Region.YMax);
	Output << Buffer;
	for (size_t Index = 0; Index < Tiles.size(); ++Index)
	{
		const FTileBounds& Tile = Tiles[Index];
		std::snprintf(Buffer, sizeof(Buffer),
					  "  {\"ix\": %d, \"iy\": %d, \"file\": \"tile_%d_%d.tgtile\", \"bounds_m\": [%.1f, %.1f, %.1f, %.1f]}%s\n",
					  Tile.IndexX, Tile.IndexY, Tile.IndexX, Tile.IndexY, Tile.X0, Tile.Y0, Tile.X1, Tile.Y1,
					  Index + 1 < Tiles.size() ? "," : "");
		Output << Buffer;
	}
	Output << " ],\n \"traffic\": \"traffic.json\",\n \"attribution\": [\n"
		   << "  \"\\u00a9 OpenStreetMap contributors (ODbL)\",\n"
		   << "  \"Freie und Hansestadt Hamburg, LGV (dl-de/by-2.0)\",\n"
		   << "  \"LGLN (2025), CC BY 4.0\",\n"
		   << "  \"Copernicus DEM GLO-30, \\u00a9 DLR e.V. 2010-2014 and \\u00a9 Airbus Defence and Space GmbH 2014-2018\"\n"
		   << " ]\n}\n";
}
}

std::string DefaultDataRoot()
{
	if (const char* Override = std::getenv("THIRD_GEAR_DATA"))
	{
		return Override;
	}
	return (std::filesystem::path(WORLD_BUILDER_REPO_ROOT) / "External").string();
}

void BuildRegion(const FRegion& Region, const FBuildOptions& Options)
{
	const std::filesystem::path DataRoot(Options.DataRoot.empty() ? DefaultDataRoot() : Options.DataRoot);
	const std::filesystem::path Geodata = DataRoot / "geodata";
	const std::filesystem::path OutputDirectory = DataRoot / "world" / (Options.OutputName.empty() ? Region.Name : Options.OutputName);
	std::filesystem::create_directories(OutputDirectory);

	FRegionInputs Inputs;
	Inputs.Osm = ReadOsm((Geodata / "osm" / (Region.OsmExtract + ".osm.pbf")).string(), Region.LonLatBounds(OsmMargin));
	Log("OSM: " + std::to_string(Inputs.Osm.Roads.size()) + " roads, " + std::to_string(Inputs.Osm.Buildings.size())
		+ " buildings, " + std::to_string(Inputs.Osm.Points.size()) + " points");
	const FTerrainGrid Terrain((Geodata / "raw" / "terrain_5m.grid").string());
	Inputs.Terrain = &Terrain;
	Inputs.Extent = {Region.XMin, Region.YMin, Region.XMax, Region.YMax};
	const FLandCover Cover(Inputs.Osm.Areas);
	Inputs.Cover = &Cover;
	const FBox TerrainExtent{Region.XMin - TerrainMargin, Region.YMin - TerrainMargin, Region.XMax + TerrainMargin,
							 Region.YMax + TerrainMargin};
	const FWaterBodies Water = BuildWaterBodies(Inputs.Osm, Terrain, TerrainExtent);
	Inputs.Water = &Water;
	Log(std::to_string(Water.Bodies.size()) + " water bodies");
	const FPathSurfaces Paths(Inputs.Osm);
	Inputs.Paths = &Paths;
	const FBuildingFootprints Buildings(Inputs.Osm.Buildings);
	Inputs.Buildings = &Buildings;
	const FVegetationSources Vegetation(Inputs.Osm, ReadStreetTrees((Geodata / "raw" / "strassenbaeume" / "street_trees.tsv").string()));
	Inputs.Vegetation = &Vegetation;
	Log("sources indexed");

	const std::vector<FTileBounds> Tiles = Region.Tiles();
	const unsigned Threads = Options.Threads > 0 ? Options.Threads : std::max(1u, std::thread::hardware_concurrency());
	BuildTiles(Tiles, Inputs, OutputDirectory, Threads);
	WriteWorldJson(Region, Tiles, OutputDirectory / "world.json");
	Log("wrote " + std::to_string(Tiles.size()) + " tiles to " + OutputDirectory.string() + " on "
		+ std::to_string(Threads) + " threads");
}
}
