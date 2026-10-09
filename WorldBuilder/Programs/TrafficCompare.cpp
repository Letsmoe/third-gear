/**
 * TrafficCompare: runs the street furniture, zebra, stop line and parked car code on the inputs the Python dump script
 * wrote and writes what it makes in the same text formats, to compare with the Python builders.
 *
 *   TrafficCompare <input.txt> <out_dir> [x0 y0 x1 y1]
 *       The optional box (default -1000 -1000 1000 1000) limits the parked cars to ways touching it.
 *   TrafficCompare --osm <file.osm.pbf> [west south east north]
 *       A timing run on OSM data alone: the road surface is a buffer of every ground way, the buildings are the OSM
 *       footprints.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>

#include "Furniture.h"
#include "OsmReader.h"
#include "Parking.h"
#include "Roads.h"
#include "TrafficJson.h"
#include "Zebras.h"

namespace
{
using namespace WorldBuilder;

double SecondsSince(std::chrono::steady_clock::time_point Start)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
}

/** The inputs of the Python dump script: OSM features and the road surface and buildings as polygon pieces. */
struct FInputs
{
	FOsmData Data;
	FPolygonSet RoadGround;
	FPolygonSet Carriageway;
	FPolygonSet Buildings;
};

/** Reads a dump file; see traffic_dump.py for the format. */
FInputs ReadInputs(const char* Path)
{
	FInputs Inputs;
	std::ifstream File(Path);
	std::string Line;
	FOsmWay* Way = nullptr;
	FOsmPoint* Point = nullptr;
	FTags* Tags = nullptr;
	FPolygons Piece;
	FPolygonSet* Target = nullptr;
	const auto Flush = [&]() {
		if (Target != nullptr)
		{
			Target->Add(Piece);
		}
		Piece.clear();
		Target = nullptr;
	};
	while (std::getline(File, Line))
	{
		if (Line.size() < 2 && Line != "B" && Line != "G" && Line != "C" && Line != "r")
		{
			continue;
		}
		const char Kind = Line[0];
		if (Kind == 'R' || Kind == 'F')
		{
			Flush();
			std::vector<FOsmWay>& List = Kind == 'R' ? Inputs.Data.Roads : Inputs.Data.Footways;
			Way = &List.emplace_back();
			Way->Id = std::atoll(Line.c_str() + 2);
			Tags = &Way->Tags;
			Point = nullptr;
		}
		else if (Kind == 'P')
		{
			Flush();
			Point = &Inputs.Data.Points.emplace_back();
			long long Id = 0;
			std::sscanf(Line.c_str() + 2, "%lld %lf %lf", &Id, &Point->Position.X, &Point->Position.Y);
			Point->Id = Id;
			Tags = &Point->Tags;
			Way = nullptr;
		}
		else if (Kind == 'T')
		{
			const size_t Tab = Line.find('\t');
			Tags->emplace_back(Line.substr(2, Tab - 2), Line.substr(Tab + 1));
		}
		else if (Kind == 'N')
		{
			long long NodeId = 0;
			double X = 0.0;
			double Y = 0.0;
			std::sscanf(Line.c_str() + 2, "%lld %lf %lf", &NodeId, &X, &Y);
			Way->NodeIds.push_back(NodeId);
			Way->Points.push_back({X, Y});
		}
		else if (Kind == 'B' || Kind == 'G' || Kind == 'C')
		{
			Flush();
			Target = Kind == 'B' ? &Inputs.Buildings : (Kind == 'G' ? &Inputs.RoadGround : &Inputs.Carriageway);
		}
		else if (Kind == 'r')
		{
			Piece.emplace_back();
		}
		else if (Kind == 'v')
		{
			double X = 0.0;
			double Y = 0.0;
			std::sscanf(Line.c_str() + 2, "%lf %lf", &X, &Y);
			Piece.back().emplace_back(X, Y);
		}
	}
	Flush();
	return Inputs;
}

/** The buildings of the OSM data as polygons for the street model's town test (outline rings only is enough). */
void AddBuildingsToData(FInputs& Inputs)
{
	for (const FPolygons& Piece : Inputs.Buildings.All())
	{
		FOsmArea Area;
		FOsmPolygon Polygon;
		for (const Clipper2Lib::PathD& Ring : Piece)
		{
			std::vector<FWorldPoint> Points;
			for (const Clipper2Lib::PointD& Corner : Ring)
			{
				Points.push_back({Corner.x, Corner.y});
			}
			Polygon.Rings.push_back(std::move(Points));
		}
		Area.Polygons.push_back(std::move(Polygon));
		Inputs.Data.Buildings.push_back(std::move(Area));
	}
}

/** The road height both comparison programs use. */
double MadeUpHeight(double X, double Y)
{
	return 0.013 * X - 0.007 * Y + 0.5 * std::sin(0.1 * X);
}

