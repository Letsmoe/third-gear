#include "OsmReader.h"

#include <cstring>
#include <initializer_list>

#include <osmium/area/assembler.hpp>
#include <osmium/area/multipolygon_manager.hpp>
#include <osmium/handler.hpp>
#include <osmium/handler/node_locations_for_ways.hpp>
#include <osmium/index/map/flex_mem.hpp>
#include <osmium/io/pbf_input.hpp>
#include <osmium/relations/manager_util.hpp>
#include <osmium/tags/tags_filter.hpp>
#include <osmium/visitor.hpp>

namespace WorldBuilder
{
const std::string* FindTag(const FTags& Tags, const char* Key)
{
	for (const auto& [TagKey, TagValue] : Tags)
	{
		if (TagKey == Key)
		{
			return &TagValue;
		}
	}
	return nullptr;
}

namespace
{
using FLocationIndex = osmium::index::map::FlexMem<osmium::unsigned_object_id_type, osmium::Location>;

/** True when the tag's value is one of the values; an empty list accepts any value. */
bool ValueIn(const char* Value, std::initializer_list<const char*> Values)
{
	if (Value == nullptr)
	{
		return false;
	}
	if (Values.size() == 0)
	{
		return true;
	}
	for (const char* Candidate : Values)
	{
		if (std::strcmp(Value, Candidate) == 0)
		{
			return true;
		}
	}
	return false;
}

bool HasValue(const osmium::TagList& Tags, const char* Key, const char* Value)
{
	const char* Actual = Tags.get_value_by_key(Key);
	return Actual != nullptr && std::strcmp(Actual, Value) == 0;
}

/** Points the world uses: signals, signs, crossings, lamps, trees and so on (osm.py's POINT_KEYS). */
bool IsWantedPoint(const osmium::TagList& Tags)
{
	return ValueIn(Tags.get_value_by_key("highway"), {"traffic_signals", "stop", "give_way", "crossing", "street_lamp",
														  "mini_roundabout", "bus_stop"})
		|| ValueIn(Tags.get_value_by_key("natural"), {"tree"})
		|| ValueIn(Tags.get_value_by_key("railway"), {"level_crossing"})
		|| ValueIn(Tags.get_value_by_key("traffic_sign"), {});
}

/** Buildings and the land use, water and paved areas the world uses (osm.py's AREA_KEYS). */
bool IsWantedArea(const osmium::TagList& Tags)
{
	if (Tags.has_key("building"))
	{
		return true;
	}
	// Closed highway ways are loops, not areas, unless they say area=yes.
	const bool bHighwayArea = HasValue(Tags, "area", "yes")
		&& ValueIn(Tags.get_value_by_key("highway"), {"pedestrian", "footway", "service"});
	return bHighwayArea
		|| ValueIn(Tags.get_value_by_key("landuse"), {})
		|| ValueIn(Tags.get_value_by_key("natural"), {"water", "wood", "scrub", "wetland", "grassland", "heath"})
		|| ValueIn(Tags.get_value_by_key("leisure"), {"park", "pitch", "playground", "garden"})
		|| ValueIn(Tags.get_value_by_key("amenity"), {"parking"})
		|| ValueIn(Tags.get_value_by_key("area:highway"), {})
		|| ValueIn(Tags.get_value_by_key("place"), {"square"})
		|| ValueIn(Tags.get_value_by_key("waterway"), {"riverbank", "dock"})
		|| ValueIn(Tags.get_value_by_key("water"), {});
}

/** The keys an area can be wanted for, so the assembler skips every other closed way and relation. */
osmium::TagsFilter AreaKeyFilter()
{
	osmium::TagsFilter Filter(false);
	for (const char* Key : {"building", "landuse", "natural", "leisure", "amenity", "highway", "area:highway", "place",
							"waterway", "water"})
	{
		Filter.add_rule(true, osmium::TagMatcher(Key));
	}
	return Filter;
}

/** Which list of FOsmData a way belongs to, or nullptr when the world doesn't use it. */
std::vector<FOsmWay>* WayList(const osmium::TagList& Tags, FOsmData& Data)
{
	const char* Highway = Tags.get_value_by_key("highway");
	if (ValueIn(Highway, {"motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary",
						  "secondary_link", "tertiary", "tertiary_link", "unclassified", "residential",
						  "living_street", "service", "road"})
		&& !HasValue(Tags, "area", "yes"))
	{
		return &Data.Roads;
	}
	if (ValueIn(Highway, {"footway", "path", "cycleway", "pedestrian", "steps", "track", "bridleway"}))
	{
		return &Data.Footways;
	}
	if (ValueIn(Tags.get_value_by_key("railway"), {"rail", "light_rail", "subway", "tram"}))
	{
		return &Data.Railways;
	}
	if (Tags.has_key("waterway"))
	{
		return &Data.Waterways;
	}
	if (HasValue(Tags, "natural", "tree_row"))
	{
		return &Data.TreeRows;
	}
	if (HasValue(Tags, "barrier", "hedge"))
	{
		return &Data.Hedges;
	}
	return nullptr;
}

FTags CopyTags(const osmium::TagList& Tags)
{
	FTags Copy;
	Copy.reserve(Tags.size());
	for (const osmium::Tag& Tag : Tags)
	{
		Copy.emplace_back(Tag.key(), Tag.value());
	}
	return Copy;
}

/** A ring's points in world coordinates, without the repeated end point. */
std::vector<FWorldPoint> RingPoints(const osmium::NodeRefList& Ring)
{
	std::vector<FWorldPoint> Points;
	Points.reserve(Ring.size());
	for (const osmium::NodeRef& Node : Ring)
	{
		Points.push_back(LonLatToWorld(Node.location().lon(), Node.location().lat()));
	}
	if (Points.size() > 1 && Ring.front().location() == Ring.back().location())
	{
		Points.pop_back();
	}
	return Points;
}

/** Collects the wanted nodes, ways and assembled areas into FOsmData. */
class FCollectHandler : public osmium::handler::Handler
{
public:
	FCollectHandler(FOsmData& InData, const FLonLatBox& InBox) : Data(InData), Box(InBox) {}

