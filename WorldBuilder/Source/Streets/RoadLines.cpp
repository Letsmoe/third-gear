#include "RoadLines.h"

#include "Assumptions.h"
#include "Corrections.h"
#include "Junctions.h"
#include "OsmTags.h"

namespace WorldBuilder
{
namespace
{
/** Adds a marking when its line exists. */
void AddMarking(std::vector<FMarking>& Markings, const char* Kind, std::optional<FStreetPolyline> Points)
{
	if (Points.has_value())
	{
		Markings.push_back({Kind, std::move(*Points)});
	}
}

/**
 * The markings of one painted line. A double centre line is two solid lines; a turn lane's line is broken along the
 * road and solid over the queueing length before the junction it leads to.
 */
std::vector<FMarking> PaintedPieces(const FSegmentLayout& Layout, const FLineKey& Key, const std::string& Kind)
{
	std::vector<FMarking> Pieces;
	if (Kind == "centre_double")
	{
		const auto [StartS, EndS] = Layout.LineRange(Key);
		const double HalfGap = Assumptions::DoubleLineSpacing / 2;
		AddMarking(Pieces, "solid", Layout.LineBetween(Key, StartS, EndS, -HalfGap));
		AddMarking(Pieces, "solid", Layout.LineBetween(Key, StartS, EndS, HalfGap));
		return Pieces;
	}
	if (Kind == "turn_lane")
	{
		const auto [StartS, EndS] = Layout.LineRange(Key);
		if (Key.Travel == ETravel::Forward)
		{
			const double QueueStart = std::max(StartS, EndS - Assumptions::TurnLaneQueueLength);
			AddMarking(Pieces, "turn_lane_dash", Layout.LineBetween(Key, StartS, QueueStart));
			AddMarking(Pieces, "turn_lane_solid", Layout.LineBetween(Key, QueueStart, EndS));
			return Pieces;
		}
		const double QueueEnd = std::min(EndS, StartS + Assumptions::TurnLaneQueueLength);
		AddMarking(Pieces, "turn_lane_solid", Layout.LineBetween(Key, StartS, QueueEnd));
		AddMarking(Pieces, "turn_lane_dash", Layout.LineBetween(Key, QueueEnd, EndS));
		return Pieces;
	}
	AddMarking(Pieces, Kind.c_str(), Layout.LineXy(Key));
	return Pieces;
}

/** True where the long rural dashes are painted: from RuralDashSpeed. */
bool IsRural(const FTags& Tags)
{
	const std::optional<double> Speed = OsmTags::SpeedLimit(Tags);
	return Speed.has_value() && *Speed >= RuralDashSpeed;
}
}

std::vector<FMarking> FRoadLines::Painted() const
{
	std::vector<FMarking> Result;
	for (const FSegmentLayout& Layout : Layouts)
	{
		for (const auto& [Key, Kind] : Layout.Painted)
		{
			for (FMarking& Piece : PaintedPieces(Layout, Key, Kind))
			{
				Result.push_back(std::move(Piece));
			}
		}
	}
	Result.insert(Result.end(), Guides.begin(), Guides.end());
	return Result;
}

FKerbs FRoadLines::Kerbs(const FSegmentLayout& Layout) const
{
	return {Layout.LineXy({ELineKind::Kerb, ESide::Left}), Layout.LineXy({ELineKind::Kerb, ESide::Right})};
}

FRoadLines BuildRoadLines(const std::shared_ptr<const std::vector<FStreetWay>>& Ways, const FSectionsByWay& Sections,
						  const std::unordered_map<int64_t, bool>& Urban, const std::vector<FStreetPoint>& SignalPoints)
{
	FRoadLines Result;
	Result.Ways = Ways;
	Result.Network = std::make_shared<FSegmentNetwork>(*Ways, Sections);
	FSegmentNetwork& Network = *Result.Network;
	FixShortNarrowings(Network);
	std::vector<FPaintedLines> Painted;
	std::vector<bool> UrbanBySegment;
	for (const FSegment& Segment : Network.Segments)
	{
		Painted.push_back(PaintedLines(Segment.Way->Tags, Segment.Section, IsRural(Segment.Way->Tags)));
		UrbanBySegment.push_back(Urban.at(Segment.Way->Id));
	}
	const FJunctionLines Junctions(Network, SignalPoints);
	const FMouths Mouths = Junctions.Mouths();
	Result.Layouts = BuildLayouts(Network, Painted, UrbanBySegment, Mouths);
	Result.Guides = Junctions.GuideLines(Result.Layouts, Mouths);
	Result.Gores = SplitGores(Network, Result.Layouts);
	return Result;
}
}
