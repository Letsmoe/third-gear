#include "Corrections.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

#include "Assumptions.h"
#include "OsmTags.h"

namespace WorldBuilder
{
namespace
{
/**
 * True when both roads are one-way or both two-way, and both slip roads or neither: a one-way branch where a dual
 * carriageway splits is not a narrowing of the road before the split.
 */
bool SameKindOfRoad(const FTags& Tags, const FTags& OtherTags)
{
	return OsmTags::IsOneway(Tags) == OsmTags::IsOneway(OtherTags)
		&& OsmTags::IsLink(Tags) == OsmTags::IsLink(OtherTags);
}

/** A straight neighbour of a short piece: its width, our end, its end and its cross-section. */
struct FNeighbourSection
{
	double Width = 0.0;
	FSegmentEnd End;
	FSegmentEnd Neighbour;
	const FCrossSection* Section = nullptr;
};

/** The cross-section a short narrow piece should have (in its own frame), or nothing when it is fine. */
std::optional<FCrossSection> NarrowingReplacement(const FSegmentNetwork& Network, const FSegment& Segment)
{
	if (Segment.Length > Assumptions::ShortNarrowingMaxLength)
	{
		return std::nullopt;
	}
	const double Width = Segment.Section.Width();
	std::vector<FNeighbourSection> Neighbours;
	for (bool bAtStart : {true, false})
	{
		const FSegmentEnd End = {Segment.Id, bAtStart};
		const std::optional<FSegmentEnd> Neighbour = StraightNeighbour(Network, End);
		if (!Neighbour.has_value())
		{
			return std::nullopt;
		}
		const FSegment& NeighbourSegment = Network.Segments[Neighbour->Segment];
		if (!SameKindOfRoad(Segment.Way->Tags, NeighbourSegment.Way->Tags))
		{
			return std::nullopt;
		}
		const FCrossSection& Section = NeighbourSegment.Section;
		if (Section.Width() < Width + Assumptions::NarrowingMinDifference)
		{
			return std::nullopt;
		}
		Neighbours.push_back({Section.Width(), End, *Neighbour, &Section});
	}
	const FNeighbourSection* Narrowest = &Neighbours[0];
	if (Neighbours[1].Width < Narrowest->Width)
	{
		Narrowest = &Neighbours[1];
	}
	// Facing out of the node, our end and the neighbour's look in opposite directions when both ways run the same
	// way (one ends where the other starts); otherwise one of them runs against the other.
	if (Narrowest->End.bAtStart == Narrowest->Neighbour.bAtStart)
	{
		return Narrowest->Section->Mirrored();
	}
	return *Narrowest->Section;
}
}

int FixShortNarrowings(FSegmentNetwork& Network)
{
	int Changed = 0;
	for (int Pass = 0; Pass < NarrowingPasses; ++Pass)
	{
		std::vector<std::pair<int, FCrossSection>> Fixes;
		for (const FSegment& Segment : Network.Segments)
		{
			std::optional<FCrossSection> Replacement = NarrowingReplacement(Network, Segment);
			if (Replacement.has_value())
			{
				Fixes.emplace_back(Segment.Id, std::move(*Replacement));
			}
		}
		for (auto& [SegmentId, Replacement] : Fixes)
		{
			Network.Segments[SegmentId].Section = std::move(Replacement);
		}
		Changed += static_cast<int>(Fixes.size());
		if (Fixes.empty())
		{
			break;
		}
	}
	return Changed;
}

std::optional<FSegmentEnd> StraightNeighbour(const FSegmentNetwork& Network, const FSegmentEnd& End)
{
	const int64_t Node = Network.NodeOf(End);
	const ENodeKind Kind = Network.NodeKind(Node);
	if (Kind == ENodeKind::Continuation)
	{
		return Network.OtherEnd(End);
	}
	if (Kind != ENodeKind::Junction)
	{
		return std::nullopt;
	}
	std::optional<FSegmentEnd> Best;
	double BestTurn = 0.0;
	for (const FSegmentEnd& Other : Network.EndsAt.at(Node))
	{
		if (Other == End || IsMinor(Network, Other))
		{
			continue;
		}
		const double Turn = Deflection(Network, End, Other);
		if (Turn <= Assumptions::ThroughMaxDeflectionDegrees && (!Best.has_value() || Turn < BestTurn))
		{
			Best = Other;
			BestTurn = Turn;
		}
	}
	return Best;
}

bool IsMinor(const FSegmentNetwork& Network, const FSegmentEnd& End)
{
	return Assumptions::MinorArmClasses.count(OsmTags::BaseClass(Network.Segments[End.Segment].Way->Tags)) > 0;
}

double Deflection(const FSegmentNetwork& Network, const FSegmentEnd& First, const FSegmentEnd& Second)
{
	const double Through = Dot(Network.ArmDirection(First), -Network.ArmDirection(Second));
	return std::acos(std::max(-1.0, std::min(1.0, Through))) * 180.0 / std::numbers::pi;
}
}
