#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "BridgeCover.h"
#include "CrossFall.h"
#include "JunctionPriority.h"
#include "Network.h"
#include "RoadChains.h"
#include "RoadDesign.h"

/**
 * The road model (issue #113): every road of the street model as a smooth centreline with designed heights, sampled
 * about every metre. The surface, junctions, markings, kerbs, terrain and AI lanes are to be derived from it.
 */
namespace WorldBuilder
{
/** Ground height at a world point. */
using FGroundSampler = std::function<double(double X, double Y)>;

struct FRoadChainModel
{
	FRoadChain Chain;
	/** The highway= class of the chain's longest segment, which sets its design values. */
	std::string RoadClass;
	std::string Name;
	FRoadDesign Design;
	/** The chain's OSM line. */
	FStreetPolyline Original;
	/** The smooth centreline, about a metre between points. */
	FStreetPolyline Points;
	/** Distance along the centreline at every point. */
	std::vector<double> Along;
	/** Designed road height at every point. */
	std::vector<double> Heights;
	/** Half the kerb to kerb width at every point, from the segment it lies on. */
	std::vector<double> HalfWidths;
	/** The cross fall at every point. */
	std::vector<FCrossFall> Falls;
	/** Ground height under every point. */
	std::vector<double> Ground;
	/** True for every point a bridge crosses over, whose ground the heights ignore. */
	std::vector<bool> UnderBridge;
	/** Index into Points of each node in Chain.Nodes. */
	std::vector<int> NodeSamples;
	/** The junctions where this chain adapts to a more important road. */
	std::vector<FJunctionYield> Yields;
	/** Largest distance of the centreline from the OSM line, metres. */
	double MaxDeviation = 0.0;
};

struct FRoadModel
{
	std::vector<FRoadChainModel> Chains;
	/** Sample steps held at their chain's steepest grade. */
	int GradeLimited = 0;
};

/** Where the road model's centrelines come from. */
enum class ECentrelines
{
	/** Fitted to the network's lines, which are OSM's. */
	Fit,
	/** The network's lines as they are, when its ways were already redrawn along fitted centrelines (SmoothWays). */
	AsGiven,
};

/**
 * Builds the road model of a segment network over the given ground, ignoring the ground under the bridges. Urban says
 * by way id which roads lie in town, where curves aren't banked.
 */
FRoadModel BuildRoadModel(const FSegmentNetwork& Network, const FGroundSampler& Ground, const FBridgeCover& Bridges,
						  const std::unordered_map<int64_t, bool>& Urban, ECentrelines Centrelines = ECentrelines::Fit);
}
