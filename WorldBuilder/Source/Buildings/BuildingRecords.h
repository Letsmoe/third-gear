#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "BuildingGeometry.h"
#include "BuildingTypes.h"
#include "OsmData.h"
#include "Roofs.h"

/**
 * What build_world.py decides for every building: the footprint list (building_footprints), the BLDG record values
 * (write_building), the BTYP record (building_types.py) and the ROOF record (roofs.py). The tile writer only has to
 * pack these into the file; positions are in world metres, the writer makes them tile relative.
 */
namespace WorldBuilder
{
/** Roof shape values of the BLDG record. */
enum EBuildingRoofShape : uint8_t
{
	BuildingRoofFlat = 0,
	BuildingRoofGabled = 1,
};

/** The values of one BLDG record except the footprint rings. */
struct FBuildingRecord
{
	int64_t OsmId = 0;
	/** Material section names; the writer turns them into indices of the NAME table. */
	std::string Facade;
	std::string RoofSection;
	uint8_t RoofShape = BuildingRoofFlat;
	uint8_t Tint = 0;
	uint8_t Variation = 0;
	float BaseZ = 0.0f;
	float EaveHeight = 0.0f;
	/** Gabled roofs only: the footprint's minimum rotated rectangle, 4 corners in world metres. */
	bool bHasRoofRectangle = false;
	std::array<FWorldPoint, 4> RoofRectangle{};
};

/** Everything the tiles carry for one building. */
struct FBuilding
{
	int64_t OsmId = 0;
	FBuildingPolygon Footprint;
	/** A point inside the footprint; the building belongs to the tile it lies in. */
	FWorldPoint RepresentativePoint;
	FBuildingRecord Record;
	FBuildingType Type;
	/** Present for the buildings whose roof is not a plain gable over a rectangle (ROOF record). */
	std::optional<FRoofGeometry> Roof;
};

/** Ground height at a world position, for the base height of a building (the lowest value along its outline). */
using FHeightSampler = std::function<double(double X, double Y)>;

/**
 * Every usable building polygon of the OSM data: polygons under 4 square metres dropped, simplified by 0.15 m and made
 * valid. The result points into the tags of Data, which must outlive it.
 */
std::vector<FBuildingFootprint> BuildingFootprints(const FOsmData& Data);

/** The BLDG record values for one footprint: building parameters, facade style, rectangle test, roof shape, tint and base height. */
FBuildingRecord DecideBuildingRecord(int64_t OsmId, const FTags& Tags, const FBuildingPolygon& Footprint,
	const FHeightSampler& SampleGround);

/**
 * Runs the whole building pipeline for the OSM data: footprints, records, typing and roofs, in the order of
 * BuildingFootprints. ThreadCount 0 uses all cores.
 */
std::vector<FBuilding> BuildBuildings(const FOsmData& Data, const FHeightSampler& SampleGround, unsigned ThreadCount = 0);
}
