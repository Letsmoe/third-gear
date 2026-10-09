#include "DualCarriageways.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "Assumptions.h"
#include "Corrections.h"
#include "Lines.h"
#include "OsmTags.h"
#include "Splits.h"

namespace WorldBuilder::DualCarriageways
{
namespace
{
/** One segment of a carriageway chain and whether the chain runs along the segment's node order. */
struct FChainLink
{
	const FSegment* Segment = nullptr;
	bool bAlong = false;
};

using FChain = std::vector<FChainLink>;

/** A shape point added to a way: a made-up node id and where it lies. */
struct FAddedPoint
{
	int64_t NodeId = 0;
	FStreetPoint Point;
};

/** What the redrawing changes: nodes moved to new places, and shape points added between two nodes. */
struct FRedrawChanges
{
	std::unordered_map<int64_t, FStreetPoint> MovedNodes;
	std::map<std::pair<int64_t, int64_t>, std::vector<FAddedPoint>> AddedPoints;
	int64_t NextSyntheticId = -1000000000000000;
};

/** The one-way segments straight on from a segment end, and the node each of them ends at. */
std::pair<FChain, std::vector<int64_t>> Walk(const FSegmentNetwork& Network, const FSegmentEnd& Start)
{
	FChain Chain;
	std::vector<int64_t> Nodes;
	std::optional<FSegmentEnd> End = Start;
	double Travelled = 0.0;
	while (End.has_value() && Travelled < MaxSectionLength)
	{
		const FSegment& Segment = Network.Segments[End->Segment];
		const bool bSeenBefore = std::any_of(Chain.begin(), Chain.end(), [&Segment](const FChainLink& Link) {
			return Link.Segment == &Segment;
		});
		if (!OsmTags::IsOneway(Segment.Way->Tags) || bSeenBefore)
		{
			break;
		}
		Chain.push_back({&Segment, End->bAtStart});
		const FSegmentEnd FarEnd = {Segment.Id, !End->bAtStart};
		Nodes.push_back(Network.NodeOf(FarEnd));
		Travelled += Segment.Length;
		if (Network.NodeKind(Nodes.back()) == ENodeKind::Continuation)
		{
			End = Network.OtherEnd(FarEnd);
		}
		else
		{
			End = StraightNeighbour(Network, FarEnd);
		}
	}
	return {Chain, Nodes};
}

/**
 * The two carriageways from the split to the node where they meet again, each a chain of segments, or nothing when
 * they don't meet within MaxSectionLength.
 */
std::optional<std::pair<FChain, FChain>> FindSection(const FSegmentNetwork& Network, const FSegmentEnd& First,
													 const FSegmentEnd& Second)
{
	auto [FirstChain, FirstNodes] = Walk(Network, First);
	auto [SecondChain, SecondNodes] = Walk(Network, Second);
	for (size_t FirstIndex = 0; FirstIndex < FirstNodes.size(); ++FirstIndex)
	{
		const auto Found = std::find(SecondNodes.begin(), SecondNodes.end(), FirstNodes[FirstIndex]);
		if (Found == SecondNodes.end())
		{
			continue;
		}
		FirstChain.resize(FirstIndex + 1);
		SecondChain.resize(static_cast<size_t>(Found - SecondNodes.begin()) + 1);
		return std::make_pair(FirstChain, SecondChain);
	}
	return std::nullopt;
}

/** The chain's centre line from the split onward. */
FStreetPolyline ChainLine(const FChain& Chain)
{
	FStreetPolyline Points;
	for (const FChainLink& Link : Chain)
	{
		const FStreetPolyline Xy = Link.bAlong ? Link.Segment->Xy : Polyline::Reversed(Link.Segment->Xy);
		Points.insert(Points.end(), Points.empty() ? Xy.begin() : Xy.begin() + 1, Xy.end());
	}
	return Points;
}

/** The way's node ids of a segment's points. */
std::vector<int64_t> SegmentNodeIds(const FSegment& Segment)
{
	const std::vector<int64_t>& Ids = Segment.Way->NodeIds;
	const auto Start = std::find(Ids.begin(), Ids.end(), Segment.StartNode);
	return std::vector<int64_t>(Start, Start + Segment.Xy.size());
}

/** The node ids along the chain from the split, matching ChainLine's points. */
std::vector<int64_t> ChainNodes(const FChain& Chain)
{
	std::vector<int64_t> Nodes;
	for (const FChainLink& Link : Chain)
	{
		std::vector<int64_t> Ids = SegmentNodeIds(*Link.Segment);
		if (!Link.bAlong)
		{
			std::reverse(Ids.begin(), Ids.end());
		}
		Nodes.insert(Nodes.end(), Nodes.empty() ? Ids.begin() : Ids.begin() + 1, Ids.end());
	}
	return Nodes;
}

/** The segment end where a chain stops, facing the node it ends at. */
FSegmentEnd LastEnd(const FChain& Chain)
{
	const FChainLink& Last = Chain.back();
	return {Last.Segment->Id, !Last.bAlong};
}

/** numpy.linspace(0, 1, Count). */
std::vector<double> Fractions(int Count)
{
	std::vector<double> Result(Count);
	const double Step = 1.0 / (Count - 1);
	for (int Index = 0; Index < Count; ++Index)
	{
		Result[Index] = Index == Count - 1 ? 1.0 : Index * Step;
	}
	return Result;
}

/** Points at fractions of a polyline's length. */
FStreetPolyline AtFractions(const FStreetPolyline& Line, const std::vector<double>& Fractions)
{
	const std::vector<double> Along = Polyline::Arclength(Line);
	FStreetPolyline Points;
	Points.reserve(Fractions.size());
	for (double Fraction : Fractions)
	{
		Points.push_back(Polyline::PointAt(Line, Along, Fraction * Along.back()));
	}
	return Points;
}

/** The median of the values (numpy.median). */
double Median(std::vector<double> Values)
{
	std::sort(Values.begin(), Values.end());
	const size_t Middle = Values.size() / 2;
	if (Values.size() % 2 == 1)
	{
		return Values[Middle];
	}
	return (Values[Middle - 1] + Values[Middle]) / 2;
}

/**
 * The common axis: straight between the end nodes when the middle line hardly bows, else the middle line smoothed with
 * its ends kept.
 */
FStreetPolyline Axis(const FStreetPolyline& MiddlePoints)
{
	const FStreetPoint Start = MiddlePoints.front();
	const FStreetPoint End = MiddlePoints.back();
	double Bow = 0.0;
	for (const FStreetPoint& Point : MiddlePoints)
	{
		Bow = std::max(Bow, Polyline::DistanceToSegment(Start, End, Point));
	}
	if (Bow <= MaxStraightBow)
	{
		const std::vector<double> Along = Fractions(static_cast<int>(MiddlePoints.size()));
		FStreetPolyline Chord;
		for (double Fraction : Along)
		{
			Chord.push_back((End - Start) * Fraction + Start);
		}
		return Chord;
	}
	FStreetPolyline Smoothed = MiddlePoints;
	for (int Pass = 0; Pass < AxisSmoothingPasses; ++Pass)
	{
		const FStreetPolyline Before = Smoothed;
		for (size_t Index = 1; Index + 1 < Smoothed.size(); ++Index)
		{
			Smoothed[Index] = Before[Index - 1] * 0.25 + Before[Index] * 0.5 + Before[Index + 1] * 0.25;
		}
	}
	return Smoothed;
}

/** How long the carriageways take to open from the split to their full distance (a taper of the road's speed). */
double GoreLength(const FSegment& Segment, double Separation, double Length)
{
	double Speed = Assumptions::DefaultSpeedUrban;
	const std::optional<double> Limit = OsmTags::SpeedLimit(Segment.Way->Tags);
	if (Limit.has_value() && *Limit != 0.0)
	{
		Speed = *Limit;
	}
	const double Taper = TaperLength(Separation / 2, Speed, true);
	return std::min(Taper, Length * Assumptions::TaperMaxSegmentShare);
}

/** A smooth ramp from 0 to 1 over RampLength. */
double Ramp(double Distance, double RampLength)
{
	const double Fraction = std::min(std::max(Distance / std::max(RampLength, 1e-6), 0.0), 1.0);
	return Fraction * Fraction * (3.0 - 2.0 * Fraction);
}

/** Moves the chain's nodes to the same share of the redrawn line and adds shape points between them. */
void PlaceNodes(const FChain& Chain, const FStreetPolyline& NewLine, FRedrawChanges& Changes)
{
	const FStreetPolyline OldLine = ChainLine(Chain);
	const std::vector<int64_t> Nodes = ChainNodes(Chain);
	const std::vector<double> OldAlong = Polyline::Arclength(OldLine);
	const std::vector<double> NewAlong = Polyline::Arclength(NewLine);
	std::vector<double> Targets;
	for (double Along : OldAlong)
	{
		Targets.push_back(Along / std::max(OldAlong.back(), 1e-6) * NewAlong.back());
	}
	for (size_t Index = 1; Index + 1 < Nodes.size(); ++Index)
	{
		Changes.MovedNodes[Nodes[Index]] = Polyline::PointAt(NewLine, NewAlong, Targets[Index]);
	}
	for (size_t Index = 0; Index + 1 < Nodes.size(); ++Index)
	{
		const double Start = Targets[Index];
		const double End = Targets[Index + 1];
		const int Count = static_cast<int>((End - Start) / PointSpacing);
		std::vector<FAddedPoint> Points;
		for (int Step = 1; Step <= Count; ++Step)
		{
			Points.push_back({Changes.NextSyntheticId--,
							  Polyline::PointAt(NewLine, NewAlong, Start + (End - Start) * Step / (Count + 1))});
		}
		Changes.AddedPoints[{Nodes[Index], Nodes[Index + 1]}] = Points;
		Changes.AddedPoints[{Nodes[Index + 1], Nodes[Index]}] = std::vector<FAddedPoint>(Points.rbegin(), Points.rend());
	}
}

/** Moves the nodes of both carriageways of a section onto lines parallel to their common axis. */
void Redraw(const FSegmentNetwork& Network, const FChain& FirstChain, const FChain& SecondChain,
			FRedrawChanges& Changes)
{
	const FStreetPolyline First = ChainLine(FirstChain);
	const FStreetPolyline Second = ChainLine(SecondChain);
	const double FirstLength = Polyline::Length(First);
	const double SecondLength = Polyline::Length(Second);
	if (std::abs(FirstLength - SecondLength) > MaxLengthDifference * std::max(FirstLength, SecondLength))
	{
		return;
	}
	const int Samples = std::max(static_cast<int>(std::max(FirstLength, SecondLength) / PointSpacing), 4) + 1;
	const std::vector<double> SampleFractions = Fractions(Samples);
	const FStreetPolyline FirstPoints = AtFractions(First, SampleFractions);
	const FStreetPolyline SecondPoints = AtFractions(Second, SampleFractions);
	const int MiddleBegin = Samples / 4;
	const int MiddleEnd = std::max(3 * Samples / 4, Samples / 4 + 1);
	std::vector<double> Distances;
	for (int Index = MiddleBegin; Index < MiddleEnd && Index < Samples; ++Index)
	{
		Distances.push_back(Norm(FirstPoints[Index] - SecondPoints[Index]));
	}
	const double Separation = Median(Distances);
	if (!(SeparationLow <= Separation && Separation <= SeparationHigh))
	{
		return;
	}
	FStreetPolyline Middle;
	for (int Index = 0; Index < Samples; ++Index)
	{
		Middle.push_back((FirstPoints[Index] + SecondPoints[Index]) / 2);
	}
	const FStreetPolyline AxisLine = Axis(Middle);
	const std::vector<double> AxisAlong = Polyline::Arclength(AxisLine);
	const double Length = AxisAlong.back();
	const double StartRamp = GoreLength(*FirstChain[0].Segment, Separation, Length);
	double EndRamp = StartRamp;
	if (!FindSplit(Network, Network.NodeOf(LastEnd(FirstChain))).has_value())
	{
		EndRamp = std::min(Assumptions::DualSectionJunctionRamp, Length / 2);
	}
	std::vector<double> Opening;
	std::vector<FStreetPoint> Normals;
	for (double Fraction : SampleFractions)
	{
		const double Along = Fraction * Length;
		Opening.push_back(std::min(Ramp(Along, StartRamp), Ramp(Length - Along, EndRamp)) * Separation / 2);
		Normals.push_back(Polyline::RightOf(Polyline::DirectionAt(AxisLine, AxisAlong, Along)));
	}
	const int Middle2 = Samples / 2;
	const double Projection = Dot(FirstPoints[Middle2] - AxisLine[Middle2], Normals[Middle2]);
	double Side = (Projection > 0) - (Projection < 0);
	if (Side == 0.0)
	{
		Side = 1.0;
	}
	const std::pair<const FChain*, double> Carriageways[] = {{&FirstChain, Side}, {&SecondChain, -Side}};
	for (const auto& [Chain, Sign] : Carriageways)
	{
		FStreetPolyline NewLine;
		for (int Index = 0; Index < Samples; ++Index)
		{
			NewLine.push_back(AxisLine[Index] + Normals[Index] * (Sign * Opening[Index]));
		}
		PlaceNodes(*Chain, NewLine, Changes);
	}
}

/** The way with moved nodes and added shape points, or the way itself when nothing changed. */
FStreetWay Rebuilt(const FStreetWay& Way, const FRedrawChanges& Changes)
{
	bool bTouched = false;
	for (size_t Index = 0; Index < Way.NodeIds.size() && !bTouched; ++Index)
	{
		bTouched = Changes.MovedNodes.count(Way.NodeIds[Index]) > 0;
		if (Index > 0)
		{
			bTouched = bTouched || Changes.AddedPoints.count({Way.NodeIds[Index - 1], Way.NodeIds[Index]}) > 0;
		}
	}
	if (!bTouched)
	{
		return Way;
	}
	FStreetWay Result;
	Result.Id = Way.Id;
	Result.Tags = Way.Tags;
	for (size_t Index = 0; Index < Way.NodeIds.size(); ++Index)
	{
		if (Index > 0)
		{
			const auto Added = Changes.AddedPoints.find({Way.NodeIds[Index - 1], Way.NodeIds[Index]});
			if (Added != Changes.AddedPoints.end())
			{
				for (const FAddedPoint& Point : Added->second)
				{
					Result.NodeIds.push_back(Point.NodeId);
					Result.Points.push_back(Point.Point);
				}
			}
		}
		Result.NodeIds.push_back(Way.NodeIds[Index]);
		const auto Moved = Changes.MovedNodes.find(Way.NodeIds[Index]);
		Result.Points.push_back(Moved != Changes.MovedNodes.end() ? Moved->second : Way.Points[Index]);
	}
	return Result;
}
}

std::vector<FStreetWay> Straighten(const std::vector<FStreetWay>& Ways, const FSectionsByWay& Sections)
{
	const FSegmentNetwork Network(Ways, Sections);
	FRedrawChanges Changes;
	for (int64_t Node : Network.NodeOrder)
	{
		if (Network.NodeKind(Node) != ENodeKind::Junction)
		{
			continue;
		}
		const std::optional<FSplit> Split = FindSplit(Network, Node);
		if (!Split.has_value())
		{
			continue;
		}
		const auto Section = FindSection(Network, Split->Incoming, Split->Outgoing);
		if (Section.has_value())
		{
			Redraw(Network, Section->first, Section->second, Changes);
		}
	}
	std::vector<FStreetWay> Result;
	Result.reserve(Ways.size());
	for (const FStreetWay& Way : Ways)
	{
		Result.push_back(Rebuilt(Way, Changes));
	}
	return Result;
}
}
