#include "Splits.h"

#include <algorithm>

#include "Assumptions.h"
#include "Corrections.h"
#include "OsmTags.h"

namespace WorldBuilder
{
namespace
{
/** True when the arm belongs to a one-way road. */
bool IsOneWayArm(const FSegmentNetwork& Network, const FSegmentEnd& End)
{
	return OsmTags::IsOneway(Network.Segments[End.Segment].Way->Tags);
}

/** True when the one-way traffic of the arm drives away from the node. */
bool LeavesNode(const FSegmentNetwork& Network, const FSegmentEnd& End)
{
	const bool bForward = !OsmTags::IsReversedOneway(Network.Segments[End.Segment].Way->Tags);
	return End.bAtStart == bForward;
}
}

std::optional<FSplit> FindSplit(const FSegmentNetwork& Network, int64_t Node)
{
	std::vector<FSegmentEnd> Ends;
	for (const FSegmentEnd& End : Network.EndsAt.at(Node))
	{
		if (!IsMinor(Network, End))
		{
			Ends.push_back(End);
		}
	}
	if (Ends.size() != 3)
	{
		return std::nullopt;
	}
	std::vector<FSegmentEnd> TwoWays;
	std::vector<FSegmentEnd> OneWays;
	for (const FSegmentEnd& End : Ends)
	{
		if (IsOneWayArm(Network, End))
		{
			OneWays.push_back(End);
		}
		else
		{
			TwoWays.push_back(End);
		}
	}
	if (TwoWays.size() != 1 || OneWays.size() != 2)
	{
		return std::nullopt;
	}
	std::vector<FSegmentEnd> Incoming;
	std::vector<FSegmentEnd> Outgoing;
	for (const FSegmentEnd& End : OneWays)
	{
		if (LeavesNode(Network, End))
		{
			Outgoing.push_back(End);
		}
		else
		{
			Incoming.push_back(End);
		}
	}
	if (Incoming.size() != 1 || Outgoing.size() != 1)
	{
		return std::nullopt;
	}
	for (const FSegmentEnd& OneWay : OneWays)
	{
		if (Deflection(Network, TwoWays[0], OneWay) > Assumptions::SplitMaxDeflection)
		{
			return std::nullopt;
		}
	}
	const double Gore = std::max({Network.ClearDistance(Incoming[0], Outgoing), Network.ClearDistance(Outgoing[0], Incoming),
								  Assumptions::TaperMinLength});
	return FSplit{TwoWays[0], Incoming[0], Outgoing[0], Gore};
}
}
