#pragma once

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "Lines.h"
#include "Network.h"
#include "Splits.h"

/**
 * Where each segment's lines run: their offsets along the segment, the tapers between roads of different
 * cross-sections, and where lines start and stop (Python: streets/layout.py).
 *
 * At a continuation node the wider of the two roads moves its lines over a taper, so that at the node they meet the
 * narrower road's lines; lines that only one of the two roads has (a lane that appears or ends) start or stop where the
 * taper does, never at the node. At a junction the painted lines stop at the junction's mouth (junctions.py); the kerbs
 * run on, since the junction's surface is built from them.
 */
namespace WorldBuilder
{
/** How a segment's lines behave at one of its ends, in the segment's own frame. */
struct FEndShape
{
	double Taper = 0.0;
	/** The offset of each line at the node, reached over the taper. */
	FLineOffsets NodeOffsets;
	/** The distance from the node where each line begins. */
	FLineOffsets Cuts;
};

/**
 * Where the painted lines of an arm stop: the arm's own distance from the junction node, and the distance of single
 * lines (key facing out of the node) that stop elsewhere.
 */
struct FMouths
{
	std::map<FSegmentEnd, double> ByEnd;
	std::map<std::pair<FSegmentEnd, FLineKey>, double> ByLine;

	/** The arm's mouth distance, or 0 when it has none. */
	double OfEnd(const FSegmentEnd& End) const;

	/** Where a line (key facing out of the node) of an arm stops: its own distance where it has one, else the arm's. */
	double OfLine(const FSegmentEnd& End, const FLineKey& Key) const;
};

struct FSegmentLayout
{
	/** Points into the network the layout was built from, which must outlive it. */
	const FSegment* Segment = nullptr;
	/** Offset of each line along the middle of the segment. */
	FLineOffsets Lines;
	/** Marking kind of each painted line. */
	FPaintedLines Painted;
	FEndShape Start;
	FEndShape End;

	/** The line's offset at the distances along the segment's centre line. */
	std::vector<double> Offsets(const FLineKey& Key, const std::vector<double>& Along) const;

	/** (start, end) distance along the segment between which the line runs. */
	std::pair<double, double> LineRange(const FLineKey& Key) const;

	/** The line as a polyline between its cuts, or nothing when nothing is left of it. */
	std::optional<FStreetPolyline> LineXy(const FLineKey& Key) const;

	/**
	 * The part of a line between two distances along the segment, moved sideways by Shift (to the right), or nothing
	 * when it is too short.
	 */
	std::optional<FStreetPolyline> LineBetween(const FLineKey& Key, double StartS, double EndS, double Shift = 0.0) const;

	/** The kerb to kerb width at one end of the segment. */
	double WidthAtEnd(bool bAtStart) const;
};

/**
 * The layout of every segment. Painted: the painted lines of each segment by segment id; Urban: whether each
 * segment lies in town, by segment id; Mouths: where the lines stop at junctions.
 */
std::vector<FSegmentLayout> BuildLayouts(const FSegmentNetwork& Network, const std::vector<FPaintedLines>& Painted,
										 const std::vector<bool>& Urban, const FMouths& Mouths);

/** The shape at one end of a layout. */
FEndShape& EndShapeOf(FSegmentLayout& Layout, bool bAtStart);
const FEndShape& EndShapeOf(const FSegmentLayout& Layout, bool bAtStart);

/** A segment's lines (offsets) facing out of the node at this end. */
FLineOffsets ArmLines(const FLineOffsets& Lines, const FSegmentEnd& End);

/** A segment's painted lines with the keys facing out of the node at this end (values unchanged). */
FPaintedLines ArmKeys(const FPaintedLines& Values, const FSegmentEnd& End);

/** A line given facing out of the node at this end, in the segment's own frame: its key and offset. */
std::pair<FLineKey, double> ToSegmentFrame(const FLineKey& Key, double Offset, const FSegmentEnd& End);
}
