#include "Lines.h"

#include <algorithm>
#include <set>

#include "Assumptions.h"
#include "OsmTags.h"

namespace WorldBuilder
{
ESide MirroredSide(ESide Side)
{
	switch (Side)
	{
	case ESide::Left:
		return ESide::Right;
	case ESide::Right:
		return ESide::Left;
	default:
		return ESide::None;
	}
}

ETravel MirroredTravel(ETravel Travel)
{
	switch (Travel)
	{
	case ETravel::Forward:
		return ETravel::Backward;
	case ETravel::Backward:
		return ETravel::Forward;
	default:
		return Travel;
	}
}

FLineKey Mirrored(const FLineKey& Key)
{
	return {Key.Kind, MirroredSide(Key.Side), MirroredTravel(Key.Travel), Key.Index};
}

FLineOffsets MirroredLines(const FLineOffsets& Lines)
{
	FLineOffsets Result;
	for (const auto& [Key, Offset] : Lines)
	{
		Result.Set(Mirrored(Key), -Offset);
	}
	return Result;
}

namespace
{
/** The inner and outer lines of the cycle lanes. */
FLineOffsets CycleLines(const FCrossSection& Section)
{
	FLineOffsets Lines;
	for (const FStripEdges& Edge : Section.StripEdges())
	{
		if (Edge.Strip->Kind != EStripKind::CycleLane)
		{
			continue;
		}
		if (Edge.Left + Edge.Right < 0)
		{
			Lines.Set({ELineKind::Cycle, ESide::Left}, Edge.Right);
			Lines.Set({ELineKind::CycleOuter, ESide::Left}, Edge.Left);
		}
		else
		{
			Lines.Set({ELineKind::Cycle, ESide::Right}, Edge.Left);
			Lines.Set({ELineKind::CycleOuter, ESide::Right}, Edge.Right);
		}
	}
	return Lines;
}

/**
 * The key of the line between Lanes[Position] and Lanes[Position + 1] (left to right), or nothing between lanes that
 * are not divided (shared lanes).
 */
std::optional<FLineKey> DividerKey(const std::vector<FStripEdges>& Lanes, size_t Position)
{
	const ETravel LeftTravel = Lanes[Position].Strip->Travel;
	const ETravel RightTravel = Lanes[Position + 1].Strip->Travel;
	if (LeftTravel == ETravel::Backward && RightTravel == ETravel::Forward)
	{
		return FLineKey{ELineKind::Centre};
	}
	if (LeftTravel != RightTravel || (LeftTravel != ETravel::Forward && LeftTravel != ETravel::Backward))
	{
		return std::nullopt;
	}
	int Index = 0;
	if (LeftTravel == ETravel::Forward)
	{
		// Forward lanes keep to the right kerb: count the forward lanes right of the line.
		for (size_t Lane = Position + 1; Lane < Lanes.size(); ++Lane)
		{
			Index += Lanes[Lane].Strip->Travel == ETravel::Forward ? 1 : 0;
		}
		return FLineKey{ELineKind::Lane, ESide::None, ETravel::Forward, Index};
	}
	for (size_t Lane = 0; Lane <= Position; ++Lane)
	{
		Index += Lanes[Lane].Strip->Travel == ETravel::Backward ? 1 : 0;
	}
	return FLineKey{ELineKind::Lane, ESide::None, ETravel::Backward, Index};
}
}

FLineOffsets SectionLines(const FCrossSection& Section)
{
	const double Width = Section.Width();
	FLineOffsets Lines;
	Lines.Set({ELineKind::Kerb, ESide::Left}, -Width / 2);
	Lines.Set({ELineKind::Kerb, ESide::Right}, Width / 2);
	std::vector<FStripEdges> Lanes;
	for (const FStripEdges& Edge : Section.StripEdges())
	{
		if (Edge.Strip->Kind == EStripKind::TravelLane)
		{
			Lanes.push_back(Edge);
		}
	}
	if (Lanes.empty())
	{
		return Lines;
	}
	Lines.Set({ELineKind::Edge, ESide::Left}, Lanes.front().Left);
	Lines.Set({ELineKind::Edge, ESide::Right}, Lanes.back().Right);
	for (size_t Position = 0; Position + 1 < Lanes.size(); ++Position)
	{
		const std::optional<FLineKey> Key = DividerKey(Lanes, Position);
		if (Key.has_value())
		{
			Lines.Set(*Key, Lanes[Position].Right);
		}
	}
	for (const auto& [Key, Offset] : CycleLines(Section))
	{
		Lines.Set(Key, Offset);
	}
	return Lines;
}

namespace
{
/**
 * A double solid line (Fahrstreifenbegrenzung, Zeichen 295) between the directions where one of them has two or more
 * lanes (VwV-StVO zu Zeichen 295 and 340), else the dashed centre line.
 */
std::string CentreLineKind(const FCrossSection& Section, const std::string& Dash)
{
	int Forward = 0;
	int Backward = 0;
	for (const FStrip& Lane : Section.TravelLanes())
	{
		Forward += Lane.Travel == ETravel::Forward ? 1 : 0;
		Backward += Lane.Travel == ETravel::Backward ? 1 : 0;
	}
	if (std::max(Forward, Backward) >= 2)
	{
		return "centre_double";
	}
	return Dash;
}

/** True when two neighbouring lanes lead to different places and at least one of them only turns. */
bool TurnsApart(const std::string& First, const std::string& Second)
{
	const std::vector<std::string> FirstParts = OsmTags::SplitText(First, ';');
	const std::vector<std::string> SecondParts = OsmTags::SplitText(Second, ';');
	const std::set<std::string> FirstTurns(FirstParts.begin(), FirstParts.end());
	const std::set<std::string> SecondTurns(SecondParts.begin(), SecondParts.end());
	if (FirstTurns == SecondTurns)
	{
		return false;
	}
	return FirstTurns.count("through") == 0 || SecondTurns.count("through") == 0;
}

/**
 * A broad line beside a lane that turns where its neighbour doesn't (or turns elsewhere): dashed along the road,
 * solid over the queueing length before the junction; else the normal dashed lane line.
 */
std::string LaneLineKind(const FTags& Tags, const FCrossSection& Section, const FLineKey& Key, const std::string& Dash)
{
	const std::vector<std::string> Turns = OsmTags::TurnLanes(Tags, Key.Travel);
	int LaneCount = 0;
	for (const FStrip& Lane : Section.TravelLanes())
	{
		LaneCount += Lane.Travel == Key.Travel ? 1 : 0;
	}
	if (static_cast<int>(Turns.size()) != LaneCount)
	{
		return Dash;
	}
	// Lane lines are counted from the kerb, the turn lanes from the driver's left: line k lies between the turn lanes
	// LaneCount - k and LaneCount - k + 1 (from 1).
	const std::string& LeftLane = Turns[LaneCount - Key.Index - 1];
	const std::string& RightLane = Turns[LaneCount - Key.Index];
	if (TurnsApart(LeftLane, RightLane))
	{
		return "turn_lane";
	}
	return Dash;
}

/** True when the centre and lane lines of a marked-class road are painted. */
bool LaneLinesPainted(const FTags& Tags, const FCrossSection& Section)
{
	if (Section.LaneCount() < 2 || OsmTags::IsZone30(Tags))
	{
		return false;
	}
	double NarrowestLane = 1e300;
	for (const FStrip& Lane : Section.TravelLanes())
	{
		NarrowestLane = std::min(NarrowestLane, Lane.Width);
	}
	if (NarrowestLane < Assumptions::MinMarkedLaneWidth)
	{
		return false;
	}
	return OsmTags::IsOneway(Tags) || Section.Width() >= Assumptions::CentreLineMinWidth;
}
}

FPaintedLines PaintedCycleLines(const FTags& Tags, const FCrossSection& Section)
{
	FPaintedLines Painted;
	for (const auto& [Key, Offset] : SectionLines(Section))
	{
		if (Key.Kind != ELineKind::Cycle)
		{
			continue;
		}
		std::string Kind = "cycle_exclusive";
		if (OsmTags::CycleLaneIsAdvisory(Tags, Key.Side == ESide::Right))
		{
			Kind = "cycle_advisory";
		}
		Painted.Set(Key, Kind);
	}
	return Painted;
}

FPaintedLines PaintedLines(const FTags& Tags, const FCrossSection& Section, bool bRural)
{
	FPaintedLines Painted = PaintedCycleLines(Tags, Section);
	const std::string RoadClass = OsmTags::Highway(Tags);
	if (Assumptions::PaintedClasses.count(RoadClass) == 0 || OsmTags::TagEquals(Tags, "lane_markings", "no")
		|| OsmTags::TagEquals(Tags, "area", "yes"))
	{
		return Painted;
	}
	const double Width = Section.Width();
	const std::string Dash = bRural ? "dash_rural" : "dash_urban";
	const FLineOffsets Lines = SectionLines(Section);
	if (LaneLinesPainted(Tags, Section))
	{
		for (const auto& [Key, Offset] : Lines)
		{
			if (Key.Kind == ELineKind::Centre)
			{
				Painted.Set(Key, CentreLineKind(Section, Dash));
			}
			if (Key.Kind == ELineKind::Lane)
			{
				Painted.Set(Key, LaneLineKind(Tags, Section, Key, Dash));
			}
		}
	}
	const bool bEdgeLines = Assumptions::EdgeLineClasses.count(RoadClass) > 0
		|| (bRural && Width >= Assumptions::RuralEdgeLineMinWidth);
	if (!bEdgeLines)
	{
		return Painted;
	}
	for (const auto& [Key, Offset] : Lines)
	{
		if (Key.Kind == ELineKind::Edge && !Painted.Contains({ELineKind::Cycle, Key.Side}))
		{
			Painted.Set(Key, "edge");
		}
	}
	return Painted;
}

double TaperLength(double Shift, double SpeedKmh, bool bUrban)
{
	const double Length = std::max(SpeedKmh * Shift * Assumptions::TaperSpeedFactor, Assumptions::TaperMinLength);
	if (bUrban)
	{
		return std::min(Length, Assumptions::UrbanTaperMaxLength);
	}
	return Length;
}
}
