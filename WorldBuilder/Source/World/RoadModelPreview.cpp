#include "RoadModelPreview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "Centreline.h"
#include "HeightGrid.h"
#include "OsmReader.h"
#include "ParallelFor.h"
#include "PolygonSet.h"
#include "RoadModel.h"
#include "RoadSurfaces.h"
#include "Roads.h"

namespace WorldBuilder
{
namespace
{
/** OSM is read this far around the region, as for the tiles, so roads leaving it keep their junctions. */
constexpr double OsmMargin = 150.0;
/** Chains are written when they come this close to the region. */
constexpr double ChainMargin = 20.0;
/** The curve radius is measured over this many samples either side, which evens out the metre steps. */
constexpr int RadiusReach = 3;
/** Radii above this are written as this, which keeps the file free of infinities. */
constexpr double StraightRadius = 9999.0;
/** True when any point of the chain lies within ChainMargin of the region. */
bool TouchesRegion(const FRoadChainModel& Chain, const FRegion& Region)
{
	for (const FStreetPoint& Point : Chain.Points)
	{
		if (Point.X > Region.XMin - ChainMargin && Point.X < Region.XMax + ChainMargin && Point.Y > Region.YMin - ChainMargin
			&& Point.Y < Region.YMax + ChainMargin)
		{
			return true;
		}
	}
	return false;
}

/** Writes Values as a JSON array with the given number of decimals. */
void WriteNumbers(std::ofstream& File, const std::vector<double>& Values, int Decimals)
{
	File << '[';
	char Buffer[32];
	for (size_t Index = 0; Index < Values.size(); ++Index)
	{
		std::snprintf(Buffer, sizeof(Buffer), "%s%.*f", Index > 0 ? "," : "", Decimals, Values[Index]);
		File << Buffer;
	}
	File << ']';
}

/** Writes a polyline as a JSON array of [x, y] pairs in centimetre precision. */
void WritePoints(std::ofstream& File, const FStreetPolyline& Points)
{
	File << '[';
	char Buffer[64];
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		std::snprintf(Buffer, sizeof(Buffer), "%s[%.2f,%.2f]", Index > 0 ? "," : "", Points[Index].X, Points[Index].Y);
		File << Buffer;
	}
	File << ']';
}

/** Writes a string as a JSON string, escaping quotes and backslashes. */
void WriteString(std::ofstream& File, const std::string& Text)
{
	File << '"';
	for (const char Character : Text)
	{
		if (Character == '"' || Character == '\\')
		{
			File << '\\';
		}
		File << Character;
	}
	File << '"';
}

/** The radius at every sample, capped at StraightRadius. */
std::vector<double> CappedRadii(const FStreetPolyline& Points)
{
	std::vector<double> Radii = CurveRadii(Points, RadiusReach);
	for (double& Radius : Radii)
	{
		Radius = std::min(Radius, StraightRadius);
	}
	return Radii;
}

/** Writes the junctions where a chain yields: its sample there and the class, name and centre height of the major. */
void WriteYields(std::ofstream& File, const FRoadChainModel& Chain, const FRoadModel& Model)
{
	File << ",\"yields\":[";
	for (size_t Index = 0; Index < Chain.Yields.size(); ++Index)
	{
		const FJunctionYield& Yield = Chain.Yields[Index];
		const FRoadChainModel& Major = Model.Chains[Yield.Major];
		File << (Index > 0 ? "," : "") << "{\"node_sample\":" << Yield.NodeSample << ",\"class\":";
		WriteString(File, Major.RoadClass);
		File << ",\"name\":";
		WriteString(File, Major.Name);
		char Buffer[64];
		std::snprintf(Buffer, sizeof(Buffer), ",\"height\":%.3f}", Major.Heights[Yield.MajorNodeSample]);
		File << Buffer;
	}
	File << "]";
}

/** Writes one chain as a JSON object; Today holds the road heights the tiles use now. */
void WriteChain(std::ofstream& File, const FRoadChainModel& Chain, const FRoadModel& Model, const std::vector<double>& Today)
{
	File << "{\"class\":";
	WriteString(File, Chain.RoadClass);
	File << ",\"name\":";
	WriteString(File, Chain.Name);
	char Buffer[256];
	std::snprintf(Buffer, sizeof(Buffer),
				  ",\"closed\":%s,\"max_deviation\":%.2f,\"design\":{\"plan_smoothing\":%.1f,\"max_deviation\":%.2f,"
				  "\"profile_smoothing\":%.1f,\"max_grade\":%.3f}",
				  Chain.Chain.bClosed ? "true" : "false", Chain.MaxDeviation, Chain.Design.PlanSmoothing,
				  Chain.Design.MaxDeviation, Chain.Design.ProfileSmoothing, Chain.Design.MaxGrade);
	File << Buffer << ",\"osm\":";
	WritePoints(File, Chain.Original);
	File << ",\"points\":";
	WritePoints(File, Chain.Points);
	File << ",\"along\":";
	WriteNumbers(File, Chain.Along, 2);
	File << ",\"ground\":";
	WriteNumbers(File, Chain.Ground, 3);
	File << ",\"today\":";
	WriteNumbers(File, Today, 3);
	File << ",\"height\":";
	WriteNumbers(File, Chain.Heights, 3);
	File << ",\"under_bridge\":[";
	for (size_t Index = 0; Index < Chain.UnderBridge.size(); ++Index)
	{
		File << (Index > 0 ? "," : "") << (Chain.UnderBridge[Index] ? 1 : 0);
	}
	File << "]";
	std::vector<double> LeftFalls;
	std::vector<double> RightFalls;
	for (const FCrossFall& Fall : Chain.Falls)
	{
		LeftFalls.push_back(Fall.Left);
		RightFalls.push_back(Fall.Right);
	}
	File << ",\"left_fall\":";
	WriteNumbers(File, LeftFalls, 4);
	File << ",\"right_fall\":";
	WriteNumbers(File, RightFalls, 4);
	File << ",\"radius\":";
	WriteNumbers(File, CappedRadii(Chain.Points), 1);
	WriteYields(File, Chain, Model);
	File << ",\"node_samples\":[";
	for (size_t Index = 0; Index < Chain.NodeSamples.size(); ++Index)
	{
		File << (Index > 0 ? "," : "") << Chain.NodeSamples[Index];
	}
	File << "]}";
}

/** The road height the tiles use today at every point of every chain. */
std::vector<std::vector<double>> TodayHeights(const std::vector<const FRoadChainModel*>& Chains,
											  const FTerrainGrid& Terrain, const FPolygonSet& RoadGround, unsigned Threads)
{
	std::vector<std::vector<double>> Heights(Chains.size());
	ParallelFor(Chains.size(), Threads, [&](size_t Index) {
		for (const FStreetPoint& Point : Chains[Index]->Points)
		{
			Heights[Index].push_back(RoadHeightAt(Terrain, RoadGround, Point.X, Point.Y));
		}
	});
	return Heights;
}

double SecondsSince(const std::chrono::steady_clock::time_point& Start)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
}
}

