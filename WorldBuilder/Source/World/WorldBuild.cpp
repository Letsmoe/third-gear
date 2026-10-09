#include "WorldBuild.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include "BuildingRecords.h"
#include "BuildingTiles.h"
#include "HeightGrid.h"
#include "LandCover.h"
#include "Lanes.h"
#include "OsmReader.h"
#include "Assumptions.h"
#include "Paths.h"
#include "Furniture.h"
#include "Parking.h"
#include "PolygonQuery.h"
#include "PolygonSet.h"
#include "RoadGraph.h"
#include "TrafficJson.h"
#include "Zebras.h"
#include "RoadSurfaces.h"
#include "Roads.h"
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

/** A marking kind's style id in the MARK section, painted width and dash (build_world.py's MARKING_STYLES). */
struct FMarkingStyle
{
	const char* Kind;
	uint8_t Style;
	float Width;
	Assumptions::FDashGap Dash;
};
const FMarkingStyle MarkingStyles[] = {
	{"dash_urban", 0, 0.12f, {3.0, 6.0}},
	{"dash_rural", 1, 0.12f, {4.0, 8.0}},
	{"solid", 2, 0.12f, {0.0, 0.0}},
	{"edge", 3, 0.25f, {0.0, 0.0}},
	{"guide", 4, 0.12f, Assumptions::GuideLineDash},
	{"edge_guide", 5, 0.25f, Assumptions::EdgeGuideDash},
	{"cycle_exclusive", 6, 0.25f, {0.0, 0.0}},
	{"cycle_advisory", 7, 0.12f, Assumptions::AdvisoryCycleLineDash},
	{"cycle_furt", 8, 0.25f, Assumptions::CycleFurtDash},
	{"turn_lane_dash", 9, 0.25f, Assumptions::TurnLaneDash},
	{"turn_lane_solid", 10, 0.25f, {0.0, 0.0}},
	{"hatch", 11, static_cast<float>(Assumptions::HatchStripeWidth), {0.0, 0.0}},
};

const FMarkingStyle* FindMarkingStyle(const std::string& Kind)
{
	for (const FMarkingStyle& Style : MarkingStyles)
	{
		if (Kind == Style.Kind)
		{
			return &Style;
		}
	}
	return nullptr;
}

const auto BuildStart = std::chrono::steady_clock::now();

/** The stages of a tile, for the time spent in each, summed over all tiles and threads. */
enum ETileStage
{
	StageTerrain,
	StageObstacles,
	StagePaths,
	StageConform,
	StageGrid,
	StageSurfaces,
	StageVegetation,
	StageWrite,
	StageCount,
};
const char* const StageNames[StageCount] = {"terrain window", "roads, water and buildings", "paths", "conform", "grid",
											"surfaces and furniture", "vegetation", "write"};
std::atomic<int64_t> StageNanoseconds[StageCount];

/** Measures from its creation to Next, adding the time to a stage. */
class FStageClock
{
public:
	void Next(ETileStage Stage)
	{
		const auto Now = std::chrono::steady_clock::now();
		StageNanoseconds[Stage] += std::chrono::duration_cast<std::chrono::nanoseconds>(Now - Last).count();
		Last = Now;
	}

private:
	std::chrono::steady_clock::time_point Last = std::chrono::steady_clock::now();
};

/** Prints a message with the seconds since the build started. */
void Log(const std::string& Message)
{
	const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - BuildStart).count();
	std::printf("[%6.1fs] %s\n", Seconds, Message.c_str());
	std::fflush(stdout);
}

/** The OSM building footprints as a polygon set. */
FPolygonSet BuildingFootprints(const std::vector<FOsmArea>& Buildings)
{
	FPolygonSet Footprints;
	for (const FOsmArea& Building : Buildings)
	{
		std::vector<FPolygonWithHoles> Polygons;
		for (const FOsmPolygon& Polygon : Building.Polygons)
		{
			Polygons.push_back({Polygon.Rings});
		}
		Footprints.Add(Clipper2Lib::Union(ToPaths(Polygons), Clipper2Lib::FillRule::EvenOdd, 4));
	}
	return Footprints;
}

