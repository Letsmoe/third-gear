#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "OsmData.h"

/**
 * Building height, roof and facade defaults from OSM tags: the rules of Tools/osmimport/osmimport/buildings.py that
 * the world tiles use (the mesh code of that module is not ported).
 */
namespace WorldBuilder
{
constexpr double LevelHeight = 3.0;

/** The number at the start of a tag value, buildings.py's _float: the part before the first ';', comma as point, 'm' dropped. */
std::optional<double> ParseTagNumber(const std::string* Value);

/** Python truthiness of an optional number: present and not zero. */
bool IsTruthy(const std::optional<double>& Value);

/** The hash in 0..1 that fixes a building's random choices from its OSM id and a salt (CRC-32 of "id:salt", low 16 bits). */
double Hash01(int64_t OsmId, int Salt = 0);

/** Building type, eave height and roof kind decided by building_params. */
struct FBuildingParams
{
	std::string BuildingType;
	double EaveHeight = 0.0;
	/** "gabled" or "flat". */
	std::string Roof;
};

/** The type, eave height and roof kind of a building from its tags and footprint area. */
FBuildingParams BuildingParams(const FTags& Tags, double FootprintArea);

/** The facade and roof material section names for a building type and OSM id. */
void FacadeStyle(int64_t OsmId, const std::string& BuildingType, std::string& Facade, std::string& RoofSection);
}
