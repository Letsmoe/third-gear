#include "JunctionPriority.h"

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <unordered_map>

#include "Assumptions.h"
#include "RoadModel.h"

namespace WorldBuilder
{
namespace
{
/** One chain at one junction: where it touches the node and how it ranks there. */
struct FJunctionArm
{
	int Chain = 0;
	int NodeSample = 0;
	int ClassRank = 0;
	bool bThrough = false;
	double HalfWidth = 0.0;
};

/** True when First ranks above Second: higher class, then going through, then wider, then the lower chain index. */
bool RanksAbove(const FJunctionArm& First, const FJunctionArm& Second)
{
	return std::make_tuple(First.ClassRank, First.bThrough, First.HalfWidth, -First.Chain)
		   > std::make_tuple(Second.ClassRank, Second.bThrough, Second.HalfWidth, -Second.Chain);
}

/** Every chain touching each junction node, by node. */
std::unordered_map<int64_t, std::vector<FJunctionArm>> ArmsByJunction(const FSegmentNetwork& Network,
																	  const std::vector<FRoadChainModel>& Chains)
{
	std::unordered_map<int64_t, std::vector<FJunctionArm>> Arms;
	for (int ChainIndex = 0; ChainIndex < static_cast<int>(Chains.size()); ++ChainIndex)
	{
		const FRoadChainModel& Chain = Chains[ChainIndex];
		const size_t Last = Chain.Chain.Nodes.size() - 1;
		for (size_t Index = 0; Index <= Last; ++Index)
		{
			const int64_t Node = Chain.Chain.Nodes[Index];
			if (Network.NodeKind(Node) != ENodeKind::Junction)
			{
				continue;
			}
			FJunctionArm Arm;
			Arm.Chain = ChainIndex;
			Arm.NodeSample = Chain.NodeSamples[Index];
			Arm.ClassRank = Assumptions::RoadClassRank(Chain.RoadClass);
			Arm.bThrough = Chain.Chain.bClosed || (Index > 0 && Index < Last);
			Arm.HalfWidth = Chain.HalfWidths[Arm.NodeSample];
			Arms[Node].push_back(Arm);
		}
	}
	return Arms;
}
}

void AssignJunctionPriorities(const FSegmentNetwork& Network, std::vector<FRoadChainModel>& Chains)
{
	const std::unordered_map<int64_t, std::vector<FJunctionArm>> Arms = ArmsByJunction(Network, Chains);
	for (const int64_t Node : Network.NodeOrder)
	{
		const auto Found = Arms.find(Node);
		if (Found == Arms.end() || Found->second.size() < 2)
		{
			continue;
		}
		const std::vector<FJunctionArm>& AtNode = Found->second;
		const FJunctionArm& Major = *std::max_element(
			AtNode.begin(), AtNode.end(), [](const FJunctionArm& A, const FJunctionArm& B) { return RanksAbove(B, A); });
		for (const FJunctionArm& Arm : AtNode)
		{
			if (Arm.Chain != Major.Chain)
			{
				Chains[Arm.Chain].Yields.push_back({Arm.NodeSample, Major.Chain, Major.NodeSample});
			}
		}
	}
}
}