/** The furniture, parked cars and buildings standing in one tile. */
struct FTileContents
{
	std::vector<const FBuilding*> Buildings;
	/** Indices into FRegionInputs::LaneQueries of the lane points in this tile. */
	std::vector<size_t> LanePoints;
	std::vector<const FLamp*> Lamps;
	std::vector<const FSignalHead*> Heads;
	std::vector<const FSign*> Signs;
	std::vector<const FParkedCar*> Cars;
};

/** White bars painted across the road: zebra stripes and stop lines, both 0.5 m wide. */
struct FBar
{
	FPolyline Points;
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
	const FPolygonSet* Buildings = nullptr;
	const FVegetationSources* Vegetation = nullptr;
	const FRoadSurfaces* Roads = nullptr;
	std::vector<FBar> Bars;
	FSpatialIndex BarIndex;
	/** The places the lanes need a road height, and the heights the tiles fill in (each index by one tile). */
	std::vector<FStreetPoint> LaneQueries;
	std::vector<double>* LaneHeights = nullptr;
	/** What stands in each tile, by tile index (Region.Tiles() order). */
	std::vector<FTileContents> Contents;
};

/** The road, pavement, path, water and bridge surfaces of a tile, with the height rule of each. */
void WriteSurfaces(FTileWriter& Writer, const FWindowRoads& Roads, const FPolygons& Paved, const FPolygons& Unpaved,
				   const std::vector<FWaterBody>& Water, const FRoadSurfaces& RoadSurfaces)
{
	Writer.AddSurface("Road_Asphalt", Roads.Asphalt, EHeightMode::Road);
	Writer.AddSurface("Road_Pavers", Roads.Pavers, EHeightMode::Road);
	Writer.AddSurface("Road_Cobble", Roads.Cobble, EHeightMode::Road);
	Writer.AddSurface("Pavement", Roads.Pavement, EHeightMode::Road, {static_cast<float>(KerbHeight)});
	Writer.AddSurface("Path_Paved", Paved, EHeightMode::Terrain, {PathLift});
	Writer.AddSurface("Path_Gravel", Unpaved, EHeightMode::Terrain, {PathLift});
	for (const FWaterBody& Body : Water)
	{
		Writer.AddSurface("Water", Body.Polygon, EHeightMode::Constant, {static_cast<float>(Body.Level)});
	}
	for (const FBridgeDeck* Deck : RoadSurfaces.BridgesNear(Writer.Bounds()))
	{
		Writer.AddSurface("Road_Asphalt", Deck->Polygon, EHeightMode::Ramp,
						  {static_cast<float>(Deck->StartHeight), static_cast<float>(Deck->EndHeight),
						   static_cast<float>(Deck->Start.X), static_cast<float>(Deck->Start.Y),
						   static_cast<float>(Deck->End.X), static_cast<float>(Deck->End.Y)});
	}
}

/** The painted lines crossing a tile. */
void WriteMarkings(FTileWriter& Writer, const FRoadSurfaces& Roads)
{
	for (const FMarkingLine* Marking : Roads.MarkingsNear(Writer.Bounds()))
	{
		const FMarkingStyle* Style = FindMarkingStyle(Marking->Kind);
		if (Style == nullptr)
		{
			continue;
		}
		Writer.AddMarking("Marking_White", Style->Style, Marking->Points, Style->Width,
						  static_cast<float>(Style->Dash.Dash), static_cast<float>(Style->Dash.Gap));
	}
}

