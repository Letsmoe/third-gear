#include "BridgeCover.h"

#include <algorithm>
#include <string>
#include <utility>

namespace WorldBuilder
{
namespace
{
/**
 * How far beyond a bridge deck's edge the survey is still disturbed by its abutments and embankments: about one cell
 * of the 5 m terrain grid and the slope of the embankment.
 */
constexpr double BridgeSurveyMargin = 6.0;
/** Deck widths when OSM gives none: a railway track with its walkway, and a footbridge. */
constexpr double RailBridgeWidth = 5.0;
constexpr double FootBridgeWidth = 3.0;
constexpr double RoadBridgeWidth = 8.0;

/** True for a way on a bridge (any bridge= value but no). */
bool IsBridge(const FOsmWay& Way)
{
	const std::string* Bridge = FindTag(Way.Tags, "bridge");
	return Bridge != nullptr && *Bridge != "no";
}

/** Adds the bridges among Ways with the given deck width (or their own from Widths) to Lines. */
void AddBridgeLines(const std::vector<FOsmWay>& Ways, double DefaultWidth, const std::unordered_map<int64_t, double>& Widths,
					std::vector<FBridgeLine>& Lines)
{
	for (const FOsmWay& Way : Ways)
	{
		if (!IsBridge(Way) || Way.Points.size() < 2)
		{
			continue;
		}
		const auto Width = Widths.find(Way.Id);
		const double DeckWidth = Width == Widths.end() ? DefaultWidth : Width->second;
		FBridgeLine Line;
		for (const FWorldPoint& Point : Way.Points)
		{
			Line.Line.push_back({Point.X, Point.Y});
		}
		Line.Reach = 0.5 * DeckWidth + BridgeSurveyMargin;
		Line.StartNode = Way.NodeIds.front();
		Line.EndNode = Way.NodeIds.back();
		Lines.push_back(std::move(Line));
	}
}
}

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

FBridgeCover BridgeCoverOf(const FOsmData& Osm, const std::unordered_map<int64_t, double>& RoadWidths)
{
	std::vector<FBridgeLine> Lines;
	const std::unordered_map<int64_t, double> NoWidths;
	AddBridgeLines(Osm.Roads, RoadBridgeWidth, RoadWidths, Lines);
	AddBridgeLines(Osm.Railways, RailBridgeWidth, NoWidths, Lines);
	AddBridgeLines(Osm.Footways, FootBridgeWidth, NoWidths, Lines);
	return FBridgeCover(std::move(Lines));
}
}
