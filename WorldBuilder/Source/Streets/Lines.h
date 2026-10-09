#pragma once

#include <string>
#include <vector>

#include "CrossSection.h"
#include "OrderedMap.h"
#include "OsmData.h"
#include "Polyline.h"

/**
 * The lines along a road's cross-section, named so they can be followed from one road into the next (Python:
 * streets/lines.py).
 *
 * A line is a kerb, the edge of the travel lanes, the centre line between the two directions, or the k-th line between
 * lanes of one direction counted from that direction's kerb. Offsets are from the way's centre line, positive to the
 * right of the way's node order.
 *
 * Seen from a node, every road leaving it is an arm; turning a road's lines to face out of the node mirrors them
 * (offsets change sign, left and right and the two directions swap). Two arms meeting at a node continue each other's
 * lines: an arm's line continues as the mirror of it on the other arm, at the mirrored offset.
 */
namespace WorldBuilder
{
enum class ELineKind
{
	Kerb,
	Edge,        // outer edge of the travel lanes, where an edge line is painted
	Centre,      // between the two directions
	Lane,        // between two lanes of one direction
	Cycle,       // between a cycle lane and the lane beside it
	CycleOuter,  // the kerb side of a cycle lane: unpainted along the road, the furt's second line
};

enum class ESide
{
	None,
	Left,
	Right,
};

struct FLineKey
{
	ELineKind Kind = ELineKind::Kerb;
	/** Kerbs and edges. */
	ESide Side = ESide::None;
	/** Lane lines: the direction of the lanes on both sides of the line. */
	ETravel Travel = ETravel::None;
	/** Lane lines: 1 for the line nearest that direction's kerb. */
	int Index = 0;

	bool operator==(const FLineKey& Other) const
	{
		return Kind == Other.Kind && Side == Other.Side && Travel == Other.Travel && Index == Other.Index;
	}

	bool operator<(const FLineKey& Other) const
	{
		if (Kind != Other.Kind)
		{
			return Kind < Other.Kind;
		}
		if (Side != Other.Side)
		{
			return Side < Other.Side;
		}
		if (Travel != Other.Travel)
		{
			return Travel < Other.Travel;
		}
		return Index < Other.Index;
	}
};

/** Line offsets from the way's centre line, in the order the lines were added. */
using FLineOffsets = TOrderedMap<FLineKey, double>;
/** The marking kind of each painted line ("edge", "dash_urban", "cycle_exclusive"...), in the order they were added. */
using FPaintedLines = TOrderedMap<FLineKey, std::string>;

/** One painted line: its marking kind ("edge", "solid", "turn_lane_dash", "hatch"...) and its points. */
struct FMarking
{
	std::string Kind;
	std::vector<FStreetPoint> Points;
};

/** The same side seen facing the other way. */
ESide MirroredSide(ESide Side);

/** The same direction seen facing the other way. */
ETravel MirroredTravel(ETravel Travel);

/** The same line seen facing the other way. */
FLineKey Mirrored(const FLineKey& Key);

/** The offsets of lines seen facing the other way. */
FLineOffsets MirroredLines(const FLineOffsets& Lines);

/** The offset of every line of a cross-section. */
FLineOffsets SectionLines(const FCrossSection& Section);

/**
 * The lines painted on a road: lane and centre lines on marked roads with two or more lanes wide enough to mark
 * (two-way roads from CentreLineMinWidth, never in a Tempo 30 zone), edge lines on major roads and on wide rural ones.
 */
FPaintedLines PaintedLines(const FTags& Tags, const FCrossSection& Section, bool bRural);

/**
 * The lines of painted cycle lanes, on every road that has them: a broad solid line beside an exclusive cycle lane, a
 * dashed one beside an advisory lane. They replace the edge line on their side.
 */
FPaintedLines PaintedCycleLines(const FTags& Tags, const FCrossSection& Section);

/**
 * How long a road takes to move its lines sideways by Shift metres (a lane appearing or ending, the road widening):
 * speed x shift / 3, at least TaperMinLength, inside towns at most UrbanTaperMaxLength.
 */
double TaperLength(double Shift, double SpeedKmh, bool bUrban);
}
