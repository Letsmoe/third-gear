/**
 * StreetsCompare: runs the street model on an OSM extract and writes what it makes, to compare with the Python model.
 *
 *   StreetsCompare <file.osm.pbf> <out.txt> [west south east north]
 *
 * The output has one line per painted line ("L <kind> <x> <y> <x> <y>...") and one per way ("W <way id> <width>"),
 * and the timings go to stderr.
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "OsmReader.h"
#include "Roads.h"

namespace
{
using namespace WorldBuilder;

double SecondsSince(std::chrono::steady_clock::time_point Start)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
}

/**
 * Replaces the roads by those of a file the Python dump script wrote ("R id" then "T key<tab>value" and "N node x y"
 * lines), so both models start from identical coordinates and any difference left is the model's.
 */
void ReplaceRoads(FOsmData& Data, const char* Path)
{
	std::ifstream File(Path);
	std::string Line;
	Data.Roads.clear();
	while (std::getline(File, Line))
	{
		if (Line.rfind("R ", 0) == 0)
		{
			Data.Roads.emplace_back();
			Data.Roads.back().Id = std::atoll(Line.c_str() + 2);
		}
		else if (Line.rfind("T ", 0) == 0)
		{
			const size_t Tab = Line.find('\t');
			Data.Roads.back().Tags.emplace_back(Line.substr(2, Tab - 2), Line.substr(Tab + 1));
		}
		else if (Line.rfind("N ", 0) == 0)
		{
			long long NodeId = 0;
			double X = 0.0;
			double Y = 0.0;
			std::sscanf(Line.c_str() + 2, "%lld %lf %lf", &NodeId, &X, &Y);
			Data.Roads.back().NodeIds.push_back(NodeId);
			Data.Roads.back().Points.push_back({X, Y});
		}
	}
}

/** Writes the markings and the way widths in the format the Python dump script writes. */
void WriteModel(const FStreetModel& Model, const char* Path)
{
	std::FILE* File = std::fopen(Path, "w");
	if (File == nullptr)
	{
		std::fprintf(stderr, "cannot write %s\n", Path);
		return;
	}
	for (const FMarking& Marking : Model.Markings)
	{
		std::fprintf(File, "L %s", Marking.Kind.c_str());
		for (const FStreetPoint& Point : Marking.Points)
		{
			std::fprintf(File, " %.4f %.4f", Point.X, Point.Y);
		}
		std::fprintf(File, "\n");
	}
	for (const auto& [WayId, Width] : Model.Widths)
	{
		std::fprintf(File, "W %lld %.4f\n", static_cast<long long>(WayId), Width);
	}
	std::fclose(File);
}

/** Text for a line key like the Python dump: kind:side:travel:index. */
std::string KeyText(const FLineKey& Key)
{
	static const char* Kinds[] = {"kerb", "edge", "centre", "lane", "cycle", "cycle_outer"};
	static const char* Sides[] = {"-", "left", "right"};
	static const char* Travels[] = {"forward", "backward", "both", "-"};
	return std::string(Kinds[static_cast<int>(Key.Kind)]) + ":" + Sides[static_cast<int>(Key.Side)] + ":"
		+ Travels[static_cast<int>(Key.Travel)] + ":" + std::to_string(Key.Index);
}

/** Sorted "key=value" text of line offsets, in the order the Python dump sorts them. */
std::string OffsetsText(const FLineOffsets& Offsets)
{
	std::vector<std::pair<std::string, double>> Entries;
	for (const auto& [Key, Value] : Offsets)
	{
		Entries.emplace_back(KeyText(Key), Value);
	}
	std::sort(Entries.begin(), Entries.end());
	std::string Text;
	char Number[64];
	for (const auto& [Key, Value] : Entries)
	{
		std::snprintf(Number, sizeof(Number), "=%.3f", Value);
		Text += (Text.empty() ? "" : " ") + Key + Number;
	}
	return Text;
}

/** Writes the end shapes of every segment like the Python dump, for finding where the two models part. */
void WriteCuts(const FStreetModel& Model, const std::string& Path)
{
	std::FILE* File = std::fopen(Path.c_str(), "w");
	if (File == nullptr)
	{
		return;
	}
	for (const FSegmentLayout& Layout : Model.Lines.Layouts)
	{
		const FSegment& Segment = *Layout.Segment;
		for (bool bAtStart : {true, false})
		{
			const FEndShape& Shape = EndShapeOf(Layout, bAtStart);
			std::fprintf(File, "%lld %lld %lld %c taper=%.3f | %s | %s\n", static_cast<long long>(Segment.Way->Id),
						 static_cast<long long>(Segment.StartNode), static_cast<long long>(Segment.EndNode),
						 bAtStart ? 's' : 'e', Shape.Taper, OffsetsText(Shape.Cuts).c_str(),
						 OffsetsText(Shape.NodeOffsets).c_str());
		}
	}
	std::fclose(File);
}
}

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 3 && ArgumentCount != 7)
	{
		std::fprintf(stderr, "usage: StreetsCompare <file.osm.pbf> <out.txt> [west south east north]\n");
		return 2;
	}
	FLonLatBox Box;
	if (ArgumentCount == 7)
	{
		Box = {std::atof(Arguments[3]), std::atof(Arguments[4]), std::atof(Arguments[5]), std::atof(Arguments[6])};
	}
	auto Start = std::chrono::steady_clock::now();
	FOsmData Data = ReadOsm(Arguments[1], Box);
	if (std::getenv("STREETS_ROADS") != nullptr)
	{
		ReplaceRoads(Data, std::getenv("STREETS_ROADS"));
	}
	std::fprintf(stderr, "read osm %.2f s: %zu roads, %zu buildings\n", SecondsSince(Start), Data.Roads.size(),
				 Data.Buildings.size());
	Start = std::chrono::steady_clock::now();
	const FStreetModel Model = BuildStreetModel(Data);
	std::fprintf(stderr, "street model %.2f s: %zu segments, %zu markings, %zu gores\n", SecondsSince(Start),
				 Model.Lines.Network->Segments.size(), Model.Markings.size(), Model.Lines.Gores.size());
	WriteModel(Model, Arguments[2]);
	WriteCuts(Model, std::string(Arguments[2]) + ".cuts");
	return 0;
}