/** The union of the water in a window. */
FPolygons WaterInWindow(const std::vector<FWaterBody>& Bodies)
{
	FPolygons Pieces;
	for (const FWaterBody& Body : Bodies)
	{
		Pieces.insert(Pieces.end(), Body.Polygon.begin(), Body.Polygon.end());
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

/** The zebra stripes and stop lines crossing a tile (build_world.py's write_zebras and write_stop_lines). */
void WriteBars(FTileWriter& Writer, const FRegionInputs& Inputs)
{
	const uint8_t SolidStyle = FindMarkingStyle("solid")->Style;
	for (const int Item : Inputs.BarIndex.Query(Writer.Bounds()))
	{
		Writer.AddMarking("Marking_White", SolidStyle, Inputs.Bars[Item].Points, 0.5f, 0.0f, 0.0f);
	}
}

/** The street furniture and parked cars of a tile, standing on the pavement or the conformed ground. */
void WritePois(FTileWriter& Writer, const FTileContents& Contents, const FPolygons& Pavement, const FHeightGrid& Ground,
			   const FHeightGrid& RoadHeight)
{
	const FPolygonQuery OnPavement(Pavement);
	auto FootHeight = [&](double X, double Y) {
		if (OnPavement.Contains(X, Y))
		{
			return static_cast<float>(RoadHeight.Sample(X, Y) + KerbHeight);
		}
		return static_cast<float>(Ground.Sample(X, Y));
	};
	for (const FLamp* Lamp : Contents.Lamps)
	{
		FPoi Poi;
		Poi.Kind = EPoiKind::Lamp;
		Poi.X = Lamp->X;
		Poi.Y = Lamp->Y;
		Poi.Z = FootHeight(Lamp->X, Lamp->Y);
		Poi.Yaw = static_cast<float>(Lamp->YawDegrees);
		Poi.Param0 = Lamp->bFromOsm ? 7.0f : 6.5f;
		Poi.Param1 = 1.6f;
		Poi.Flags = Lamp->bFromOsm ? 1 : 0;
		Writer.AddPoi(Poi);
	}
	for (const FSignalHead* Head : Contents.Heads)
	{
		FPoi Poi;
		Poi.Kind = EPoiKind::SignalHead;
		Poi.X = Head->X;
		Poi.Y = Head->Y;
		Poi.Z = FootHeight(Head->X, Head->Y);
		Poi.Yaw = static_cast<float>(Head->YawDegrees);
		Poi.Param0 = 3.4f;
		Poi.Link = static_cast<uint32_t>(Head->Approach);
		Poi.Flags = Head->bLeftSide ? 1 : 0;
		Writer.AddPoi(Poi);
	}
	for (const FSign* Sign : Contents.Signs)
	{
		FPoi Poi;
		Poi.Kind = EPoiKind::Sign;
		Poi.X = Sign->X;
		Poi.Y = Sign->Y;
		Poi.Z = FootHeight(Sign->X, Sign->Y);
		Poi.Yaw = static_cast<float>(Sign->YawDegrees);
		Poi.Variant = Writer.Name(Sign->Names[0]);
		Poi.Variant2 = Sign->Names.size() > 1 ? Writer.Name(Sign->Names[1]) : NoVariant;
		Writer.AddPoi(Poi);
	}
	const auto SampleRoad = [&](double X, double Y) { return RoadHeight.Sample(X, Y); };
	for (const FParkedCar* Car : Contents.Cars)
	{
		const FParkedCarPose Pose = FinishParkedCar(*Car, SampleRoad);
		FPoi Poi;
		Poi.Kind = EPoiKind::ParkedCar;
		Poi.X = Car->X;
		Poi.Y = Car->Y;
		Poi.Z = static_cast<float>(Pose.Z);
		Poi.Yaw = static_cast<float>(Car->YawDegrees);
		Poi.Variant = static_cast<uint16_t>(Car->Model);
		Poi.Param0 = static_cast<float>(Pose.RollDegrees);
		Poi.Param1 = static_cast<float>(Pose.PitchDegrees);
		Writer.AddPoi(Poi);
	}
}

/** One tile, written to its file. */
void BuildTile(const FTileBounds& Tile, size_t TileIndex, const FRegionInputs& Inputs,
			   const std::filesystem::path& OutputDirectory)
{
	const FBox Box{Tile.X0, Tile.Y0, Tile.X1, Tile.Y1};
	const FBox Window{Box.X0 - TileMargin, Box.Y0 - TileMargin, Box.X1 + TileMargin, Box.Y1 + TileMargin};
	FStageClock Clock;
	const FHeightGrid Terrain = Inputs.Terrain->FineWindow(Window.X0, Window.Y0, Window.X1, Window.Y1);
	Clock.Next(StageTerrain);
	const FWindowRoads Roads = Inputs.Roads->InWindow(Window);
	const FPolygons& RoadGround = Roads.Ground;
	const FPolygons& Pavement = Roads.Pavement;
	const std::vector<FWaterBody> Water = Inputs.Water->InWindow(Window);
	const FPolygons WaterArea = WaterInWindow(Water);
	const FPolygons Buildings = Inputs.Buildings->InWindow(Window);
	Clock.Next(StageObstacles);
	FPolygons Paved;
	FPolygons Unpaved;
	Inputs.Paths->InWindow(Window, Combined({&RoadGround, &Pavement, &Buildings}), WaterArea, Paved, Unpaved);
	Clock.Next(StagePaths);

	const FHeightGrid RoadHeight = RoadHeightField(Terrain, RoadGround);
	const FHeightGrid Ground = ConformTerrain(Terrain, RoadHeight, RoadGround, Pavement, Water);
	Clock.Next(StageConform);
	FTileWriter Writer(Box);
	WriteGrid(Writer, Ground, RoadHeight, *Inputs.Cover);
	Clock.Next(StageGrid);
	WriteSurfaces(Writer, Roads, Paved, Unpaved, Water, *Inputs.Roads);
	WriteMarkings(Writer, *Inputs.Roads);
	WriteBars(Writer, Inputs);
	WritePois(Writer, Inputs.Contents[TileIndex], Pavement, Ground, RoadHeight);
	for (const FBuilding* Building : Inputs.Contents[TileIndex].Buildings)
	{
		WriteBuilding(Writer, *Building, Ground, Window, Box);
	}
	for (const size_t Query : Inputs.Contents[TileIndex].LanePoints)
	{
		const FStreetPoint& Point = Inputs.LaneQueries[Query];
		(*Inputs.LaneHeights)[Query] = RoadHeight.Sample(Point.X, Point.Y);
	}
	Clock.Next(StageSurfaces);

	const double Reach = FVegetationSources::WindowMargin();
	FVegetationObstacles Obstacles;
	Obstacles.Window = {Box.X0 - Reach, Box.Y0 - Reach, Box.X1 + Reach, Box.Y1 + Reach};
	Obstacles.RoadGround = RoadGround;
	Obstacles.Pavement = Pavement;
	Obstacles.Buildings = Buildings;
	Obstacles.Water = WaterArea;
	Obstacles.Paths = Combined({&Paved, &Unpaved});
	WritePlants(Writer, Inputs.Vegetation->PlantsInTile(Box, Inputs.Extent, Obstacles), Ground);
	Clock.Next(StageVegetation);
	const std::string Name = "tile_" + std::to_string(Tile.IndexX) + "_" + std::to_string(Tile.IndexY) + ".tgtile";
	Writer.Write((OutputDirectory / Name).string());
	Clock.Next(StageWrite);
}

/** Prints the time each tile stage took, summed over all tiles (CPU seconds across the threads). */
void LogStageTimes()
{
	std::string Line = "tile stages (CPU s):";
	char Buffer[64];
	for (int Stage = 0; Stage < StageCount; ++Stage)
	{
		std::snprintf(Buffer, sizeof(Buffer), " %s %.1f,", StageNames[Stage], StageNanoseconds[Stage].load() / 1e9);
		Line += Buffer;
	}
	Line.pop_back();
	Log(Line);
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
				BuildTile(Tiles[Index], Index, Inputs, OutputDirectory);
			}
		});
	}
	for (std::thread& Worker : Workers)
	{
		Worker.join();
	}
}

