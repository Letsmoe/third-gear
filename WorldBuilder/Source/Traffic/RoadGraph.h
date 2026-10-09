#pragma once

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Network.h"
#include "Roads.h"

/** Road ends, node degrees and line geometry of the drivable ways (Python: furniture.py's RoadGraph). */
namespace WorldBuilder
{
/** Speed limit in km/h from the way's tags or the German default for its class and surroundings. 0 means no limit. */
double ParseSpeed(const FTags& Tags, bool bUrban);

/** Whether vehicles may move along increasing (Travel = +1) or decreasing (-1) node index of a way with these tags. */
bool CanTravelByTags(const FTags& Tags, int Travel);

/** Where a node lies on a way: the way's index in the graph and the node's index in the way. */
struct FNodeEntry
{
	int Way = 0;
	int Index = 0;
};

/** A position on a way with the unit travel direction there. */
struct FPositionAndDirection
{
	FStreetPoint Position;
	FStreetPoint Direction;
};

/** The point of a way nearest to some place: the way, the distance along its line and the signed offset to its right. */
struct FNearestWay
{
	int Way = 0;
	double Along = 0.0;
	double Offset = 0.0;
};

class FRoadGraph
{
public:
	/**
	 * Widths: the carriageway width of each way by way id. Buildings decide which ways run through town. The ways
	 * must outlive the graph.
	 */
	FRoadGraph(const std::vector<FStreetWay>& InWays, const std::unordered_map<int64_t, double>& Widths,
			   const FBuildingIndex& Buildings);

	const std::vector<FStreetWay>& Ways;
	std::vector<double> WayWidth;
	std::vector<bool> WayUrban;
	std::vector<double> WaySpeed;
	/** The cumulative distance along each way's line, and so its length at the back. */
	std::vector<std::vector<double>> WayAlong;
	std::unordered_map<int64_t, std::vector<FNodeEntry>> AtNode;
	std::unordered_map<int64_t, FStreetPoint> NodePoint;
	std::unordered_map<int64_t, int> Degree;

	/** The entries of a node, or nullptr when no way has it. */
	const std::vector<FNodeEntry>* EntriesAt(int64_t Node) const;

	/** True for a node of three or more road ends that isn't part of a roundabout. */
	bool IsJunction(int64_t Node) const;

	/** Whether vehicles may move along increasing (Travel = +1) or decreasing (-1) node index. */
	bool CanTravel(int Way, int Travel) const;

	/** Position at distance S along the way and the unit travel direction there. */
	FPositionAndDirection PointAndDirection(int Way, double S, int Travel) const;

	/** Distance along the way's line of its Index-th node. */
	double NodeS(int Way, int Index) const;

	/** Lanes of one direction of a way. */
	int LanesPerDirection(int Way) const;

	/** The length of a way's line. */
	double LineLength(int Way) const;

	/** Distance along a way's line of the point nearest to the given one. */
	double Project(int Way, const FStreetPoint& Point) const;

	/** The nearest way to a point within MaxDistance, or nothing. */
	std::optional<FNearestWay> NearestWay(double X, double Y, double MaxDistance) const;

private:
	mutable std::vector<std::vector<double>> NodeSCache;
	/** Segments by 50 m cell: way index and segment index. */
	std::unordered_map<int64_t, std::vector<std::pair<int, int>>> SegmentCells;

	void BuildSegmentCells();
};
}
