/**
 * BuildingsCompare: runs the building pipeline on an OSM extract and writes one JSON line per building, in the format
 * of /tmp/buildings_port/dump_python.py, so the C++ port can be compared with build_world.py field by field.
 *
 *   BuildingsCompare <file.osm.pbf> [--box west south east north] [--dump out.jsonl] [--threads count] [--stages]
 *
 * The ground is flat at height 0. Without --dump only the timings are printed.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "BuildingRecords.h"
#include "OsmReader.h"

namespace
{
using namespace WorldBuilder;
using FClock = std::chrono::steady_clock;

double SecondsSince(FClock::time_point Start)
{
	return std::chrono::duration<double>(FClock::now() - Start).count();
}

void WriteNumber(FILE* File, double Value)
{
	std::fprintf(File, "%.17g", Value);
}

void WritePoint(FILE* File, double X, double Y)
{
	std::fputc('[', File);
	WriteNumber(File, X);
	std::fputc(',', File);
	WriteNumber(File, Y);
	std::fputc(']', File);
}

void WriteRoof(FILE* File, const FRoofGeometry& Roof)
{
	std::fputs("{\"faces\":[", File);
	for (size_t FaceIndex = 0; FaceIndex < Roof.Faces.size(); ++FaceIndex)
	{
		const FRoofFace& Face = Roof.Faces[FaceIndex];
		std::fprintf(File, "%s{\"kind\":%d,\"v\":[", FaceIndex > 0 ? "," : "", Face.Kind);
		for (size_t Index = 0; Index < Face.Vertices.size(); ++Index)
		{
			const FRoofVertex& Vertex = Face.Vertices[Index];
			std::fputs(Index > 0 ? ",[" : "[", File);
			const double Values[5] = {Vertex.X, Vertex.Y, Vertex.Height, Vertex.U, Vertex.V};
			for (int Value = 0; Value < 5; ++Value)
			{
				std::fputs(Value > 0 ? "," : "", File);
				WriteNumber(File, Values[Value]);
			}
			std::fputc(']', File);
		}
		std::fputs("],\"t\":[", File);
		for (size_t Index = 0; Index + 2 < Face.Triangles.size(); Index += 3)
		{
			std::fprintf(File, "%s[%d,%d,%d]", Index > 0 ? "," : "", Face.Triangles[Index], Face.Triangles[Index + 1],
				Face.Triangles[Index + 2]);
		}
		std::fputs("]}", File);
	}
	std::fputs("],\"caps\":[", File);
	for (size_t CapIndex = 0; CapIndex < Roof.Caps.size(); ++CapIndex)
	{
		std::fputs(CapIndex > 0 ? ",[" : "[", File);
		for (int Value = 0; Value < 6; ++Value)
		{
			std::fputs(Value > 0 ? "," : "", File);
			WriteNumber(File, Roof.Caps[CapIndex][static_cast<size_t>(Value)]);
		}
		std::fputc(']', File);
	}
	std::fputs("]}", File);
}

void WriteBuilding(FILE* File, const FBuilding& Building, int Occurrence)
{
	const FBuildingRecord& Record = Building.Record;
	const FBuildingType& Type = Building.Type;
	std::fprintf(File, "{\"id\":%lld,\"k\":%d,\"ring\":[", static_cast<long long>(Building.OsmId), Occurrence);
	for (size_t Index = 0; Index < Building.Footprint.Outline.size(); ++Index)
	{
		std::fputs(Index > 0 ? "," : "", File);
		WritePoint(File, Building.Footprint.Outline[Index].X, Building.Footprint.Outline[Index].Y);
	}
	std::fprintf(File, "],\"holes\":%zu,\"area\":", Building.Footprint.Holes.size());
	WriteNumber(File, PolygonArea(Building.Footprint));
	std::fputs(",\"rep\":", File);
	WritePoint(File, Building.RepresentativePoint.X, Building.RepresentativePoint.Y);
	std::fprintf(File, ",\"facade\":\"%s\",\"roof_section\":\"%s\",\"roof_shape\":%d,\"tint\":%d,\"variation\":%d,\"base_z\":",
		Record.Facade.c_str(), Record.RoofSection.c_str(), Record.RoofShape, Record.Tint, Record.Variation);
	WriteNumber(File, Record.BaseZ);
	std::fputs(",\"eave\":", File);
	WriteNumber(File, Record.EaveHeight);
	std::fputs(",\"mrr\":", File);
	std::array<FWorldPoint, 4> Corners;
	if (MinimumRotatedRectangle(Building.Footprint, Corners))
	{
		std::fputc('[', File);
		for (size_t Index = 0; Index < 4; ++Index)
		{
			std::fputs(Index > 0 ? "," : "", File);
			WritePoint(File, Corners[Index].X, Corners[Index].Y);
		}
		std::fputc(']', File);
	}
	else
	{
		std::fputs("null", File);
	}
	std::fputs(",\"rect\":", File);
	if (Record.bHasRoofRectangle)
	{
		std::fputc('[', File);
		for (size_t Index = 0; Index < 4; ++Index)
		{
			std::fputs(Index > 0 ? "," : "", File);
			WritePoint(File, Record.RoofRectangle[Index].X, Record.RoofRectangle[Index].Y);
		}
		std::fputc(']', File);
	}
	else
	{
		std::fputs("null", File);
	}
	std::fprintf(File, ",\"typ\":{\"class\":%d,\"roof\":%d,\"pitch\":", Type.ClassId, Type.RoofShape);
	WriteNumber(File, Type.PitchDegrees);
	std::fprintf(File, ",\"storeys\":%d,\"attic\":%d,\"flags\":%d,\"tag_bits\":%d", Type.Storeys, Type.AtticLevels, Type.Flags,
		Type.TagBits);
	const std::pair<const char*, double> Values[] = {{"plinth", Type.Plinth}, {"storey_height", Type.StoreyHeight},
		{"ground_height", Type.GroundHeight}, {"eave_height", Type.EaveHeight}, {"ridge_yaw", Type.RidgeYaw},
		{"front_yaw", Type.FrontYaw}};
	for (const auto& [Name, Value] : Values)
	{
		std::fprintf(File, ",\"%s\":", Name);
		WriteNumber(File, Value);
	}
	std::fputs("},\"roofgeo\":", File);
	if (Building.Roof.has_value())
	{
		WriteRoof(File, *Building.Roof);
	}
	else
	{
		std::fputs("null", File);
	}
	std::fputs("}\n", File);
}
}

int main(int ArgumentCount, char** Arguments)
{
	if (ArgumentCount < 2)
	{
		std::fprintf(stderr, "usage: BuildingsCompare <file.osm.pbf> [--box west south east north] [--dump out.jsonl] [--threads count] [--stages]\n");
		return 2;
	}
	FLonLatBox Box;
	const char* DumpPath = nullptr;
	unsigned ThreadCount = 0;
	bool bStages = false;
	for (int Index = 2; Index < ArgumentCount; ++Index)
	{
		if (std::strcmp(Arguments[Index], "--box") == 0 && Index + 4 < ArgumentCount)
		{
			Box = {std::atof(Arguments[Index + 1]), std::atof(Arguments[Index + 2]), std::atof(Arguments[Index + 3]),
				std::atof(Arguments[Index + 4])};
			Index += 4;
		}
		else if (std::strcmp(Arguments[Index], "--dump") == 0 && Index + 1 < ArgumentCount)
		{
			DumpPath = Arguments[++Index];
		}
		else if (std::strcmp(Arguments[Index], "--threads") == 0 && Index + 1 < ArgumentCount)
		{
			ThreadCount = static_cast<unsigned>(std::atoi(Arguments[++Index]));
		}
		else if (std::strcmp(Arguments[Index], "--stages") == 0)
		{
			bStages = true;
		}
	}
	FClock::time_point Start = FClock::now();
	const FOsmData Data = ReadOsm(Arguments[1], Box);
	std::printf("read OSM: %.2f s, %zu buildings, %zu roads, %zu footways, %zu areas\n", SecondsSince(Start), Data.Buildings.size(),
		Data.Roads.size(), Data.Footways.size(), Data.Areas.size());
	const FHeightSampler FlatGround = [](double, double) { return 0.0; };
	if (bStages)
	{
		Start = FClock::now();
		const std::vector<FBuildingFootprint> Footprints = BuildingFootprints(Data);
		std::printf("footprints: %.2f s (%zu)\n", SecondsSince(Start), Footprints.size());
		Start = FClock::now();
		const FBuildingTyper Typer(Footprints, Data.Roads, Data.Footways, Data.Areas);
		std::printf("typer index: %.2f s\n", SecondsSince(Start));
	}
	Start = FClock::now();
	const std::vector<FBuilding> Buildings = BuildBuildings(Data, FlatGround, ThreadCount);
	size_t RoofCount = 0;
	for (const FBuilding& Building : Buildings)
	{
		RoofCount += Building.Roof.has_value() ? 1 : 0;
	}
	std::printf("buildings (footprints, typing, records, roofs): %.2f s, %zu buildings, %zu skeleton roofs\n", SecondsSince(Start),
		Buildings.size(), RoofCount);
	if (DumpPath != nullptr)
	{
		FILE* File = std::fopen(DumpPath, "w");
		if (File == nullptr)
		{
			std::fprintf(stderr, "cannot write %s\n", DumpPath);
			return 1;
		}
		std::map<int64_t, int> Occurrences;
		for (const FBuilding& Building : Buildings)
		{
			WriteBuilding(File, Building, Occurrences[Building.OsmId]++);
		}
		std::fclose(File);
	}
	return 0;
}
