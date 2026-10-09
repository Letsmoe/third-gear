#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Projection.h"

/**
 * The OSM features the world is built from, in world coordinates: the same selection as
 * Tools/osmimport/osmimport/osm.py's OsmData.
 */
namespace WorldBuilder
{
using FTags = std::vector<std::pair<std::string, std::string>>;

/** The value of a tag, or nullptr when the feature doesn't have it. */
const std::string* FindTag(const FTags& Tags, const char* Key);

struct FOsmWay
{
	int64_t Id = 0;
	FTags Tags;
	std::vector<int64_t> NodeIds;
	std::vector<FWorldPoint> Points;
};

struct FOsmPoint
{
	int64_t Id = 0;
	FTags Tags;
	FWorldPoint Position;
};

/** One polygon: the outline first, then its holes, each without a repeated end point. */
struct FOsmPolygon
{
	std::vector<std::vector<FWorldPoint>> Rings;
};

/** A closed way or a multipolygon relation, as one or more polygons. */
struct FOsmArea
{
	/** The way's or the relation's id. */
	int64_t Id = 0;
	bool bFromRelation = false;
	FTags Tags;
	std::vector<FOsmPolygon> Polygons;
};

struct FOsmData
{
	std::vector<FOsmWay> Roads;
	std::vector<FOsmWay> Footways;
	std::vector<FOsmWay> Railways;
	std::vector<FOsmWay> Waterways;
	std::vector<FOsmWay> TreeRows;
	std::vector<FOsmWay> Hedges;
	std::vector<FOsmArea> Buildings;
	std::vector<FOsmArea> Areas;
	std::vector<FOsmPoint> Points;
};

/** A longitude and latitude box in degrees; features with no node inside it are left out. */
struct FLonLatBox
{
	double West = -180.0;
	double South = -90.0;
	double East = 180.0;
	double North = 90.0;

	bool Contains(double Longitude, double Latitude) const
	{
		return West <= Longitude && Longitude <= East && South <= Latitude && Latitude <= North;
	}
};
}
