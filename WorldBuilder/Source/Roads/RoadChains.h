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

/** A chain's line and its samples, taken piece by piece so that every node of the chain is a sample. */
struct FChainSamples
{
	/** The pieces' points joined in the chain's direction. */
	FStreetPolyline Original;
	/** Samples about Spacing apart along Original. */
	FStreetPolyline Samples;
	/** The sample at each of the chain's nodes. */
	std::vector<int> NodeSamples;
};

/** Samples a chain's line about Spacing metres apart, every node included. */
FChainSamples SampleChain(const FSegmentNetwork& Network, const FRoadChain& Chain, double Spacing);

/** The piece's points in the chain's direction. */
FStreetPolyline PieceLine(const FSegmentNetwork& Network, const FChainPiece& Piece);

/** The samples of a chain whose position is fixed: its ends and every junction it runs through. */
std::vector<int> PinnedSamples(const FSegmentNetwork& Network, const FRoadChain& Chain,
							   const std::vector<int>& NodeSamples);

/** The longest segment of the chain, whose road class the chain takes. */
const FSegment& LongestSegment(const FSegmentNetwork& Network, const FRoadChain& Chain);
}
