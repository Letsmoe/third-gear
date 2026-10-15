#pragma once

#include <vector>

#include "Network.h"

/**
 * Which road keeps its surface through a junction. The most important road at a junction keeps its cross-section
 * across it; every other road there adapts to its kerb line, as a side street starts at the main road's gutter.
 */
namespace WorldBuilder
{
struct FRoadChainModel;

/**
 * Where a chain gives way at a junction: its sample at the junction node, the chain it adapts to there and that chain's
 * sample at the node.
 */
struct FJunctionYield
{
	int NodeSample = 0;
	int Major = 0;
	int MajorNodeSample = 0;
};

/**
 * Fills every chain's Yields. At each junction the chains are ranked by road class, then by whether they go straight
 * through, then by width; every chain there but the first yields to the first.
 */
void AssignJunctionPriorities(const FSegmentNetwork& Network, std::vector<FRoadChainModel>& Chains);
}