/** world.json: the tile index, start pose and attribution, as build_world.py writes it. */
void WriteWorldJson(const FRegion& Region, const std::vector<FTileBounds>& Tiles, const FStartPose& Start,
					const std::filesystem::path& Path)
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
	std::string Road;
	for (const char Character : Start.Road)
	{
		if (Character == '"' || Character == '\\')
		{
			Road.push_back('\\');
		}
		Road.push_back(Character);
	}
	std::snprintf(Buffer, sizeof(Buffer), " ],\n \"start\": {\"x\": %.6f, \"y\": %.6f, \"z\": %.6f, \"yaw\": %.6f, \"road\": \"",
				  Start.X, Start.Y, Start.Z, Start.Yaw);
	Output << Buffer << Road << "\"},\n";
	Output << " \"traffic\": \"traffic.json\",\n \"attribution\": [\n"
		   << "  \"\\u00a9 OpenStreetMap contributors (ODbL)\",\n"
		   << "  \"Freie und Hansestadt Hamburg, LGV (dl-de/by-2.0)\",\n"
		   << "  \"LGLN (2025), CC BY 4.0\",\n"
		   << "  \"Copernicus DEM GLO-30, \\u00a9 DLR e.V. 2010-2014 and \\u00a9 Airbus Defence and Space GmbH 2014-2018\"\n"
		   << " ]\n}\n";
}
}

