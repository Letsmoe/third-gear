#include "SmoothWays.h"

#include <algorithm>

#include "Centreline.h"
#include "OsmTags.h"
#include "RoadChains.h"
#include "RoadDesign.h"
#include "WayEdits.h"

namespace WorldBuilder
{
namespace
{
/** Distance between the samples the centreline is fitted to, metres. */
constexpr double SampleSpacing = 1.0;
/** Distance between the shape points added along a redrawn way, metres. */
constexpr double PointSpacing = 2.0;
/** The first id of the shape points added by the smoothing, below the range the dual carriageways count down from. */
constexpr int64_t FirstSyntheticId = -2000000000000000;

/** The part of the chain's fitted line along one piece, in the segment's own node order. */
FStreetPolyline PieceOfFit(const FStreetPolyline& Fitted, const std::vector<int>& NodeSamples, size_t PieceIndex,
						   bool bReversed)
{
	FStreetPolyline Line(Fitted.begin() + NodeSamples[PieceIndex], Fitted.begin() + NodeSamples[PieceIndex + 1] + 1);
	if (bReversed)
	{
		return Polyline::Reversed(Line);
	}
	return Line;
}

/**
 * Moves the nodes of one segment onto its redrawn line, each to the same share of the line's length it had of the
 * segment's, and adds shape points between them. Junction nodes stay, as other chains are pinned to them.
 */
void PlaceSegment(const FSegmentNetwork& Network, const FSegment& Segment, const FStreetPolyline& NewLine,
				  FWayEdits& Edits)
{
	const std::vector<double> OldAlong = Polyline::Arclength(Segment.Xy);
	const std::vector<double> NewAlong = Polyline::Arclength(NewLine);
	const double Scale = NewAlong.back() / std::max(OldAlong.back(), 1e-6);
	const std::vector<int64_t>& Nodes = Segment.Way->NodeIds;
	for (size_t Index = 0; Index < Segment.Xy.size(); ++Index)
	{
		const int64_t Node = Nodes[Segment.FirstIndex + Index];
		if (Network.NodeKind(Node) != ENodeKind::Junction)
		{
			Edits.MovedNodes[Node] = Polyline::PointAt(NewLine, NewAlong, OldAlong[Index] * Scale);
		}
		if (Index > 0)
		{
			Edits.AddPoints(Nodes[Segment.FirstIndex + Index - 1], Node, NewLine, OldAlong[Index - 1] * Scale,
							OldAlong[Index] * Scale, PointSpacing);
		}
	}
}

/** Fits the chain's centreline and redraws its segments along it. */
void SmoothChain(const FSegmentNetwork& Network, const FRoadChain& Chain, FWayEdits& Edits)
{
	const FRoadDesign Design = RoadDesignFor(OsmTags::ValueOrEmpty(LongestSegment(Network, Chain).Way->Tags, "highway"));
	const FChainSamples Sampled = SampleChain(Network, Chain, SampleSpacing);
	const FCentrelineFit Fit = FitCentreline(Sampled.Samples, Sampled.Original,
											 PinnedSamples(Network, Chain, Sampled.NodeSamples), Design.PlanSmoothing,
											 Design.MaxDeviation);
	for (size_t Index = 0; Index < Chain.Pieces.size(); ++Index)
	{
		const FChainPiece& Piece = Chain.Pieces[Index];
		PlaceSegment(Network, Network.Segments[Piece.Segment],
					 PieceOfFit(Fit.Points, Sampled.NodeSamples, Index, Piece.bReversed), Edits);
	}
}
}

std::vector<FStreetWay> SmoothWays(const std::vector<FStreetWay>& Ways, const FSectionsByWay& Sections)
{
	const FSegmentNetwork Network(Ways, Sections);
	FWayEdits Edits;
	Edits.NextSyntheticId = FirstSyntheticId;
	for (const FRoadChain& Chain : BuildRoadChains(Network))
	{
		SmoothChain(Network, Chain, Edits);
	}
	return WithEdits(Ways, Edits);
}
}
