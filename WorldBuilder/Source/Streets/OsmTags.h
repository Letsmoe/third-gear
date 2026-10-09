#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CrossSection.h"
#include "OsmData.h"

/**
 * Reading the OSM tags the street model uses: plain functions over a way's tags (Python: streets/tags.py).
 *
 * Sides are relative to the way's node order: left and right as seen when walking from its first node to its last.
 */
namespace WorldBuilder::OsmTags
{
/** The value of a tag, or an empty string when the way doesn't have it (Python's tags.get(key, "")). */
std::string ValueOrEmpty(const FTags& Tags, const char* Key);

/** True when the way has the tag, whatever its value. */
bool HasTag(const FTags& Tags, const char* Key);

/** True when the way has the tag with exactly this value. */
bool TagEquals(const FTags& Tags, const char* Key, const char* Value);

/** The text split at every separator, keeping empty parts (Python's str.split(separator)). */
std::vector<std::string> SplitText(const std::string& Text, char Separator);

/** The number at the start of a tag value ("5.5", "5,5 m", "4;6"), or nothing when there is none. */
std::optional<double> Number(const std::string* Value);

/** The number at the start of a tag value, or the default when there is none. */
double Number(const std::string* Value, double Default);

/** The road class, such as "secondary" or "secondary_link". */
std::string Highway(const FTags& Tags);

/** The road class without the _link of slip roads: "secondary" for both secondary and secondary_link. */
std::string BaseClass(const FTags& Tags);

/** True for slip roads and junction connectors (highway=*_link). */
bool IsLink(const FTags& Tags);

/** True when traffic only goes one way: tagged one-way, roundabouts and motorways. */
bool IsOneway(const FTags& Tags);

/** True for oneway=-1: traffic goes against the node order. */
bool IsReversedOneway(const FTags& Tags);

/**
 * How many lanes the turn:lanes tags list (both directions on a two-way road), or nothing when they are incomplete.
 * turn:lanes lists every lane, so it is rarely wrong where lanes= is: it wins when both are there.
 */
std::optional<int> TurnLaneCount(const FTags& Tags);

/** The number of travel lanes from the tags (turn:lanes, then lanes), or nothing when neither says. */
std::optional<int> TaggedLaneCount(const FTags& Tags);

/**
 * How many of the lanes= lanes are cycle or bus lanes. Some mappers count a painted cycle or bus lane in lanes= and
 * say which one it is in the per-lane lists (vehicle:lanes=yes|yes|no, bicycle:lanes=no|no|designated); those lanes
 * are already in the cycleway= and busway= tags and must not count as travel lanes.
 */
int NonMotorLanesCounted(const FTags& Tags);

/** (forward, backward) lane counts from lanes:forward and lanes:backward, or nothing when not both are tagged. */
std::optional<std::pair<int, int>> TaggedLanesByDirection(const FTags& Tags);

/** True for a road only buses may use: bus=yes or psv=yes, with general traffic (vehicle, motor_vehicle) banned. */
bool IsBusRoad(const FTags& Tags);

/** True inside a Tempo 30 zone (Zeichen 274.1), however the zone is tagged. */
bool IsZone30(const FTags& Tags);

/** True or false when lane_markings says whether lanes are painted, nothing when it is not tagged. */
std::optional<bool> LaneMarkings(const FTags& Tags);

/** The speed limit in km/h, or nothing when maxspeed is missing or unreadable ("none" on motorways is nothing too). */
std::optional<double> SpeedLimit(const FTags& Tags);

/**
 * What a sided tag such as cycleway or busway says per side; a side without a value is missing. key= alone means
 * both sides, except on a one-way street, where it means the right side (the side traffic keeps to).
 */
struct FSideValues
{
	std::optional<std::string> Left;
	std::optional<std::string> Right;

	const std::optional<std::string>& OfSide(bool bRightSide) const
	{
		return bRightSide ? Right : Left;
	}
};
FSideValues SideValues(const FTags& Tags, const std::string& Key);

/** The cycleway tagged on a road per side (lane, track, separate, no...). */
FSideValues CyclewaySides(const FTags& Tags);

/** True for a cycle lane value (cycleway=lane or opposite_lane). */
bool IsCycleLaneValue(const std::string& Value);

/** True for an advisory cycle lane (Schutzstreifen, dashed line) rather than an exclusive one (solid line). */
bool CycleLaneIsAdvisory(const FTags& Tags, bool bRightSide);

/** True when the side (false left, true right) has a bus lane on the carriageway. */
bool HasBusLane(const FTags& Tags, bool bRightSide);

/**
 * The turn:lanes values of the lanes going one way along the way (Forward or Backward), from the driver's left;
 * empty when not tagged.
 */
std::vector<std::string> TurnLanes(const FTags& Tags, ETravel Travel);
}