void WriteFurniture(const FFurniture& Furniture, const std::vector<FParkedCar>& Cars, const std::string& Directory)
{
	std::FILE* File = std::fopen((Directory + "/lamps.txt").c_str(), "w");
	for (const FLamp& Lamp : Furniture.Lamps)
	{
		std::fprintf(File, "%.6f %.6f %.6f %s\n", Lamp.X, Lamp.Y, Lamp.YawDegrees, Lamp.bFromOsm ? "osm" : "lit");
	}
	std::fclose(File);
	File = std::fopen((Directory + "/heads.txt").c_str(), "w");
	for (const FSignalHead& Head : Furniture.Heads)
	{
		std::fprintf(File, "%.6f %.6f %.6f %d %s %d\n", Head.X, Head.Y, Head.YawDegrees, Head.Approach,
					 Head.bLeftSide ? "left" : "right", Head.Junction);
	}
	std::fclose(File);
	File = std::fopen((Directory + "/signs.txt").c_str(), "w");
	for (const FSign& Sign : Furniture.Signs)
	{
		std::string Names;
		for (const std::string& Name : Sign.Names)
		{
			Names += (Names.empty() ? "" : ",") + Name;
		}
		std::fprintf(File, "%.6f %.6f %.6f %s\n", Sign.X, Sign.Y, Sign.YawDegrees, Names.c_str());
	}
	std::fclose(File);
	File = std::fopen((Directory + "/parked.txt").c_str(), "w");
	for (const FParkedCar& Car : Cars)
	{
		const FParkedCarPose Pose = FinishParkedCar(Car, MadeUpHeight);
		std::fprintf(File, "%.6f %.6f %.6f %.6f %.6f %.6f %d\n", Car.X, Car.Y, Pose.Z, Car.YawDegrees, Pose.RollDegrees,
					 Pose.PitchDegrees, Car.Model);
	}
	std::fclose(File);
}

void WriteZebrasAndStopLines(const FFurniture& Furniture, const FPolygonSet& Carriageway, const std::string& Directory)
{
	std::FILE* File = std::fopen((Directory + "/zebras.txt").c_str(), "w");
	for (const FZebra& Zebra : Furniture.Zebras)
	{
		const FZebraBars Bars = MakeZebraBars(Zebra, Carriageway);
		std::fprintf(File, "Z %.6f %.6f %.6f %.6f %.6f %zu\n", Zebra.X, Zebra.Y, Zebra.Direction.X, Zebra.Direction.Y,
					 Zebra.Width, Bars.Bars.size());
		for (const FZebraSpan& Span : Bars.Spans)
		{
			std::fprintf(File, "S %.6f %.6f %.6f %.6f\n", Span.Low, Span.High, Span.BarLow, Span.BarHigh);
		}
		for (const FStreetPolyline& Bar : Bars.Bars)
		{
			std::fprintf(File, "L bar %.6f %.6f %.6f %.6f\n", Bar[0].X, Bar[0].Y, Bar[1].X, Bar[1].Y);
		}
		std::fprintf(File, "O");
		for (const Clipper2Lib::PointD& Corner : Bars.Outline[0])
		{
			std::fprintf(File, " %.6f %.6f", Corner.x, Corner.y);
		}
		std::fprintf(File, "\n");
	}
	std::fclose(File);
	File = std::fopen((Directory + "/stoplines.txt").c_str(), "w");
	for (const FJunctionRecord& Junction : Furniture.Network.Junctions)
	{
		for (const FApproachRecord& Approach : Junction.Approaches)
		{
			const std::optional<FStreetPolyline> Line = StopLineGeometry(Approach);
			if (Line.has_value())
			{
				std::fprintf(File, "%d %.6f %.6f %.6f %.6f\n", Approach.Id, (*Line)[0].X, (*Line)[0].Y, (*Line)[1].X,
							 (*Line)[1].Y);
			}
		}
	}
	std::fclose(File);
}
}

/** A rough road surface for timing runs: each ground way buffered by half its width. */
FPolygonSet RoughRoadSurface(const FStreetModel& Model)
{
	FPolygonSet Surface;
	for (const FStreetWay& Way : *Model.GroundWays)
	{
		FPolyline Line;
		for (const FStreetPoint& Point : Way.Points)
		{
			Line.push_back({Point.X, Point.Y});
		}
		Surface.Add(BufferPolyline(Line, Model.Widths.at(Way.Id) / 2, ECapStyle::Round, 4));
	}
	return Surface;
}

/** The building footprints of OSM data as a polygon set. */
FPolygonSet BuildingSet(const FOsmData& Data)
{
	FPolygonSet Set;
	for (const FOsmArea& Building : Data.Buildings)
	{
		for (const FOsmPolygon& Polygon : Building.Polygons)
		{
			FPolygons Piece;
			for (const std::vector<FWorldPoint>& Ring : Polygon.Rings)
			{
				Piece.emplace_back();
				for (const FWorldPoint& Point : Ring)
				{
					Piece.back().emplace_back(Point.X, Point.Y);
				}
			}
			Set.Add(std::move(Piece));
		}
	}
	return Set;
}

