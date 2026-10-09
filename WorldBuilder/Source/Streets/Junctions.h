#pragma once

#include <unordered_map>
#include <vector>

#include "Layout.h"
#include "Lines.h"
#include "Network.h"

/**
 * Junctions: where each arm's painted lines stop, which arms are one road going straight through, and the guide lines
 * inside them (Python: streets/junctions.py).
 *
 * - An arm's mouth is where its carriageway leaves the other arms' carriageways, plus the corner radius: the lane and
 *   centre lines stop there instead of running into the crossing road, which keeps the junction clear.
 * - Lane and centre lines don't run through a junction: where the lines of two crossing roads would meet inside it, a
 *   small cross marks the spot. The road going straight through continues its edge lines as broken broad lines across
 *   the mouths of side roads. Driveways and other minor arms don't interrupt its lines at all.
 * - At signalised junctions, approaches with their own left-turn lanes get guide lines around the corner, along the
 *   right-hand edge of each left-turn lane's path into the road it turns into.
 */
namespace WorldBuilder
{
// The deflection range, in degrees, of the arm a left turn goes into.
inline constexpr double LeftTurnAngleLow = 45.0;
inline constexpr double LeftTurnAngleHigh = 135.0;
// Lines meeting at a shallower angle than this get no cross.
inline constexpr double CrossMinAngle = 30.0;

/** Mouths and guide lines of every junction of a segment network. */
class FJunctionLines
{
public:
	/** SignalPoints: where the traffic signals are, which decide where left turns get guide lines. */
	FJunctionLines(const FSegmentNetwork& InNetwork, const std::vector<FStreetPoint>& SignalPoints);

	/** Where the arm's painted lines stop, for every arm of every junction. */
	FMouths Mouths() const;

	/** The lines inside the junctions, from the segment layouts. */
	std::vector<FMarking> GuideLines(const std::vector<FSegmentLayout>& Layouts, const FMouths& Mouths) const;

	/** True for driveways and similar arms, which don't interrupt the lines of the road they join. */
	bool IsMinor(const FSegmentEnd& End) const;

private:
	const FSegmentNetwork& Network;
	std::vector<int64_t> Junctions;
	std::vector<FStreetPoint> Signals;
	/** Signal points by 40 m cell, for the distance test of every junction. */
	std::unordered_map<int64_t, std::vector<FStreetPoint>> SignalCells;

	/** The other arms whose carriageway this arm's lines must stop short of. */
	std::vector<FSegmentEnd> ArmsThatInterrupt(const FSegmentEnd& End, const std::vector<FSegmentEnd>& Ends) const;

	/**
	 * The distances for the edge and cycle lines of a through arm on the side where another road joins or leaves at a
	 * shallow angle: they stay broken until that road's carriageway is MergeGap away, not just clear.
	 */
	void AddMergeMouths(const FSegmentEnd& End, const std::vector<FSegmentEnd>& Ends,
						const std::pair<FSegmentEnd, FSegmentEnd>& ThroughPair, double Mouth, FMouths& Result) const;

	/** Pairs of non-minor arms that are one road going straight through, the most important road first. */
	std::vector<std::pair<FSegmentEnd, FSegmentEnd>> ThroughPairs(const std::vector<FSegmentEnd>& Ends) const;

	/** The painted lines of the entering arm continued to the matching lines of the leaving arm. */
	std::vector<FMarking> ThroughGuides(const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& Entering,
										const FSegmentEnd& Leaving, const std::vector<FSegmentEnd>& Ends,
										const FMouths& Mouths) const;

	/**
	 * Crosses where the lane and centre lines of two crossing roads would meet inside the junction (German junctions
	 * don't carry the lines through). Each road's lines run straight across the junction: from mouth to mouth for a
	 * road going through, from the mouth into the junction for one that ends there.
	 */
	std::vector<FMarking> Crosses(const std::vector<FSegmentLayout>& Layouts, const std::vector<FSegmentEnd>& Ends,
								  const std::vector<std::pair<FSegmentEnd, FSegmentEnd>>& Pairs,
								  const FMouths& Mouths) const;

	/** (start, end) of the straight lines joining the painted lane and centre lines of a through road's arms. */
	std::vector<std::pair<FStreetPoint, FStreetPoint>> ThroughChords(const std::vector<FSegmentLayout>& Layouts,
																	 const FSegmentEnd& Entering,
																	 const FSegmentEnd& Leaving,
																	 const FMouths& Mouths) const;

	/** (start, end) of the painted lane and centre lines of an arm ending at the junction, run on straight across it. */
	std::vector<std::pair<FStreetPoint, FStreetPoint>> EntryChords(const std::vector<FSegmentLayout>& Layouts,
																   const FSegmentEnd& End, const FMouths& Mouths) const;

	/**
	 * How a line continues across a junction: an edge line as a broken broad line and a cycle lane as a cycle crossing
	 * (furt) where a side road joins on its side, unchanged on a side no other road joins.
	 */
	std::string GuideKind(const FLineKey& Key, const std::string& Kind, const FSegmentEnd& Entering,
						  const FSegmentEnd& Leaving, const std::vector<FSegmentEnd>& SideArms) const;

	/** Guide lines along the right-hand edge of the paths of the entering arm's exclusive left-turn lanes. */
	std::vector<FMarking> LeftTurnGuides(const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& Entering,
										 const std::vector<FSegmentEnd>& Ends, const FMouths& Mouths) const;

	/** The arm a left turn from the entering arm goes into: the one nearest a right angle to the left. */
	std::optional<FSegmentEnd> LeftTurnTarget(const FSegmentEnd& Entering, const std::vector<FSegmentEnd>& Ends) const;

	/** The segment an end belongs to. */
	const FSegment& SegmentOf(const FSegmentEnd& End) const;

	/** The importance of the arm's road class (Assumptions::RoadClassRank). */
	int Rank(const FSegmentEnd& End) const;

	/** True when a traffic signal lies within SignalJunctionRadius of the junction node. */
	bool IsSignalised(int64_t Node) const;

	/** The point of a line (offset facing out of the node) at a distance out along the arm. */
	FStreetPoint MouthPoint(const FSegmentEnd& End, double Offset, double Distance) const;

	/** The direction out of the node at a distance out along the arm. */
	FStreetPoint DirectionAtMouth(const FSegmentEnd& End, double Distance) const;
};
}