	void node(const osmium::Node& Node)
	{
		if (Node.tags().empty() || !IsWantedPoint(Node.tags()) || !Node.location().valid())
		{
			return;
		}
		if (!Box.Contains(Node.location().lon(), Node.location().lat()))
		{
			return;
		}
		Data.Points.push_back({Node.id(), CopyTags(Node.tags()), LonLatToWorld(Node.location().lon(), Node.location().lat())});
	}

	void way(const osmium::Way& Way)
	{
		std::vector<FOsmWay>* List = WayList(Way.tags(), Data);
		if (List == nullptr || Way.nodes().size() < 2 || !AllLocationsValid(Way.nodes()) || !AnyInBox(Way.nodes()))
		{
			return;
		}
		FOsmWay& Result = List->emplace_back();
		Result.Id = Way.id();
		Result.Tags = CopyTags(Way.tags());
		Result.NodeIds.reserve(Way.nodes().size());
		Result.Points.reserve(Way.nodes().size());
		for (const osmium::NodeRef& Node : Way.nodes())
		{
			Result.NodeIds.push_back(Node.ref());
			Result.Points.push_back(LonLatToWorld(Node.location().lon(), Node.location().lat()));
		}
	}

	void area(const osmium::Area& Area)
	{
		if (!IsWantedArea(Area.tags()))
		{
			return;
		}
		FOsmArea Result;
		Result.Id = Area.orig_id();
		Result.bFromRelation = !Area.from_way();
		for (const osmium::OuterRing& Outer : Area.outer_rings())
		{
			AddPolygon(Area, Outer, Result);
		}
		if (Result.Polygons.empty())
		{
			return;
		}
		Result.Tags = CopyTags(Area.tags());
		if (Area.tags().has_key("building"))
		{
			Data.Buildings.push_back(std::move(Result));
		}
		else
		{
			Data.Areas.push_back(std::move(Result));
		}
	}

private:
	FOsmData& Data;
	FLonLatBox Box;

	static bool AllLocationsValid(const osmium::NodeRefList& Nodes)
	{
		for (const osmium::NodeRef& Node : Nodes)
		{
			if (!Node.location().valid())
			{
				return false;
			}
		}
		return true;
	}

	bool AnyInBox(const osmium::NodeRefList& Nodes) const
	{
		for (const osmium::NodeRef& Node : Nodes)
		{
			if (Box.Contains(Node.location().lon(), Node.location().lat()))
			{
				return true;
			}
		}
		return false;
	}

	/** One outer ring with its holes, when the ring has a node in the box; holes under three points are dropped. */
	void AddPolygon(const osmium::Area& Area, const osmium::OuterRing& Outer, FOsmArea& Result) const
	{
		if (!AnyInBox(Outer))
		{
			return;
		}
		FOsmPolygon& Polygon = Result.Polygons.emplace_back();
		Polygon.Rings.push_back(RingPoints(Outer));
		for (const osmium::InnerRing& Inner : Area.inner_rings(Outer))
		{
			std::vector<FWorldPoint> Hole = RingPoints(Inner);
			if (Hole.size() >= 3)
			{
				Polygon.Rings.push_back(std::move(Hole));
			}
		}
	}
};
}

FOsmData ReadOsm(const std::string& PbfPath, const FLonLatBox& Box)
{
	const osmium::io::File File(PbfPath);
	osmium::area::Assembler::config_type AssemblerConfig;
	osmium::area::MultipolygonManager<osmium::area::Assembler> AreaManager(AssemblerConfig, AreaKeyFilter());
	// First pass: the multipolygon relations, so the second knows which ways and nodes they need.
	osmium::relations::read_relations(File, AreaManager);

	FOsmData Data;
	FCollectHandler Collect(Data, Box);
	FLocationIndex LocationIndex;
	osmium::handler::NodeLocationsForWays<FLocationIndex> Locations(LocationIndex);
	Locations.ignore_errors();
	osmium::io::Reader Reader(File, osmium::io::read_meta::no);
	osmium::apply(Reader, Locations, Collect, AreaManager.handler([&Collect](osmium::memory::Buffer&& Buffer) {
		osmium::apply(Buffer, Collect);
	}));
	Reader.close();
	return Data;
}
}
