#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Furniture.h"
#include "Roads.h"

/**
 * Lane graph for AI traffic, derived from the street model's road graph and the signal junctions of the furniture
 * (Python: lanes.py).
 *
 * Every drivable way is cut into segments between graph nodes (way ends and nodes shared with other ways). Each segment
 * gets one lane per allowed direction of travel: the right-hand lane, a centre line offset from the way's centre line.
 * At every node, connection lanes (Bezier curves) link each incoming lane to each outgoing lane, so a car's route is a
 * chain road lane, connection, road lane, connection...
 *
 * Rules are attached to the graph rather than left to the runtime:
 *
 * - the end of a road lane at a junction has a control: signal (stop lines come from the signal approaches), stop or
 *   give way (from the signs placed), priority, or equal (right before left);
 * - every connection lists the connections it crosses or merges with inside the same junction, with the stretch of both
 *   paths where they meet, and says whether this connection has to give way to the other one;
 * - lane points carry a curvature speed limit, the road's legal speed limit travels with the lane.
 *
 * Coordinates are world metres (x east, y south).
 */
namespace WorldBuilder
{
inline constexpr double LaneSpacing = 1.5;
inline constexpr double ConnectionSpacing = 0.75;
inline constexpr double MinLaneOffset = 1.2;
inline constexpr double JunctionTrimBase = 2.5;
// Lanes beside parked cars keep the normal offset this far from a segment end and reach the shifted one
// ParkingTaperLength later.
inline constexpr double ParkingTaperStart = 6.5;
inline constexpr double ParkingTaperLength = 2.5;
inline constexpr double StopTrimExtra = 0.3;
inline constexpr double MaxStopBeforeLane = 8.0;
inline constexpr double ConflictDistance = 1.9;
inline constexpr double ConflictMargin = 1.2;
inline constexpr double LateralAcceleration = 2.0;
inline constexpr double StraightAngleDegrees = 28.0;
inline constexpr double MaxTurnDegrees = 155.0;
inline constexpr double SignSnapDistance = 9.0;
inline constexpr double EdgeInset = 2.0;

// Priority of an arm; a higher level has right of way over a lower one.
inline constexpr int LevelSignal = 4;
inline constexpr int LevelPriority = 3;
inline constexpr int LevelEqual = 2;
inline constexpr int LevelYield = 1;

enum class ELaneKind
{
	Road = 0,
	Connection = 1,
	UTurn = 2,
};

/** A stop line along a road lane: distance along the lane (negative before its start) and the signal approach id. */
struct FLaneStop
{
	double S = 0.0;
	int Approach = 0;
};

/** A connection that crosses or merges with another one inside the same junction. */
struct FLaneConflict
{
	int Other = 0;
	/** The stretch of this connection and of the other one where the paths meet. */
	double OwnFrom = 0.0;
	double OwnTo = 0.0;
	double OtherFrom = 0.0;
	double OtherTo = 0.0;
	/** 0: does not give way, 1: gives way (right before left tie), 2: gives way (strict). */
	int Yields = 0;
};

/** One road lane (a way segment in one direction) or one connection through a junction. */
struct FLane
{
	int Id = 0;
	ELaneKind Kind = ELaneKind::Road;
	int64_t Way = 0;
	FStreetPolyline Points;
	/** Road height per point; filled by AssignLaneHeights or ApplyLaneHeights. */
	std::vector<double> Heights;
	double LimitKmh = 50.0;
	/** Curvature speed limit per point. */
	std::vector<double> CurveKmh;
	bool bRoundabout = false;
	int Tier = 1;
	std::vector<int> Next;
	bool bGood = false;
	// Road lanes.
	int64_t StartNode = 0;
	int64_t EndNode = 0;
	std::string Control = "none";
	int ControlLevel = LevelEqual;
	std::vector<FLaneStop> Stops;
	int Segment = 0;
	int Travel = 0;
	// Connections.
	int FromLane = -1;
	int ToLane = -1;
	int64_t Node = 0;
	/** -1 left, 0 straight, +1 right. */
	int Turn = 0;
	int Level = LevelEqual;
	int Phase = -1;
	std::vector<FLaneConflict> Conflicts;

	/** Cumulative distance along the lane at every point. */
	std::vector<double> Arclength() const;
	double Length() const;
};

/** A bridge way whose lanes ramp between the road heights at the bridge's ends. */
struct FBridgeRamp
{
	int64_t Way = 0;
	FStreetPolyline Line;
	double StartHeight = 0.0;
	double EndHeight = 0.0;
};

struct FLaneGraph
{
	std::vector<FLane> Lanes;
	std::vector<FBridgeRamp> Bridges;
	/** Lanes in the largest strongly connected component. */
	int GoodCount = 0;
};

/**
 * Builds the lane graph. Model: the street model; Graph: its road graph; Furniture: the approaches and signs; Area:
 * the area box lanes are clipped to (EdgeInset inside it). The heights are not filled in.
 */
FLaneGraph BuildLaneGraph(const FStreetModel& Model, const FRoadGraph& Graph, const FFurniture& Furniture,
						  const FBox& Area);

/** Every place that needs a road height: first the two ends of each bridge, then every lane point in lane order. */
std::vector<FStreetPoint> LaneHeightQueries(const FLaneGraph& Lanes);

/** Takes the heights for LaneHeightQueries (in the same order) and ramps the lanes of bridges. */
void ApplyLaneHeights(FLaneGraph& Lanes, const std::vector<double>& Heights);

/** Road surface height at every lane point from a sampler; bridges ramp between the heights at their ends. */
void AssignLaneHeights(FLaneGraph& Lanes, const std::function<double(double X, double Y)>& RoadHeight);

/** The lanes.json text (the graph as lane_builder.to_dict dumps it); the heights must be filled in. */
std::string LanesJsonText(const FLaneGraph& Lanes);

/** Writes lanes.json; returns false when the file can't be written. */
bool WriteLanesJson(const std::string& Path, const FLaneGraph& Lanes);
}
