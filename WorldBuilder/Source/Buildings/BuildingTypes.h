#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "BuildingGeometry.h"
#include "OsmData.h"

/**
 * Building typology from OSM alone: class, roof, storeys and orientation for every building (building_types.py).
 * The classes are those of Tools/buildingkit/typology.md. A tag is trusted when it is there; everything else is
 * inferred from the footprint, the neighbours and the surroundings, and where the evidence does not decide, the
 * choice is a deterministic draw (hash of the OSM id) from the shares in the typology.
 */
namespace WorldBuilder
{
/** Typology classes (ids are stored in the tile, never renumber). */
inline constexpr std::array<std::string_view, 16> ClassNames = {
	"gruenderzeit_clinker", "brick_block_1920s", "postwar_plaster", "slab_block", "terraced", "semidetached",
	"detached_postwar", "villa", "modern", "commercial_groundfloor", "vierlande_farmhouse", "shed_garage",
	"industrial_hall", "public", "retail_centre", "halftimbered_town",
};

/** Roof shapes (0 and 1 are the ones the tile format had before). */
inline constexpr std::array<std::string_view, 9> RoofNames = {
	"flat", "gabled", "hipped", "half_hipped", "mansard", "gambrel", "pyramidal", "skillion", "round",
};

/** Flag bits of a classified building. */
enum EBuildingFlag : int
{
	FlagAtticHabitable = 1,
	FlagShopGroundFloor = 2,
	FlagClosedLeft = 4,
	FlagClosedRight = 8,
	FlagCorner = 16,
	FlagComplexFootprint = 32,
	FlagBarn = 64,
	FlagTower = 128,
};

/** Which values came from OSM tags, as bits (the rest is inferred). */
enum EBuildingTagBit : int
{
	TagClass = 1,
	TagLevels = 2,
	TagRoofShape = 4,
	TagHeight = 8,
	TagStartDate = 16,
	TagRoofLevels = 32,
};

/** The id of a class name, or -1. */
int ClassIdOf(std::string_view ClassName);

/** The id of a roof shape name, or -1. */
int RoofIdOf(std::string_view RoofName);

/** One usable building polygon with the tags of the OSM feature it came from. */
struct FBuildingFootprint
{
	int64_t OsmId = 0;
	const FTags* Tags = nullptr;
	FBuildingPolygon Polygon;
};

/**
 * Result of the classification of one building. The BTYP record stores the pitch rounded to degrees, the plinth in
 * 5 cm units and the rest as given here.
 */
struct FBuildingType
{
	int ClassId = 0;
	int RoofShape = 0;
	double PitchDegrees = 0.0;
	int Storeys = 0;
	int AtticLevels = 0;
	double StoreyHeight = 0.0;
	double GroundHeight = 0.0;
	double Plinth = 0.0;
	double EaveHeight = 0.0;
	double RidgeYaw = 0.0;
	double FrontYaw = 0.0;
	int Flags = 0;
	int TagBits = 0;
};

/** Classifies the buildings of one region. Build it once with all buildings and streets, then call Classify. */
class FBuildingTyper
{
public:
	/**
	 * Indexes the buildings, streets and landuse areas on ThreadCount threads (0 for all cores); the footprint list
	 * must outlive the typer.
	 */
	FBuildingTyper(const std::vector<FBuildingFootprint>& InFootprints, const std::vector<FOsmWay>& Roads,
		const std::vector<FOsmWay>& Footways, const std::vector<FOsmArea>& Areas, unsigned ThreadCount = 0);
	~FBuildingTyper();

	/** Classifies building number Index (its position in the list given to the constructor); safe to call from several threads. */
	FBuildingType Classify(size_t Index) const;

	/** The indexes and tables behind the classification; defined in BuildingTypes.cpp. */
	struct FImpl;

private:
	std::unique_ptr<FImpl> Impl;
};
}
