#include "StopLines.h"

#include <algorithm>
#include <initializer_list>

namespace WorldBuilder
{
namespace
{
/** How far from a segment's centre line the point may lie and still be placed on it, metres. */
constexpr double SnapDistance = 15.0;

/** The segment of the way nearest the point and the point's distance along it, or nothing when none is near. */
std::optional<std::pair<int, double>> NearestSegment(const FRoadLines& Lines, const FStreetWay& Way,
													 const FStreetPoint& Near)
{
	std::optional<std::pair<int, double>> Best;
	double BestDistance = SnapDistance;
	for (const FSegment& Segment : Lines.Network->Segments)
	{
		if (Segment.Way != &Way)
		{
			continue;
		}
		const auto [Along, Distance] = Polyline::ProjectOnPolyline(Segment.Xy, Near);
		if (Distance <= BestDistance)
		{
			BestDistance = Distance;
			Best = std::make_pair(Segment.Id, Along);
		}
	}
	return Best;
}

/** How far before the node at one end the segment's lane and centre lines stop; 0 where they run on to it. */
double LaneLineCut(const FSegmentLayout& Layout, bool bAtStart)
{
	double Cut = 0.0;
	for (const auto& [Key, Distance] : EndShapeOf(Layout, bAtStart).Cuts)
	{
		if (Key.Kind == ELineKind::Centre || Key.Kind == ELineKind::Lane)
		{
			Cut = std::max(Cut, Distance);
		}
	}
	return Cut;
}

/** The offset at a distance along the segment of the first of the lines the segment has, or nothing. */
std::optional<double> FirstOffsetAt(const FSegmentLayout& Layout, std::initializer_list<FLineKey> Keys, double Along)
{
	for (const FLineKey& Key : Keys)
	{
		if (Layout.Lines.Find(Key) != nullptr)
		{
			return Layout.Offsets(Key, {Along})[0];
		}
	}
	return std::nullopt;
}

/** True when some travel lane of the segment carries traffic against the given direction. */
bool HasOncomingLanes(const FSegment& Segment, int Travel)
{
	const ETravel Oncoming = Travel > 0 ? ETravel::Backward : ETravel::Forward;
	for (const FStrip& Lane : Segment.Section.TravelLanes())
	{
		if (Lane.Travel == Oncoming)
		{
			return true;
		}
	}
	return false;
}

/**
 * The offsets (left of traffic, right of traffic) in the segment's frame of the lanes of one direction: from the centre
 * line or a one-way carriageway's left edge, to the outer edge of the cycle lane beside the lanes or of the lanes.
 */
std::pair<double, double> LaneSpan(const FSegmentLayout& Layout, int Travel, double Along)
{
	const ESide Outer = Travel > 0 ? ESide::Right : ESide::Left;
	const ESide Inner = Travel > 0 ? ESide::Left : ESide::Right;
	const double Right = FirstOffsetAt(Layout,
									   {FLineKey{ELineKind::CycleOuter, Outer}, FLineKey{ELineKind::Edge, Outer},
										FLineKey{ELineKind::Kerb, Outer}},
									   Along)
							 .value_or(0.0);
	double Left = 0.0;
	if (HasOncomingLanes(*Layout.Segment, Travel))
	{
		Left = FirstOffsetAt(Layout, {FLineKey{ELineKind::Centre}}, Along).value_or(0.0);
	}
	else
	{
		Left = FirstOffsetAt(Layout, {FLineKey{ELineKind::Edge, Inner}, FLineKey{ELineKind::Kerb, Inner}}, Along)
				   .value_or(0.0);
	}
	return {Left, Right};
}
}

std::optional<FPlacedStopLine> PlaceStopLine(const FRoadLines& Lines, const FStreetWay& Way, const FStreetPoint& Near,
											 int Travel, bool bAtMouth)
{
	const std::optional<std::pair<int, double>> Found = NearestSegment(Lines, Way, Near);
	if (!Found.has_value())
	{
		return std::nullopt;
	}
	const FSegmentLayout& Layout = Lines.Layouts[Found->first];
	const FSegment& Segment = *Layout.Segment;

	// The junction ahead is at the segment's end for traffic along it, at its start for traffic against it.
	const bool bAheadAtStart = Travel < 0;
	const int64_t AheadNode = bAheadAtStart ? Segment.StartNode : Segment.EndNode;
	double FromNode = bAheadAtStart ? Found->second : Segment.Length - Found->second;
	if (Lines.Network->NodeKind(AheadNode) == ENodeKind::Junction)
	{
		const double Mouth = LaneLineCut(Layout, bAheadAtStart);
		if (bAtMouth && Mouth > 0.0)
		{
			FromNode = Mouth;
		}
		FromNode = std::max(FromNode, Mouth);
	}
	FromNode = std::min(FromNode, Segment.Length);
	const double Along = bAheadAtStart ? FromNode : Segment.Length - FromNode;

	const FStreetPoint Point = Polyline::PointAt(Segment.Xy, Along);
	const FStreetPoint Forward = Polyline::DirectionAt(Segment.Xy, Along);
	const FStreetPoint SegmentRight = Polyline::RightOf(Forward);
	const auto [Left, Right] = LaneSpan(Layout, Travel, Along);
	return FPlacedStopLine{Point, Forward * static_cast<double>(Travel), Point + SegmentRight * Left,
						   Point + SegmentRight * Right};
}
}
