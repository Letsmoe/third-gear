#pragma once

#include "CoreMinimal.h"

/** What a road lane's end at a junction demands of the vehicle (see Tools/osmimport/osmimport/lanes.py). */
enum class ELaneControl : uint8
{
	None,
	Signal,
	Priority,
	Equal,
	Yield,
	Stop,
};

enum class ELaneKind : uint8
{
	Road,
	Connection,
	UTurn,
};

/** A stop line along a lane that belongs to a signal approach of FTrafficNetwork. */
struct FLaneStopLine
{
	float SMeters = 0.f;
	int32 ApproachId = INDEX_NONE;
};

/** A connection that crosses or merges with another one inside a junction. Ranges are arc lengths along each lane. */
struct FLaneConflict
{
	int32 OtherLane = INDEX_NONE;
	float StartMeters = 0.f;
	float EndMeters = 0.f;
	float OtherStartMeters = 0.f;
	float OtherEndMeters = 0.f;
	/** 0 the owner has right of way, 1 it gives way by the right before left tie, 2 it gives way strictly (priority level or left turn). */
	int32 YieldKind = 0;
	bool Yields() const { return YieldKind > 0; }
};

/**
 * One lane of the AI traffic graph: a road lane (a way segment in one direction) or a connection through a junction.
 * Points are world positions in metres (x east, y south, z up), spaced about 1.5 m on roads and 0.75 m in junctions.
 */
struct FTrafficLane
{
	int32 Id = INDEX_NONE;
	ELaneKind Kind = ELaneKind::Road;
	int64 WayId = 0;
	float LimitKmh = 50.f;
	int32 Tier = 1;
	/** In the largest strongly connected part of the graph: routes and spawns stay on these, so no car gets stuck. */
	bool bGood = false;
	bool bRoundabout = false;
	TArray<FVector> Points;
	/** Arc length at every point, metres. */
	TArray<float> Arc;
	/** Speed that keeps the lateral acceleration comfortable at every point, m/s; empty when the limit alone applies. */
	TArray<float> CurveSpeedMs;
	TArray<int32> Next;

	// Road lanes
	ELaneControl Control = ELaneControl::None;
	TArray<FLaneStopLine> Stops;

	// Connections
	int32 FromLane = INDEX_NONE;
	int32 ToLane = INDEX_NONE;
	/** -1 left, 0 straight, +1 right. */
	int32 Turn = 0;
	TArray<FLaneConflict> Conflicts;

	float Length() const { return Arc.Num() > 0 ? Arc.Last() : 0.f; }
	float LimitMs() const { return LimitKmh / 3.6f; }
	bool IsConnection() const { return Kind != ELaneKind::Road; }
};

/**
 * The lane graph of a region (lanes.json beside world.json, built by Tools/osmimport/build_lanes.py), with position
 * queries along lanes. Pure data: it knows nothing about cars.
 */
class MAPRUNTIME_API FLaneNetwork
{
public:
	/** Reads lanes.json; false (with Error) when it is missing or malformed. */
	bool Load(const FString& Path, FString& Error);

	bool IsLoaded() const { return Lanes.Num() > 0; }
	const TArray<FTrafficLane>& GetLanes() const { return Lanes; }
	const FTrafficLane& GetLane(int32 LaneId) const { return Lanes[LaneId]; }

	/** Road lanes that are in the main loop of the graph: where cars may be placed. */
	const TArray<int32>& GetSpawnLanes() const { return SpawnLanes; }

	/** Position (metres) at arc length S along a lane, clamped to its ends. */
	FVector PositionAt(const FTrafficLane& Lane, float S) const;

	/** Unit direction of travel (horizontal) at arc length S along a lane. */
	FVector2D DirectionAt(const FTrafficLane& Lane, float S) const;

	/** Index of the last lane point at or before arc length S. */
	int32 FindPointIndex(const FTrafficLane& Lane, float S) const;

private:
	TArray<FTrafficLane> Lanes;
	TArray<int32> SpawnLanes;
};
