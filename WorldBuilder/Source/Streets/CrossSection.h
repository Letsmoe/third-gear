#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "OsmData.h"

/**
 * A road's cross-section: the strips of its carriageway from the left kerb to the right one, and so its width
 * (Python: streets/cross_section.py).
 *
 * The strips are, from left to right along the way's node order: a margin (gutter or shoulder strip), the left side's
 * bus and cycle lanes, the travel lanes, the right side's cycle and bus lanes, and a margin. Travel lanes on the left
 * half of a two-way road carry traffic against the node order (right-hand traffic).
 *
 * Sources in order of trust: a width= tag where mappers measure kerb to kerb, then the lane tags (lanes, turn:lanes,
 * lanes:forward, cycleway, busway, lane_markings), then the assumptions for the road class and its surroundings.
 */
namespace WorldBuilder
{
enum class EStripKind
{
	TravelLane,
	CycleLane,
	BusLane,
	Margin,
};

/** Which way traffic moves on a strip, relative to the way's node order. */
enum class ETravel
{
	Forward,
	Backward,
	Both,  // a shared lane on a narrow two-way road
	None,  // margins, and the lines that belong to no direction
};

enum class EWidthSource
{
	WidthTag,
	Assumed,
};

/** What the surroundings say about a road, found by the geometry code: urban when buildings line it. */
struct FRoadContext
{
	bool bUrban = false;
};

struct FStrip
{
	EStripKind Kind = EStripKind::Margin;
	double Width = 0.0;
	ETravel Travel = ETravel::None;
};

/** One strip with its edges as offsets from the way's centre line (positive to the right). */
struct FStripEdges
{
	const FStrip* Strip = nullptr;
	double Left = 0.0;
	double Right = 0.0;
};

struct FCrossSection
{
	/** From the left kerb to the right one. */
	std::vector<FStrip> Strips;
	/** Lane lines are painted between the travel lanes. */
	bool bMarked = false;
	EWidthSource WidthSource = EWidthSource::Assumed;

	/** The carriageway width from kerb to kerb. */
	double Width() const;

	/** The travel lane strips, from left to right. */
	std::vector<FStrip> TravelLanes() const;

	/** The number of travel lanes, both directions together (an unmarked two-way road has two). */
	int LaneCount() const;

	/** The same cross-section seen along the other direction: strips in reverse order, directions swapped. */
	FCrossSection Mirrored() const;

	/**
	 * Every strip with its left and right edge as offsets from the way's centre line, which runs down the middle of
	 * the carriageway; positive is to the right. The pointers stay valid as long as this cross-section does.
	 */
	std::vector<FStripEdges> StripEdges() const;

	/** Offsets of the lines between neighbouring travel lanes, the centre line of a two-way road among them. */
	std::vector<double> LaneDividers() const;

	/**
	 * Distance from the centre line to the middle of the strip of a kind (cycle or bus lane) on one side (left or
	 * right), or nothing when that side has none.
	 */
	std::optional<double> SideStripCentre(EStripKind Kind, bool bRightSide) const;

	/**
	 * Distance from the centre line to the middle of the travel lane nearest the kerb on the right of traffic going
	 * one way (Forward or Backward); 0 when no lane carries that way on its own (a shared lane).
	 */
	double KerbLaneCentre(ETravel Travel) const;
};

/** The cross-section of a road from its tags and surroundings. */
FCrossSection MakeCrossSection(const FTags& Tags, const FRoadContext& Context);

/**
 * The number of travel lanes for motor traffic from the tags, capped at what the road class plausibly has, or the
 * default.
 */
int LaneCount(const FTags& Tags);
}