/** The timing run on OSM data alone. */
int RunOsm(int ArgumentCount, char** Arguments)
{
	FLonLatBox Box;
	if (ArgumentCount == 7)
	{
		Box = {std::atof(Arguments[3]), std::atof(Arguments[4]), std::atof(Arguments[5]), std::atof(Arguments[6])};
	}
	auto Start = std::chrono::steady_clock::now();
	const FOsmData Data = ReadOsm(Arguments[2], Box);
	std::fprintf(stderr, "read osm %.2f s: %zu roads, %zu buildings\n", SecondsSince(Start), Data.Roads.size(),
				 Data.Buildings.size());
	Start = std::chrono::steady_clock::now();
	const FStreetModel Model = BuildStreetModel(Data);
	std::fprintf(stderr, "street model %.2f s\n", SecondsSince(Start));
	Start = std::chrono::steady_clock::now();
	const FPolygonSet Surface = RoughRoadSurface(Model);
	const FPolygonSet Buildings = BuildingSet(Data);
	std::fprintf(stderr, "rough surface and building set %.2f s\n", SecondsSince(Start));
	Start = std::chrono::steady_clock::now();
	const FBuildingIndex BuildingIndex(Data.Buildings);
	const FRoadGraph Graph(*Model.GroundWays, Model.Widths, BuildingIndex);
	std::fprintf(stderr, "road graph %.2f s\n", SecondsSince(Start));
	Start = std::chrono::steady_clock::now();
	const FFurniture Furniture = BuildFurniture(Data, Graph, Surface, Buildings);
	std::fprintf(stderr, "furniture %.2f s: %zu junctions, %zu heads, %zu signs, %zu lamps, %zu zebras\n",
				 SecondsSince(Start), Furniture.Network.Junctions.size(), Furniture.Heads.size(),
				 Furniture.Signs.size(), Furniture.Lamps.size(), Furniture.Zebras.size());
	Start = std::chrono::steady_clock::now();
	size_t BarCount = 0;
	for (const FZebra& Zebra : Furniture.Zebras)
	{
		BarCount += MakeZebraBars(Zebra, Surface).Bars.size();
	}
	std::fprintf(stderr, "zebra bars %.2f s: %zu\n", SecondsSince(Start), BarCount);
	Start = std::chrono::steady_clock::now();
	const std::vector<FParkedCar> Cars =
		BuildParkedCars(Data, Graph, Furniture.Zebras, Surface, Buildings, std::nullopt);
	std::fprintf(stderr, "parked cars %.2f s: %zu\n", SecondsSince(Start), Cars.size());
	Start = std::chrono::steady_clock::now();
	const std::string Json = TrafficJsonText(Furniture.Network);
	std::fprintf(stderr, "traffic json %.2f s: %zu bytes\n", SecondsSince(Start), Json.size());
	return 0;
}

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount >= 3 && std::string(Arguments[1]) == "--osm")
	{
		return RunOsm(ArgumentCount, Arguments);
	}
	if (ArgumentCount != 3 && ArgumentCount != 7)
	{
		std::fprintf(stderr, "usage: TrafficCompare <input.txt> <out_dir> [x0 y0 x1 y1]\n");
		return 2;
	}
	const std::string Directory = Arguments[2];
	auto Start = std::chrono::steady_clock::now();
	FInputs Inputs = ReadInputs(Arguments[1]);
	AddBuildingsToData(Inputs);
	std::fprintf(stderr, "read inputs %.2f s: %zu roads, %zu points, %zu ground pieces\n", SecondsSince(Start),
				 Inputs.Data.Roads.size(), Inputs.Data.Points.size(), Inputs.RoadGround.Size());

	Start = std::chrono::steady_clock::now();
	const FStreetModel Model = BuildStreetModel(Inputs.Data);
	std::fprintf(stderr, "street model %.2f s\n", SecondsSince(Start));

	Start = std::chrono::steady_clock::now();
	const FBuildingIndex BuildingIndex(Inputs.Data.Buildings);
	const FRoadGraph Graph(*Model.GroundWays, Model.Widths, BuildingIndex);
	const FFurniture Furniture = BuildFurniture(Inputs.Data, Graph, Inputs.RoadGround, Inputs.Buildings);
	std::fprintf(stderr, "furniture %.2f s: %zu junctions, %zu heads, %zu signs, %zu lamps, %zu zebras\n",
				 SecondsSince(Start), Furniture.Network.Junctions.size(), Furniture.Heads.size(),
				 Furniture.Signs.size(), Furniture.Lamps.size(), Furniture.Zebras.size());
	WriteTrafficJson(Directory + "/traffic.json", Furniture.Network);

	Start = std::chrono::steady_clock::now();
	FBox Region = {-1000.0, -1000.0, 1000.0, 1000.0};
	if (ArgumentCount == 7)
	{
		Region = {std::atof(Arguments[3]), std::atof(Arguments[4]), std::atof(Arguments[5]), std::atof(Arguments[6])};
	}
	const std::vector<FParkedCar> Cars =
		BuildParkedCars(Inputs.Data, Graph, Furniture.Zebras, Inputs.Carriageway, Inputs.Buildings, Region);
	std::fprintf(stderr, "parked cars %.2f s: %zu\n", SecondsSince(Start), Cars.size());
	WriteFurniture(Furniture, Cars, Directory);
	WriteZebrasAndStopLines(Furniture, Inputs.Carriageway, Directory);
	return 0;
}
