#pragma once

#include "CoreMinimal.h"
#include "LaneNetwork.h"

class FTrafficNetwork;

/** A vehicle that is not simulated here (the player's car) but that AI cars must see and yield to. Metres, world frame. */
struct FSimAgent
{
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D Forward = FVector2D(1.0, 0.0);
	FVector2D Velocity = FVector2D::ZeroVector;
	float HalfLengthM = 2.2f;
	float HalfWidthM = 0.95f;
};

/** One simulated car: its size, driver, route along the lane graph and everything it decided this step. */
struct FSimCar
{
	int32 Id = INDEX_NONE;

	// Shape, metres
	float LengthM = 4.4f;
	float WidthM = 1.85f;
	float WheelbaseM = 2.7f;
	float FrontOverhangM = 0.8f;
	float RearOverhangM = 0.9f;
	float WheelRadiusM = 0.31f;

	// Driver
	float DesiredSpeedFactor = 0.93f;
	float MaxAccelerationMs2 = 3.2f;
	float ComfortDecelerationMs2 = 2.2f;
	float TimeHeadwaySeconds = 1.3f;

	// Position on the lane graph. Route holds lane ids; the car's front axle is at S on Route[RouteIndex], and a few
	// lanes before it stay in the list so the rear axle can be placed.
	TArray<int32> Route;
	int32 RouteIndex = 0;
	float S = 0.f;
	float SpeedMs = 0.f;
	float AccelerationMs2 = 0.f;

	// Recomputed every step
	/** Distance from the front axle to the start of every lane of Route, metres (negative for the current lane). */
	TArray<float> LaneStart;
	FVector FrontAxleM = FVector::ZeroVector;
	FVector RearAxleM = FVector::ZeroVector;
	FVector2D Heading = FVector2D(1.0, 0.0);
	float PitchRadians = 0.f;
	float SteerRadians = 0.f;
	float WheelRollRadians = 0.f;

	// Behaviour state
	float SecondsAlive = 0.f;
	float StoppedSeconds = 0.f;
	float WaitSeconds = 0.f;
	float StopHoldSeconds = 0.f;
	bool bStopSatisfied = false;
	int32 ClaimedConnection = INDEX_NONE;
	/** Signal the car has decided to stop for; it keeps that decision until the light is green, even though speeding up changes the sums. */
	int32 LatchedStopApproach = INDEX_NONE;
	bool bBrakeLight = false;
	/** -1 left, 0 none, +1 right. */
	int32 TurnSignal = 0;
	bool bDeadlockReported = false;
	bool bLongWaitLogged = false;
	/** Why the car is held back right now, for the logs. */
	const TCHAR* BlockReason = TEXT("");
	int32 BlockerId = INDEX_NONE;
	/** The last reason the car had while standing still, which outlives the moment it starts again. */
	const TCHAR* StopReason = TEXT("");
	int32 StopBlockerId = INDEX_NONE;
	/** Every obstacle the car reacted to in its last step while it was held up, for stuck-car logs. */
	FString ObstacleSummary;

	/** Centre of the body in the world, metres. */
	FVector2D BodyCenter() const;
};

/** Counters the tests read. */
struct FAITrafficStats
{
	int32 Spawned = 0;
	int32 Removed = 0;
	/** Overlaps between two AI cars (each pair counted once per contact). */
	int32 Collisions = 0;
	/** Overlaps between an AI car and an outside vehicle such as the player's car. */
	int32 AgentContacts = 0;
	/** Cars that stood still for more than a minute for a reason other than a red light or the player (a fault). */
	int32 Deadlocks = 0;
	/** Cars that stood still for more than a minute, whatever the reason (a long red light counts). */
	int32 LongWaits = 0;
	double DistanceDrivenM = 0.0;
	double CarSeconds = 0.0;
};

/**
 * The driving of AI cars along the lane graph, without any Unreal objects: routes, car following (intelligent driver
 * model), signals, stop and give way lines, right of way inside junctions through the conflicts of the lane graph,
 * and avoiding anything in the way, including the player's car. Cars move kinematically: a rear axle follows the lane
 * and the front axle a wheelbase ahead of it, so turns look right without a physics step.
 */