void WriteRoadModelPreview(const FRegion& Region, const FBuildOptions& Options)
{
	const auto Start = std::chrono::steady_clock::now();
	const std::filesystem::path DataRoot(Options.DataRoot.empty() ? DefaultDataRoot() : Options.DataRoot);
	const std::filesystem::path Geodata = DataRoot / "geodata";
	const std::filesystem::path OutputDirectory = DataRoot / "world" / (Options.OutputName.empty() ? Region.Name : Options.OutputName);
	std::filesystem::create_directories(OutputDirectory);
	const unsigned Threads = ResolveThreadCount(Options.Threads);

	const FOsmData Osm = ReadOsm((Geodata / "osm" / (Region.OsmExtract + ".osm.pbf")).string(), Region.LonLatBounds(OsmMargin));
	const FTerrainGrid Terrain((Geodata / "raw" / "terrain_5m.grid").string());
	const FStreetModel Streets = BuildStreetModel(Osm);
	std::printf("street model: %zu segments (%.1f s)\n", Streets.Lines.Network->Segments.size(), SecondsSince(Start));

	const auto ModelStart = std::chrono::steady_clock::now();
	const FRoadModel Model = BuildRoadModel(*Streets.Lines.Network, [&](double X, double Y) { return Terrain.Sample(X, Y); },
											BridgeCoverOf(Osm, Streets.Widths), Streets.Urban);
	std::printf("road model: %zu chains, %d steps at the grade limit (%.2f s)\n", Model.Chains.size(), Model.GradeLimited,
				SecondsSince(ModelStart));

	std::vector<const FRoadChainModel*> Written;
	for (const FRoadChainModel& Chain : Model.Chains)
	{
		if (TouchesRegion(Chain, Region))
		{
			Written.push_back(&Chain);
		}
	}
	const FPolygonSet NoBuildings;
	// Today's road heights for the comparison: no designed heights, so the surfaces take the smoothed terrain.
	const FDesignedHeight Undesigned = [](double, double) { return std::optional<double>(); };
	const FRoadSurfaces Roads(Streets, Terrain, NoBuildings, Undesigned, Threads);
	const std::vector<std::vector<double>> Today = TodayHeights(Written, Terrain, Roads.Ground(), Threads);

	const std::filesystem::path Path = OutputDirectory / "road_model.json";
	std::ofstream File(Path);
	if (!File)
	{
		throw std::runtime_error("cannot write " + Path.string());
	}
	File << "{\"region\":";
	WriteString(File, Region.Name);
	File << ",\"chains\":[";
	for (size_t Index = 0; Index < Written.size(); ++Index)
	{
		File << (Index > 0 ? ",\n" : "\n");
		WriteChain(File, *Written[Index], Model, Today[Index]);
	}
	File << "]}\n";
	std::printf("wrote %zu chains to %s (%.1f s)\n", Written.size(), Path.string().c_str(), SecondsSince(Start));
}
}
