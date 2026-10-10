#pragma once

#include "CoreMinimal.h"
#include "MinimapIndex.h"

class FLaneNetwork;

/** A planned drive along the lane graph, simplified for drawing and for measuring progress. */
struct MAPRUNTIME_API FMinimapRoute
{
	/** Lanes in driving order, road lanes and junction connections. */
	TArray<int32> LaneIds;
	/** The route's centre line, world metres (x east, y south). */
	TArray<FVector2f> Points;
	/** Distance along the route at every point, metres. */
	TArray<float> Arc;
	float TotalMeters = 0.f;
	/** Time the search took, milliseconds. */
	double SearchMilliseconds = 0.0;

	bool IsValid() const { return Points.Num() >= 2; }

	/** How far a position is from the route, and how much of the route is left after the closest point; false when empty. */
	bool Locate(const FVector2f& PositionM, float& OutDistanceFromRouteM, float& OutRemainingM) const;

	/** Adds the parts of the route within RadiusM of CentreM to a map frame as strokes of class MinimapRouteTier. */
	void AppendToFrame(const FVector2f& CentreM, float RadiusM, float HeadingRadians, FMinimapFrame& OutFrame) const;
};

namespace MinimapRouting
{
/**
 * A* search over the main loop of the lane graph (lanes with bGood, following Next, so one-way streets and banned turns
 * are respected) from StartLane to GoalLane. The cost is the driving time at the speed limits; the heuristic is the
 * straight line at 36 m/s. Safe to call on a worker thread. Returns false when the goal cannot be reached.
 */
MAPRUNTIME_API bool FindRoute(const FLaneNetwork& Network, int32 StartLane, int32 GoalLane, FMinimapRoute& OutRoute);
}
