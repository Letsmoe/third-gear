#include "BridgeCover.h"

#include <algorithm>

namespace WorldBuilder
{
FBridgeCover::FBridgeCover(std::vector<FBridgeLine> Lines)
{
	for (FBridgeLine& Line : Lines)
	{
		if (Line.Line.size() < 2)
		{
			continue;
		}
		FBridge Bridge{std::move(Line), 1e300, 1e300, -1e300, -1e300};
		for (const FStreetPoint& Point : Bridge.Bridge.Line)
		{
			Bridge.MinX = std::min(Bridge.MinX, Point.X - Bridge.Bridge.Reach);
			Bridge.MinY = std::min(Bridge.MinY, Point.Y - Bridge.Bridge.Reach);
			Bridge.MaxX = std::max(Bridge.MaxX, Point.X + Bridge.Bridge.Reach);
			Bridge.MaxY = std::max(Bridge.MaxY, Point.Y + Bridge.Bridge.Reach);
		}
		Bridges.push_back(std::move(Bridge));
	}
}

bool FBridgeCover::Covers(const FStreetPoint& Point, int64_t FirstNode, int64_t LastNode) const
{
	for (const FBridge& Bridge : Bridges)
	{
		if (Point.X < Bridge.MinX || Point.X > Bridge.MaxX || Point.Y < Bridge.MinY || Point.Y > Bridge.MaxY)
		{
			continue;
		}
		const FBridgeLine& Line = Bridge.Bridge;
		const bool bCarriesRoadOn = Line.StartNode == FirstNode || Line.StartNode == LastNode
									|| Line.EndNode == FirstNode || Line.EndNode == LastNode;
		if (!bCarriesRoadOn && Polyline::DistanceTo(Line.Line, Point) <= Line.Reach)
		{
			return true;
		}
	}
	return false;
}
}