class MAPRUNTIME_API FAITrafficSimulation
{
public:
	void Initialize(const FLaneNetwork* InLanes, const FTrafficNetwork* InSignals, int32 Seed);

	/** Adds a car at arc length S of a road lane, moving at SpeedMs; picks its route. Returns it. */
	FSimCar* AddCar(const FSimCar& Template, int32 LaneId, float S, float SpeedMs);

	void RemoveCar(int32 CarId);
	FSimCar* FindCar(int32 CarId);
	const TArray<TSharedPtr<FSimCar>>& GetCars() const { return Cars; }

	/** True when no car is within DistanceM (horizontal) of Location. */
	bool IsAreaFree(const FVector& LocationM, float DistanceM) const;

	/** Advances all cars by DeltaSeconds at traffic time TrafficTimeSeconds. Substeps internally when the frame is long. */
	void Step(float DeltaSeconds, double TrafficTimeSeconds, const TArray<FSimAgent>& Agents);

	const FAITrafficStats& GetStats() const { return Stats; }

	/** Console-friendly description of what holds a car back, for debugging stuck cars. */
	FString Describe(const FSimCar& Car) const;

private:
	/** Something ahead the car has to react to: a car in front, a stop line, a speed change. */
	struct FObstacle
	{
		float GapM = 0.f;
		float SpeedMs = 0.f;
		float MinGapM = 0.f;
		float HeadwaySeconds = 0.f;
		/** What the obstacle is, for the logs. */
		const TCHAR* Reason = TEXT("");
		/** The car or agent behind it, if there is one. */
		int32 OtherId = INDEX_NONE;
	};

	/** A car's footprint on one lane, in that lane's arc length (may reach beyond both ends). */
	struct FOccupant
	{
		FSimCar* Car = nullptr;
		float RearS = 0.f;
		float FrontS = 0.f;
	};

	/** A car that has the connection on its route, and where in the route. */
	struct FClaim
	{
		FSimCar* Car = nullptr;
		int32 RouteSlot = 0;
	};

	using FObstacleList = TArray<FObstacle, TInlineAllocator<48>>;

	/** One physics step of every car: decide, move, update poses, look for contacts and stuck cars. */
	void StepOnce(float DeltaSeconds, double TrafficTimeSeconds, const TArray<FSimAgent>& Agents);

	/** Recomputes where the car's lanes start relative to its front axle, and its axle positions, heading, pitch and steering. */
	void UpdateGeometry(FSimCar& Car) const;

	/** Keeps a few lanes of route ahead of the car and drops the lanes far behind it. */
	void ExtendRoute(FSimCar& Car);

	/** Picks the lane after Lane at random, weighted by turn and road importance, preferring the main loop of the graph. */
	int32 ChooseNextLane(const FSimCar& Car, const FTrafficLane& Lane);

	/** World position (metres) of the point on the route that is OffsetFromFrontAxleM along it (negative behind the front axle). */
	FVector PointOnRoute(const FSimCar& Car, float OffsetFromFrontAxleM) const;

	/** Registers every car on the lanes its body covers, and on the connections it is going to take. */
	void BuildOccupancy();

	/** The acceleration the car's driver chooses: free road, car following and every obstacle ahead combined. */
	float ComputeAcceleration(FSimCar& Car, double TrafficTimeSeconds, const TArray<FSimAgent>& Agents, float DeltaSeconds);

	/** The speed the driver wants where the front of the car is: the lane's limit times the driver's factor. */
	float DesiredSpeed(const FSimCar& Car) const;

	/** Walks the route ahead and collects speed changes, curves, signals and junction entries as obstacles. */
	void ScanRoute(FSimCar& Car, double TrafficTimeSeconds, float DeltaSeconds, const TArray<FSimAgent>& Agents, FObstacleList& Obstacles);

