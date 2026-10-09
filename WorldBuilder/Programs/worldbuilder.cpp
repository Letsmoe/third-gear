/**
 * worldbuilder: the command line of the world builder.
 *
 *   worldbuilder read-osm <file.osm.pbf> [west south east north]
 *       Reads the OSM features the world uses, optionally only those with a node inside the box (degrees), and
 *       prints how many of each kind there are and how long it took.
 *   worldbuilder project <longitude> <latitude>
 *       Prints the UTM and world coordinates of a point, to check the projection.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "OsmReader.h"
#include "Projection.h"

namespace
{
using namespace WorldBuilder;

int PrintUsage()
{
	std::fprintf(stderr, "usage: worldbuilder read-osm <file.osm.pbf> [west south east north]\n"
						 "       worldbuilder project <longitude> <latitude>\n");
	return 2;
}

size_t CountPolygons(const std::vector<FOsmArea>& Areas)
{
	size_t Count = 0;
	for (const FOsmArea& Area : Areas)
	{
		Count += Area.Polygons.size();
	}
	return Count;
}

int ReadOsmCommand(int ArgumentCount, char** Arguments)
{
	FLonLatBox Box;
	if (ArgumentCount == 7)
	{
		Box = {std::atof(Arguments[3]), std::atof(Arguments[4]), std::atof(Arguments[5]), std::atof(Arguments[6])};
	}
	else if (ArgumentCount != 3)
	{
		return PrintUsage();
	}
	const auto Start = std::chrono::steady_clock::now();
	const FOsmData Data = ReadOsm(Arguments[2], Box);
	const double Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - Start).count();
	std::printf("roads %zu\nfootways %zu\nrailways %zu\nwaterways %zu\ntree_rows %zu\nhedges %zu\n", Data.Roads.size(),
				Data.Footways.size(), Data.Railways.size(), Data.Waterways.size(), Data.TreeRows.size(),
				Data.Hedges.size());
	std::printf("buildings %zu (%zu polygons)\nareas %zu (%zu polygons)\npoints %zu\n", Data.Buildings.size(),
				CountPolygons(Data.Buildings), Data.Areas.size(), CountPolygons(Data.Areas), Data.Points.size());
	std::printf("seconds %.2f\n", Seconds);
	return 0;
}

int ProjectCommand(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount != 4)
	{
		return PrintUsage();
	}
	const double Longitude = std::atof(Arguments[2]);
	const double Latitude = std::atof(Arguments[3]);
	const FUtmPoint Utm = LonLatToUtm(Longitude, Latitude);
	const FWorldPoint World = LonLatToWorld(Longitude, Latitude);
	std::printf("%.4f %.4f %.4f %.4f\n", Utm.East, Utm.North, World.X, World.Y);
	return 0;
}
}

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount < 2)
	{
		return PrintUsage();
	}
	const std::string Command = Arguments[1];
	if (Command == "read-osm")
	{
		return ReadOsmCommand(ArgumentCount, Arguments);
	}
	if (Command == "project")
	{
		return ProjectCommand(ArgumentCount, Arguments);
	}
	return PrintUsage();
}
