#pragma once

#include "CoreMinimal.h"

/** What a signal head shows. RedAmber is the one second before green. */
enum class ESignalAspect : uint8
{
	Red,
	RedAmber,
	Green,
	Amber,
};

/** One phase of a junction's cycle; times in seconds. A phase without approaches serves what the data doesn't show. */
struct FSignalPhase
{
	float GreenSeconds = 0.f;
	float AmberSeconds = 0.f;
	float ClearanceSeconds = 0.f;
	bool bUnserved = false;
	/** Seconds from the start of the cycle where this phase's green begins. */
	float StartSeconds = 0.f;

	float TotalSeconds() const { return GreenSeconds + AmberSeconds + ClearanceSeconds; }
};

/** One direction of travel into a signalised junction, with its stop line. Positions in cm, world frame. */
struct FTrafficApproach
{
	int32 Id = INDEX_NONE;
	int32 JunctionIndex = INDEX_NONE;
	int32 Phase = 0;
	int64 WayId = 0;
	/** Both ends of the stop line, left end first as seen by the driver. */
	FVector2D StopLineLeft = FVector2D::ZeroVector;
	FVector2D StopLineRight = FVector2D::ZeroVector;
	/** Unit direction of travel across the stop line. */
	FVector2D Direction = FVector2D::ZeroVector;
	int32 Lanes = 1;
	float SpeedKmh = 50.f;

	FVector2D StopLineCenter() const { return (StopLineLeft + StopLineRight) * 0.5; }
};

struct FSignalJunction
{
	int32 Id = INDEX_NONE;
	FVector2D Center = FVector2D::ZeroVector;
	bool bCrossingOnly = false;
	TArray<FSignalPhase> Phases;
	TArray<int32> ApproachIds;
	float CycleSeconds = 1.f;
	/** Shifts the cycle so neighbouring junctions are not in step; derived from the junction id, so it is deterministic. */
	float OffsetSeconds = 0.f;
};

/** A road centre line with its legal speed, for the speed limit query. */
struct FSpeedWay
{
	int64 Id = 0;
	/** km/h; 0 means no limit. */
	float LimitKmh = 50.f;
	float WidthCm = 600.f;
	bool bOneway = false;
	TArray<FVector2D> Points;
};

/** The state of the signal that controls an approach at some time. */
struct FSignalState
{
	ESignalAspect Aspect = ESignalAspect::Red;
	/** Seconds until the aspect changes. */
	float SecondsUntilChange = 0.f;
	/** Seconds since the aspect last changed. */
	float SecondsInState = 0.f;
};

/** Result of looking for the signal ahead of a vehicle. */
struct FApproachQuery
{
	int32 ApproachId = INDEX_NONE;
	/** Distance along the direction of travel to the stop line (negative once the front axle is past it), cm. */
	float DistanceToStopLineCm = 0.f;
	FSignalState State;
};

/** Result of a speed limit lookup. */
struct FSpeedLimitResult
{
	bool bFound = false;
	/** km/h; 0 means no limit. */
	float LimitKmh = 0.f;
	int64 WayId = 0;
};

/**
 * The signal junctions, stop lines, phases and speed limits of a region (traffic.json beside world.json), with the
 * queries other systems use: the rule checker today, AI traffic next. Pure data and functions of time: the same
 * network and time always give the same answer, so a vehicle can predict a signal ahead of it.
 */
class MAPRUNTIME_API FTrafficNetwork
{
public:
	/** Reads traffic.json; false (with Error) when it is missing or malformed. */
	bool Load(const FString& Path, FString& Error);

	bool IsLoaded() const { return bLoaded; }

	const TArray<FSignalJunction>& GetJunctions() const { return Junctions; }
	const TArray<FTrafficApproach>& GetApproaches() const { return Approaches; }
	const FTrafficApproach* FindApproachById(int32 ApproachId) const;

	/** Aspect and time to the next change of the signal controlling an approach, at TimeSeconds of traffic time. */
	FSignalState GetApproachState(int32 ApproachId, double TimeSeconds) const;

	/**
	 * The signal a vehicle at Location (cm, world xy) heading Forward (unit vector) is approaching: the nearest stop
	 * line ahead within MaxDistanceCm (or just behind it, up to BehindCm) that points the same way and spans the
	 * vehicle's lateral position.
	 */
	bool FindApproachAhead(const FVector2D& Location, const FVector2D& Forward, float MaxDistanceCm, float BehindCm, double TimeSeconds,
		FApproachQuery& Out) const;

	/** The legal speed limit on the road at Location for a vehicle heading Forward. */
	FSpeedLimitResult GetSpeedLimit(const FVector2D& Location, const FVector2D& Forward) const;

	const TArray<FSpeedWay>& GetWays() const { return Ways; }

private:
	static FSignalState StateOfPhase(const FSignalJunction& Junction, int32 PhaseIndex, double TimeSeconds);
	void BuildIndices();
	static int64 CellKey(int32 CellX, int32 CellY) { return (int64(CellX) << 32) ^ int64(uint32(CellY)); }

	bool bLoaded = false;
	TArray<FSignalJunction> Junctions;
	TArray<FTrafficApproach> Approaches;
	TArray<FSpeedWay> Ways;
	/** Approach and way segment indices by spatial cell. */
	TMap<int64, TArray<int32>> ApproachCells;
	TMap<int64, TArray<int64>> SegmentCells;
	TMap<int32, int32> ApproachById;
};
