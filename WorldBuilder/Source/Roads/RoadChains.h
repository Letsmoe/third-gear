#pragma once

#include <cstdint>
#include <vector>

#include "Network.h"

/**
 * Roads as the driver sees them: segments joined into chains through every continuation node and straight through
 * junctions where the same road carries on. Each chain gets one smooth centreline and one height profile, so a road
 * has no kink or step where OSM splits its way.
 */
namespace WorldBuilder
{
/** A segment in a chain, run along its node order or against it. */
struct FChainPiece
{
	int Segment = 0;
	bool bReversed = false;
};

struct FRoadChain
{
	std::vector<FChainPiece> Pieces;
	/** The node where each piece starts, then the node where the last one ends. */
	std::vector<int64_t> Nodes;
	/** True for a ring, whose last node is its first. */
	bool bClosed = false;
};

/** Every segment of the network in exactly one chain. */
std::vector<FRoadChain> BuildRoadChains(const FSegmentNetwork& Network);
}
