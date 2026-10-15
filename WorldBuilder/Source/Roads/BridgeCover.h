#pragma once

#include <cstdint>
#include <vector>

#include "Polyline.h"

/**
 * Where bridges cross over the roads. The terrain survey smears a bridge's embankments and abutments across the road
 * that passes under it, so the ground there is no guide to the road's height.
 */
namespace WorldBuilder
{
/** A bridge deck as its OSM line and the distance either side of it the survey is unreliable. */
struct FBridgeLine
{
	FStreetPolyline Line;
	double Reach = 0.0;
	int64_t StartNode = 0;
	int64_t EndNode = 0;
};

class FBridgeCover
{
public:
	explicit FBridgeCover(std::vector<FBridgeLine> Bridges);

	/**
	 * True when a bridge passes over the point. Bridges that start or end at one of the given nodes are the road
	 * carrying on, not crossing it, and don't count.
	 */
	bool Covers(const FStreetPoint& Point, int64_t FirstNode, int64_t LastNode) const;

private:
	struct FBridge
	{
		FBridgeLine Bridge;
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
	};

	std::vector<FBridge> Bridges;
};
}