	/** Adds the curve speeds of one lane that are slower than the lane's limit. */
	void ScanLaneCurves(const FSimCar& Car, int32 Slot, const FTrafficLane& Lane, float Horizon, FObstacleList& Obstacles) const;

	/** Adds a stop obstacle at every stop line of the lane whose signal the car has to stop for. */
	void ScanLaneSignals(FSimCar& Car, int32 Slot, const FTrafficLane& Lane, double TrafficTimeSeconds, FObstacleList& Obstacles) const;

	/** Whether a car GapM from the stop line at SpeedMs has to stop for the signal, now or at its predicted state on arrival. */
	bool MustStopForSignal(float GapM, float SpeedMs, int32 ApproachId, double TrafficTimeSeconds) const;

	/** Handles the end of a road lane before a junction: stop sign hold, slow approach, and waiting for right of way and room. */
	void ScanJunctionEntry(FSimCar& Car, int32 Slot, float GapToLineM, float DeltaSeconds, FObstacleList& Obstacles, const TArray<FSimAgent>& Agents);

	/** Whether the car has to wait before taking the connection after the lane in Slot, and why (Reason, and the car it waits for). */
	bool IsConnectionBlocked(FSimCar& Car, int32 Slot, const TArray<FSimAgent>& Agents, const TCHAR*& Reason, int32& BlockerId) const;

	/** Whether the lane after Connection has no room for the car yet (a queue reaches back to its start). */
	bool IsExitCongested(const FSimCar& Car, const FTrafficLane& Connection, int32& BlockerId) const;

	/** Whether an outside vehicle is, or is predicted to be, on the connection while the car would be. */
	bool IsBlockedByAgent(const FSimCar& Car, const FTrafficLane& Connection, float EnterSeconds, float ExitSeconds, const TArray<FSimAgent>& Agents) const;

	/** Whether a car on the other connection of the conflict would be in the shared area while this car would be. */
	bool ConflictBlocks(const FSimCar& Car, const FLaneConflict& Conflict, float MyEnterSeconds, float MyExitSeconds, int32& BlockerId) const;

	/** Adds the nearest car ahead on the route as an obstacle that is followed at a time headway. */
	void AddLeadCars(const FSimCar& Car, FObstacleList& Obstacles) const;

	/** Adds the first thing in the car's path among outside vehicles and cars not on its lanes (crossing traffic). */
	void AddAgentObstacles(const FSimCar& Car, const TArray<FSimAgent>& Agents, FObstacleList& Obstacles) const;

	/** Sets the indicator for a turn coming up within 35 m or under way. */
	void UpdateSignals(FSimCar& Car) const;

	/** Moves the car along its route by the acceleration over DeltaSeconds and switches to the next lane at a lane end. */
	void Advance(FSimCar& Car, float DeltaSeconds, float Acceleration);

	/** Counts overlaps between AI cars and with outside vehicles, once per contact. */
	void DetectContacts(const TArray<FSimAgent>& Agents);

	/** Whether a car that has stood still for a minute is waiting for something that is not a fault (a red light, the player). */
	bool IsLegitimateWait(const FSimCar& Car);

	/** Time to cover DistanceM from SpeedMs, accelerating at AccelerationMs2 up to MaxSpeedMs. */
	static float TimeToCover(float DistanceM, float SpeedMs, float AccelerationMs2, float MaxSpeedMs);

	/** Whether Other occupies a lane of the car's route within sight, so car following already accounts for it. */
	bool IsOnMyLanes(const FSimCar& Car, const FSimCar& Other) const;

	const FLaneNetwork* Lanes = nullptr;
	const FTrafficNetwork* Signals = nullptr;
	FRandomStream Random;
	TArray<TSharedPtr<FSimCar>> Cars;
	int32 NextCarId = 1;
	TMap<int32, TArray<FOccupant>> Occupancy;
	TMap<int32, TArray<FClaim>> Claims;
	TSet<uint64> ContactPairs;
	TSet<uint64> AgentContactPairs;
	FAITrafficStats Stats;
};