namespace
{
/** The index of the region tile holding a point, or -1 outside the region. */
int TileIndexOf(const FRegion& Region, double X, double Y)
{
	const int Columns = static_cast<int>(std::ceil((Region.XMax - Region.XMin) / Region.TileSize));
	const int Rows = static_cast<int>(std::ceil((Region.YMax - Region.YMin) / Region.TileSize));
	const int Column = static_cast<int>(std::floor((X - Region.XMin) / Region.TileSize));
	const int Row = static_cast<int>(std::floor((Y - Region.YMin) / Region.TileSize));
	if (Column < 0 || Row < 0 || Column >= Columns || Row >= Rows)
	{
		return -1;
	}
	return Row * Columns + Column;
}

/** The furniture and parked cars by the tile they stand in. */
std::vector<FTileContents> SortIntoTiles(const FRegion& Region, const FFurniture& Furniture,
										 const std::vector<FParkedCar>& Cars, const std::vector<FBuilding>& Buildings)
{
	std::vector<FTileContents> Contents(Region.Tiles().size());
	auto Place = [&](double X, double Y, auto Member, const auto* Item) {
		const int Index = TileIndexOf(Region, X, Y);
		if (Index >= 0)
		{
			(Contents[Index].*Member).push_back(Item);
		}
	};
	for (const FLamp& Lamp : Furniture.Lamps)
	{
		Place(Lamp.X, Lamp.Y, &FTileContents::Lamps, &Lamp);
	}
	for (const FSignalHead& Head : Furniture.Heads)
	{
		Place(Head.X, Head.Y, &FTileContents::Heads, &Head);
	}
	for (const FSign& Sign : Furniture.Signs)
	{
		Place(Sign.X, Sign.Y, &FTileContents::Signs, &Sign);
	}
	for (const FParkedCar& Car : Cars)
	{
		Place(Car.X, Car.Y, &FTileContents::Cars, &Car);
	}
	for (const FBuilding& Building : Buildings)
	{
		const FWorldPoint& Point = Building.RepresentativePoint;
		Place(Point.X, Point.Y, &FTileContents::Buildings, &Building);
	}
	return Contents;
}

/**
 * Sorts the lane points needing a height into the tiles, whose road height field fills them in. The few outside every
 * tile (bridge ends past the region's edge) are computed here.
 */
void PrepareLaneHeights(FRegionInputs& Inputs, const FRegion& Region, const FLaneGraph& Lanes,
						const FTerrainGrid& Terrain, const FRoadSurfaces& Roads, std::vector<double>& Heights)
{
	Inputs.LaneQueries = LaneHeightQueries(Lanes);
	Heights.assign(Inputs.LaneQueries.size(), 0.0);
	Inputs.LaneHeights = &Heights;
	for (size_t Query = 0; Query < Inputs.LaneQueries.size(); ++Query)
	{
		const FStreetPoint& Point = Inputs.LaneQueries[Query];
		const int Index = TileIndexOf(Region, Point.X, Point.Y);
		if (Index < 0)
		{
			Heights[Query] = RoadHeightAt(Terrain, Roads.Ground(), Point.X, Point.Y);
			continue;
		}
		Inputs.Contents[Index].LanePoints.push_back(Query);
	}
}

FPolyline ToPolyline(const FStreetPolyline& Line)
{
	FPolyline Points;
	for (const FStreetPoint& Point : Line)
	{
		Points.push_back({Point.X, Point.Y});
	}
	return Points;
}

/** Zebra stripes and stop lines as bars, and the zebras cut out of the painted lines. */
void AddBars(FRegionInputs& Inputs, const FFurniture& Furniture, FRoadSurfaces& Roads)
{
	FPolygonSet Cutout;
	for (const FZebra& Zebra : Furniture.Zebras)
	{
		FZebraBars Bars = MakeZebraBars(Zebra, Roads.Ground());
		for (const FStreetPolyline& Bar : Bars.Bars)
		{
			Inputs.Bars.push_back({ToPolyline(Bar)});
		}
		Cutout.Add(std::move(Bars.Outline));
	}
	Roads.CutMarkings(Cutout);
	for (const FJunctionRecord& Junction : Furniture.Network.Junctions)
	{
		for (const FApproachRecord& Approach : Junction.Approaches)
		{
			if (const std::optional<FStreetPolyline> Line = StopLineGeometry(Approach))
			{
				Inputs.Bars.push_back({ToPolyline(*Line)});
			}
		}
	}
	for (size_t Index = 0; Index < Inputs.Bars.size(); ++Index)
	{
		const FPolyline& Points = Inputs.Bars[Index].Points;
		FBox Box{1e300, 1e300, -1e300, -1e300};
		for (const FWorldPoint& Point : Points)
		{
			Box = {std::min(Box.X0, Point.X), std::min(Box.Y0, Point.Y), std::max(Box.X1, Point.X), std::max(Box.Y1, Point.Y)};
		}
		Inputs.BarIndex.Insert(Box, static_cast<int>(Index));
	}
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
	const unsigned Threads = Options.Threads > 0 ? Options.Threads : std::max(1u, std::thread::hardware_concurrency());
	const FOsmData& Osm = Inputs.Osm;

	// The sources only need the OSM data, so they are prepared side by side; the road surfaces wait for the streets.
	const FBox TerrainExtent{Region.XMin - TerrainMargin, Region.YMin - TerrainMargin, Region.XMax + TerrainMargin,
							 Region.YMax + TerrainMargin};
	auto StreetsTask = std::async(std::launch::async, [&] { return BuildStreetModel(Osm); });
	auto BuildingsTask = std::async(std::launch::async, [&] { return BuildingFootprints(Osm.Buildings); });
	auto WaterTask = std::async(std::launch::async, [&] { return BuildWaterBodies(Osm, Terrain, TerrainExtent); });
	auto CoverTask = std::async(std::launch::async, [&] { return FLandCover(Osm.Areas); });
	auto PathsTask = std::async(std::launch::async, [&] { return FPathSurfaces(Osm); });
	const std::string TreeTable = (Geodata / "raw" / "strassenbaeume" / "street_trees.tsv").string();
	auto KitBuildingsTask = std::async(std::launch::async, [&] {
		return BuildBuildings(Osm, [&](double X, double Y) { return Terrain.Sample(X, Y); }, Threads);
	});
	auto VegetationTask = std::async(std::launch::async, [&] { return FVegetationSources(Osm, ReadStreetTrees(TreeTable)); });

	const FPolygonSet Buildings = BuildingsTask.get();
	Inputs.Buildings = &Buildings;
	const FStreetModel Streets = StreetsTask.get();
	Log("street model: " + std::to_string(Streets.Lines.Layouts.size()) + " segments, "
		+ std::to_string(Streets.Markings.size()) + " painted lines");
	FRoadSurfaces Roads(Streets, Terrain, Buildings, Threads);
	Inputs.Roads = &Roads;
	const FBuildingIndex BuildingIndex(Osm.Buildings);
	const FRoadGraph Graph(*Streets.GroundWays, Streets.Widths, BuildingIndex);
	const FFurniture Furniture = BuildFurniture(Osm, Graph, Roads.Ground(), Buildings);
	const std::vector<FParkedCar> Cars = BuildParkedCars(Osm, Graph, Furniture.Zebras, Roads.Ground(), Buildings, Inputs.Extent);
	AddBars(Inputs, Furniture, Roads);
	const std::vector<FBuilding> KitBuildings = KitBuildingsTask.get();
	Log("buildings: " + std::to_string(KitBuildings.size()) + " footprints");
	Inputs.Contents = SortIntoTiles(Region, Furniture, Cars, KitBuildings);
	WriteTrafficJson((OutputDirectory / "traffic.json").string(), Furniture.Network);
	FLaneGraph Lanes = BuildLaneGraph(Streets, Graph, Furniture, Inputs.Extent);
	std::vector<double> LaneHeights;
	PrepareLaneHeights(Inputs, Region, Lanes, Terrain, Roads, LaneHeights);
	Log("lanes: " + std::to_string(Lanes.Lanes.size()) + ", " + std::to_string(Lanes.GoodCount) + " in the connected network");
	Log("furniture: " + std::to_string(Furniture.Network.Junctions.size()) + " signal junctions, "
		+ std::to_string(Furniture.Heads.size()) + " signal poles, " + std::to_string(Furniture.Signs.size()) + " signs, "
		+ std::to_string(Furniture.Lamps.size()) + " lamps, " + std::to_string(Furniture.Zebras.size()) + " zebras, "
		+ std::to_string(Cars.size()) + " parked cars");
	Log("road surfaces: " + std::to_string(Roads.Ground().Size()) + " pieces, " + std::to_string(Roads.Markings().size())
		+ " marking lines");
	const FWaterBodies Water = WaterTask.get();
	Inputs.Water = &Water;
	const FLandCover Cover = CoverTask.get();
	Inputs.Cover = &Cover;
	const FPathSurfaces Paths = PathsTask.get();
	Inputs.Paths = &Paths;
	const FVegetationSources Vegetation = VegetationTask.get();
	Inputs.Vegetation = &Vegetation;
	Log(std::to_string(Water.Levels.size()) + " water bodies; sources indexed");

	const std::vector<FTileBounds> Tiles = Region.Tiles();
	BuildTiles(Tiles, Inputs, OutputDirectory, Threads);
	LogStageTimes();
	ApplyLaneHeights(Lanes, LaneHeights);
	WriteLanesJson((OutputDirectory / "lanes.json").string(), Lanes);
	Log("wrote lanes.json");
	WriteWorldJson(Region, Tiles, FindStart(Streets, Terrain, Roads.Ground()), OutputDirectory / "world.json");
	Log("wrote " + std::to_string(Tiles.size()) + " tiles to " + OutputDirectory.string() + " on "
		+ std::to_string(Threads) + " threads");
}
}
