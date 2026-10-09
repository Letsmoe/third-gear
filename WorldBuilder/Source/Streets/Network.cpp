#include "Network.h"

#include <algorithm>
#include <cmath>

#include "Assumptions.h"

namespace WorldBuilder
{
FStreetWay MakeStreetWay(const FOsmWay& Road)
{
	FStreetWay Way;
	Way.Id = Road.Id;
	Way.Tags = Road.Tags;
	Way.NodeIds = Road.NodeIds;
	Way.Points.reserve(Road.Points.size());
	for (const FWorldPoint& Point : Road.Points)
	{
		Way.Points.push_back({Point.X, Point.Y});
	}
	return Way;
}

std::unordered_map<int64_t, int> NodeDegrees(const std::vector<FStreetWay>& Ways)
{
	std::unordered_map<int64_t, int> Degrees;
	for (const FStreetWay& Way : Ways)
	{
		const size_t Last = Way.NodeIds.size() - 1;
		for (size_t Index = 0; Index < Way.NodeIds.size(); ++Index)
		{
			Degrees[Way.NodeIds[Index]] += (Index == 0 || Index == Last) ? 1 : 2;
		}
	}
	return Degrees;
}

FSegmentNetwork::FSegmentNetwork(const std::vector<FStreetWay>& Ways, const FSectionsByWay& Sections)
{
	const std::unordered_map<int64_t, int> Degrees = NodeDegrees(Ways);
	for (const FStreetWay& Way : Ways)
	{
		SplitWay(Way, Sections.at(Way.Id), Degrees);
	}
	for (const FSegment& Segment : Segments)
	{
		for (const auto& [Node, bAtStart] : {std::pair<int64_t, bool>{Segment.StartNode, true},
											 std::pair<int64_t, bool>{Segment.EndNode, false}})
		{
			auto [Found, bInserted] = EndsAt.try_emplace(Node);
			if (bInserted)
			{
				NodeOrder.push_back(Node);
			}
			Found->second.push_back({Segment.Id, bAtStart});
		}
	}
}

void FSegmentNetwork::SplitWay(const FStreetWay& Way, const FCrossSection& Section,
							   const std::unordered_map<int64_t, int>& Degrees)
{
	size_t Start = 0;
	for (size_t Index = 0; Index < Way.NodeIds.size(); ++Index)
	{
		const int64_t Node = Way.NodeIds[Index];
		NodePoints[Node] = Way.Points[Index];
		const bool bIsLast = Index == Way.NodeIds.size() - 1;
		if (Index == 0 || !(bIsLast || Degrees.at(Node) >= 3))
		{
			continue;
		}
		FSegment Segment;
		Segment.Xy.assign(Way.Points.begin() + Start, Way.Points.begin() + Index + 1);
		Segment.Length = Polyline::Length(Segment.Xy);
		if (Segment.Length > 0.05)
		{
			Segment.Id = static_cast<int>(Segments.size());
			Segment.Way = &Way;
			Segment.StartNode = Way.NodeIds[Start];
			Segment.EndNode = Node;
			Segment.Section = Section;
			Segments.push_back(std::move(Segment));
		}
		Start = Index;
	}
}

ENodeKind FSegmentNetwork::NodeKind(int64_t Node) const
{
	const auto Found = EndsAt.find(Node);
	const size_t Count = Found == EndsAt.end() ? 0 : Found->second.size();
	if (Count >= 3)
	{
		return ENodeKind::Junction;
	}
	if (Count == 2)
	{
		return ENodeKind::Continuation;
	}
	return ENodeKind::DeadEnd;
}

int64_t FSegmentNetwork::NodeOf(const FSegmentEnd& End) const
{
	const FSegment& Segment = Segments[End.Segment];
	if (End.bAtStart)
	{
		return Segment.StartNode;
	}
	return Segment.EndNode;
}

std::optional<FSegmentEnd> FSegmentNetwork::OtherEnd(const FSegmentEnd& End) const
{
	for (const FSegmentEnd& Candidate : EndsAt.at(NodeOf(End)))
	{
		if (Candidate != End)
		{
			return Candidate;
		}
	}
	return std::nullopt;
}

FStreetPolyline FSegmentNetwork::ArmLine(const FSegmentEnd& End) const
{
	const FStreetPolyline& Xy = Segments[End.Segment].Xy;
	if (End.bAtStart)
	{
		return Xy;
	}
	return Polyline::Reversed(Xy);
}

FStreetPoint FSegmentNetwork::ArmDirection(const FSegmentEnd& End, double Reach) const
{
	const FStreetPolyline Line = ArmLine(End);
	const std::vector<double> Along = Polyline::Arclength(Line);
	return Polyline::Unit(Polyline::PointAt(Line, Along, std::min(Reach, Along.back())) - Line[0]);
}

namespace
{
/** An arm's centre line out to the search limit, with the half width its carriageway must stay clear by. */
struct FOtherArm
{
	FStreetPolyline Line;
	double HalfWidth = 0.0;
};

/** True when no probe point across the arm (centre plus an offset along Right) lies closer to the other arm than its half width. */
bool IsProbeRowClear(const FOtherArm& Other, const FStreetPoint& Centre, const FStreetPoint& Right, double HalfWidth,
					 double AcrossStep)
{
	for (int Probe = 0; Probe < MouthProbePoints; ++Probe)
	{
		const double Across = Probe == MouthProbePoints - 1 ? HalfWidth : -HalfWidth + Probe * AcrossStep;
		if (Polyline::DistanceTo(Other.Line, Centre + Right * Across) < Other.HalfWidth)
		{
			return false;
		}
	}
	return true;
}
}

double FSegmentNetwork::ClearDistance(const FSegmentEnd& End, const std::vector<FSegmentEnd>& Others, double Gap) const
{
	const FStreetPolyline Line = ArmLine(End);
	const std::vector<double> Along = Polyline::Arclength(Line);
	const double HalfWidth = Segments[End.Segment].Section.Width() / 2;
	std::vector<FOtherArm> OtherArms;
	for (const FSegmentEnd& Other : Others)
	{
		OtherArms.push_back({Polyline::CutPolyline(ArmLine(Other), 0.0, MouthSearchLimit),
							 Segments[Other.Segment].Section.Width() / 2 + Gap});
	}
	const double Limit = std::min(MouthSearchLimit, Along.back() * Assumptions::TaperMaxSegmentShare);
	const double AcrossStep = (2 * HalfWidth) / (MouthProbePoints - 1);
	const int StepCount = static_cast<int>(std::ceil(Limit / MouthSearchStep));
	for (int StepIndex = 0; StepIndex < StepCount; ++StepIndex)
	{
		const double Distance = StepIndex * MouthSearchStep;
		const FStreetPoint Centre = Polyline::PointAt(Line, Along, Distance);
		const FStreetPoint Right = Polyline::RightOf(Polyline::DirectionAt(Line, Along, Distance));
		bool bClear = true;
		for (const FOtherArm& Other : OtherArms)
		{
			bClear = IsProbeRowClear(Other, Centre, Right, HalfWidth, AcrossStep);
			if (!bClear)
			{
				break;
			}
		}
		if (bClear)
		{
			return Distance;
		}
	}
	return Limit;
}
}
