#include "AITrafficSimulation.h"

#include "TrafficNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogAITrafficSim, Log, All);

namespace
{
constexpr float MaxBrakingMs2 = 9.f;
constexpr float HorizonMinimumM = 70.f;
constexpr float HorizonMaximumM = 140.f;
constexpr float JunctionConsiderM = 40.f;
/** Speed at the line of a give way sign, and of a junction where right before left applies. */
constexpr float YieldApproachSpeedMs = 5.0f;
constexpr float EqualApproachSpeedMs = 7.0f;
constexpr float StopLineMarginM = 0.2f;
constexpr float StopLineMinGapM = 1.0f;
constexpr float CarMinGapM = 2.0f;
constexpr float ConflictMarginSeconds = 0.8f;
constexpr float JunctionAccelerationMs2 = 2.0f;
constexpr float JunctionMaxSpeedMs = 7.f;
constexpr float PatientSeconds = 25.f;
constexpr float CongestionPatientSeconds = 90.f;
constexpr float DeadlockSeconds = 60.f;
constexpr float LongWaitLogSeconds = 40.f;
/** A red light lasts longer than a minute at the biggest junctions; a car waiting longer than this has a problem. */
constexpr float SignalWaitLimitSeconds = 130.f;
constexpr float AgentLookAheadM = 35.f;
constexpr float AgentCheckRangeM = 50.f;
constexpr float MaxSubstepSeconds = 0.05f;
constexpr int32 RouteLanesAhead = 6;
constexpr int32 RouteLanesBehind = 3;
constexpr float ComfortStopDecelerationMs2 = 3.0f;
constexpr float HardStopDecelerationMs2 = 6.0f;
constexpr float AmberStopDecelerationMs2 = 3.5f;
constexpr float AmberReactionSeconds = 0.7f;

float Square(float Value)
{
	return Value * Value;
}

/** Whether two oriented rectangles overlap (separating axis test). */
bool RectanglesOverlap(const FVector2D& CenterA, const FVector2D& ForwardA, float HalfLengthA, float HalfWidthA,
	const FVector2D& CenterB, const FVector2D& ForwardB, float HalfLengthB, float HalfWidthB)
{
	const FVector2D RightA(-ForwardA.Y, ForwardA.X);
	const FVector2D RightB(-ForwardB.Y, ForwardB.X);
	const FVector2D Axes[4] = {ForwardA, RightA, ForwardB, RightB};
	const FVector2D Offset = CenterB - CenterA;
	for (const FVector2D& Axis : Axes)
	{
		const float RadiusA = HalfLengthA * FMath::Abs(float(FVector2D::DotProduct(ForwardA, Axis))) + HalfWidthA * FMath::Abs(float(FVector2D::DotProduct(RightA, Axis)));
		const float RadiusB = HalfLengthB * FMath::Abs(float(FVector2D::DotProduct(ForwardB, Axis))) + HalfWidthB * FMath::Abs(float(FVector2D::DotProduct(RightB, Axis)));
		if (FMath::Abs(float(FVector2D::DotProduct(Offset, Axis))) > RadiusA + RadiusB)
		{
			return false;
		}
	}
	return true;
}

/** Whether a point lies inside an oriented rectangle grown by Margin. */
bool PointInRectangle(const FVector2D& Point, const FVector2D& Center, const FVector2D& Forward, float HalfLength, float HalfWidth)
{
	const FVector2D Offset = Point - Center;
	const FVector2D Right(-Forward.Y, Forward.X);
	return FMath::Abs(float(FVector2D::DotProduct(Offset, Forward))) <= HalfLength && FMath::Abs(float(FVector2D::DotProduct(Offset, Right))) <= HalfWidth;
}
}

FVector2D FSimCar::BodyCenter() const
{
	const FVector2D Middle = (FVector2D(FrontAxleM) + FVector2D(RearAxleM)) * 0.5;
	return Middle + Heading * (FrontOverhangM - RearOverhangM) * 0.5;
}

void FAITrafficSimulation::Initialize(const FLaneNetwork* InLanes, const FTrafficNetwork* InSignals, int32 Seed)
{
	Lanes = InLanes;
	Signals = InSignals;
	Random.Initialize(Seed);
	Cars.Reset();
	Stats = FAITrafficStats();
}

FSimCar* FAITrafficSimulation::AddCar(const FSimCar& Template, int32 LaneId, float S, float SpeedMs)
{
	TSharedPtr<FSimCar> Car = MakeShared<FSimCar>(Template);
	Car->Id = NextCarId++;
	Car->Route.Reset();
	Car->Route.Add(LaneId);
	Car->RouteIndex = 0;
	Car->S = S;
	Car->SpeedMs = SpeedMs;
	Car->DesiredSpeedFactor = Random.FRandRange(0.84f, 0.97f);
	Car->MaxAccelerationMs2 = Random.FRandRange(2.8f, 4.0f);
	Car->ComfortDecelerationMs2 = Random.FRandRange(1.9f, 2.6f);
	Car->TimeHeadwaySeconds = Random.FRandRange(1.1f, 1.6f);
	ExtendRoute(*Car);
	UpdateGeometry(*Car);
	Cars.Add(Car);
	++Stats.Spawned;
	return Car.Get();
}

void FAITrafficSimulation::RemoveCar(int32 CarId)
{
	const int32 Removed = Cars.RemoveAll([CarId](const TSharedPtr<FSimCar>& Car) { return Car->Id == CarId; });
	Stats.Removed += Removed;
}

FSimCar* FAITrafficSimulation::FindCar(int32 CarId)
{
	for (const TSharedPtr<FSimCar>& Car : Cars)
	{
		if (Car->Id == CarId)
		{
			return Car.Get();
		}
	}
	return nullptr;
}

bool FAITrafficSimulation::IsAreaFree(const FVector& LocationM, float DistanceM) const
{
	for (const TSharedPtr<FSimCar>& Car : Cars)
	{
		if (FVector2D::Distance(FVector2D(Car->FrontAxleM), FVector2D(LocationM)) < DistanceM)
		{
			return false;
		}
	}
	return true;
}

float FAITrafficSimulation::TimeToCover(float DistanceM, float SpeedMs, float AccelerationMs2, float MaxSpeedMs)
{
	if (DistanceM <= 0.f)
	{
		return 0.f;
	}
	SpeedMs = FMath::Max(SpeedMs, 0.f);
	MaxSpeedMs = FMath::Max(MaxSpeedMs, SpeedMs);
	if (AccelerationMs2 <= 1e-3f || SpeedMs >= MaxSpeedMs)
	{
		return DistanceM / FMath::Max(SpeedMs, 0.5f);
	}
	const float AccelerationDistance = (Square(MaxSpeedMs) - Square(SpeedMs)) / (2.f * AccelerationMs2);
	if (DistanceM <= AccelerationDistance)
	{
		return (FMath::Sqrt(Square(SpeedMs) + 2.f * AccelerationMs2 * DistanceM) - SpeedMs) / AccelerationMs2;
	}
	return (MaxSpeedMs - SpeedMs) / AccelerationMs2 + (DistanceM - AccelerationDistance) / MaxSpeedMs;
}

// ---------------------------------------------------------------------------------------------------------------- route

int32 FAITrafficSimulation::ChooseNextLane(const FSimCar& Car, const FTrafficLane& Lane)
{
	TArray<int32> Candidates;
	TArray<float> Weights;
	float Total = 0.f;
	const auto Consider = [&](bool bGoodOnly)
	{
		for (const int32 NextId : Lane.Next)
		{
			const FTrafficLane& Next = Lanes->GetLane(NextId);
			if (bGoodOnly && !Next.bGood)
			{
				continue;
			}
			float Weight = 1.f;
			if (Next.IsConnection())
			{
				Weight = Next.Kind == ELaneKind::UTurn ? 0.05f : (Next.Turn == 0 ? 5.f : (Next.Turn > 0 ? 2.5f : 1.5f));
				const FTrafficLane& Target = Lanes->GetLane(Next.ToLane);
				static const float TierWeights[5] = {0.25f, 1.f, 1.6f, 2.2f, 2.5f};
				Weight *= TierWeights[FMath::Clamp(Target.Tier, 0, 4)];
			}
			Candidates.Add(NextId);
			Weights.Add(Weight);
			Total += Weight;
		}
	};
	Consider(true);
	if (Candidates.IsEmpty())
	{
		Consider(false);
	}
	if (Candidates.IsEmpty())
	{
		return INDEX_NONE;
	}
	float Pick = Random.FRand() * Total;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		Pick -= Weights[Index];
		if (Pick <= 0.f)
		{
			return Candidates[Index];
		}
	}
	return Candidates.Last();
}

void FAITrafficSimulation::ExtendRoute(FSimCar& Car)
{
	while (Car.Route.Num() - Car.RouteIndex < RouteLanesAhead)
	{
		const int32 Next = ChooseNextLane(Car, Lanes->GetLane(Car.Route.Last()));
		if (Next == INDEX_NONE)
		{
			break;
		}
		Car.Route.Add(Next);
	}
	while (Car.RouteIndex > RouteLanesBehind)
	{
		Car.Route.RemoveAt(0);
		--Car.RouteIndex;
	}
}

FVector FAITrafficSimulation::PointOnRoute(const FSimCar& Car, float OffsetFromFrontAxleM) const
{
	const int32 Count = Car.Route.Num();
	for (int32 Slot = 0; Slot < Count; ++Slot)
	{
		const FTrafficLane& Lane = Lanes->GetLane(Car.Route[Slot]);
		const float Start = Car.LaneStart[Slot];
		if (OffsetFromFrontAxleM <= Start + Lane.Length() || Slot == Count - 1)
		{
			if (Slot == 0 && OffsetFromFrontAxleM < Start)
			{
				const FVector2D Direction = Lanes->DirectionAt(Lane, 0.f);
				return Lanes->PositionAt(Lane, 0.f) - FVector(Direction.X, Direction.Y, 0.0) * (Start - OffsetFromFrontAxleM);
			}
			if (Slot == Count - 1 && OffsetFromFrontAxleM > Start + Lane.Length())
			{
				const FVector2D Direction = Lanes->DirectionAt(Lane, Lane.Length());
				return Lanes->PositionAt(Lane, Lane.Length()) + FVector(Direction.X, Direction.Y, 0.0) * (OffsetFromFrontAxleM - Start - Lane.Length());
			}
			return Lanes->PositionAt(Lane, OffsetFromFrontAxleM - Start);
		}
	}
	return FVector::ZeroVector;
}

void FAITrafficSimulation::UpdateGeometry(FSimCar& Car) const
{
	const int32 Count = Car.Route.Num();
	Car.LaneStart.SetNum(Count);
	Car.LaneStart[Car.RouteIndex] = -Car.S;
	for (int32 Slot = Car.RouteIndex + 1; Slot < Count; ++Slot)
	{
		Car.LaneStart[Slot] = Car.LaneStart[Slot - 1] + Lanes->GetLane(Car.Route[Slot - 1]).Length();
	}
	for (int32 Slot = Car.RouteIndex - 1; Slot >= 0; --Slot)
	{
		Car.LaneStart[Slot] = Car.LaneStart[Slot + 1] - Lanes->GetLane(Car.Route[Slot]).Length();
	}
	Car.FrontAxleM = PointOnRoute(Car, 0.f);
	Car.RearAxleM = PointOnRoute(Car, -Car.WheelbaseM);
	const FVector2D Body = FVector2D(Car.FrontAxleM - Car.RearAxleM);
	const FTrafficLane& Lane = Lanes->GetLane(Car.Route[Car.RouteIndex]);
	const FVector2D PathDirection = Lanes->DirectionAt(Lane, Car.S);
	Car.Heading = Body.SizeSquared() > 1e-6 ? Body.GetSafeNormal() : PathDirection;
	Car.PitchRadians = FMath::Atan2(float(Car.FrontAxleM.Z - Car.RearAxleM.Z), Car.WheelbaseM);
	const float Cross = float(Car.Heading.X * PathDirection.Y - Car.Heading.Y * PathDirection.X);
	const float Dot = float(FVector2D::DotProduct(Car.Heading, PathDirection));
	Car.SteerRadians = FMath::Clamp(FMath::Atan2(Cross, Dot), -0.6f, 0.6f);
}

// ------------------------------------------------------------------------------------------------------------- occupancy

void FAITrafficSimulation::BuildOccupancy()
{
	Occupancy.Reset();
	Claims.Reset();
	for (const TSharedPtr<FSimCar>& CarPtr : Cars)
	{
		FSimCar& Car = *CarPtr;
		const float Rear = -(Car.WheelbaseM + Car.RearOverhangM);
		const float Front = Car.FrontOverhangM;
		const int32 First = FMath::Max(Car.RouteIndex - RouteLanesBehind, 0);
		const int32 Last = FMath::Min(Car.RouteIndex + 2, Car.Route.Num() - 1);
		for (int32 Slot = First; Slot <= Last; ++Slot)
		{
			const float Start = Car.LaneStart[Slot];
			const float End = Start + Lanes->GetLane(Car.Route[Slot]).Length();
			if (Front >= Start - 0.01f && Rear <= End + 0.01f)
			{
				Occupancy.FindOrAdd(Car.Route[Slot]).Add({&Car, Rear - Start, Front - Start});
			}
		}
		const int32 ClaimFirst = FMath::Max(Car.RouteIndex - 1, 0);
		const int32 ClaimLast = FMath::Min(Car.RouteIndex + RouteLanesAhead - 1, Car.Route.Num() - 1);
		for (int32 Slot = ClaimFirst; Slot <= ClaimLast; ++Slot)
		{
			if (Lanes->GetLane(Car.Route[Slot]).IsConnection())
			{
				Claims.FindOrAdd(Car.Route[Slot]).Add({&Car, Slot});
			}
		}
	}
}

bool FAITrafficSimulation::IsOnMyLanes(const FSimCar& Car, const FSimCar& Other) const
{
	// Cars that occupy a lane of my route within sight are seen by the lane logic (car ahead); the rest need the geometric check.
	for (int32 Slot = Car.RouteIndex; Slot < Car.Route.Num(); ++Slot)
	{
		if (Car.LaneStart[Slot] > AgentLookAheadM)
		{
			break;
		}
		const TArray<FOccupant>* Occupants = Occupancy.Find(Car.Route[Slot]);
		if (!Occupants)
		{
			continue;
		}
		const float MyFront = Car.FrontOverhangM - Car.LaneStart[Slot];
		for (const FOccupant& Occupant : *Occupants)
		{
			if (Occupant.Car == &Other && Occupant.RearS >= MyFront - 0.01f)
			{
				return true;    // ahead on the lane: car following handles it (a car behind us on a closed loop is not)
			}
		}
	}
	return false;
}

void FAITrafficSimulation::AddLeadCars(const FSimCar& Car, FObstacleList& Obstacles) const
{
	float BestGap = TNumericLimits<float>::Max();
	float BestSpeed = 0.f;
	int32 BestId = INDEX_NONE;
	for (int32 Slot = Car.RouteIndex; Slot < Car.Route.Num(); ++Slot)
	{
		if (Car.LaneStart[Slot] - Car.FrontOverhangM > HorizonMaximumM)
		{
			break;
		}
		const TArray<FOccupant>* Occupants = Occupancy.Find(Car.Route[Slot]);
		if (!Occupants)
		{
			continue;
		}
		const float MyFront = Car.FrontOverhangM - Car.LaneStart[Slot];
		for (const FOccupant& Other : *Occupants)
		{
			if (Other.Car == &Car || Other.RearS < MyFront - 0.01f)
			{
				continue;
			}
			const float Gap = Other.RearS - MyFront;
			if (Gap < BestGap)
			{
				BestGap = Gap;
				BestSpeed = Other.Car->SpeedMs;
				BestId = Other.Car->Id;
			}
		}
	}
	if (BestGap < TNumericLimits<float>::Max())
	{
		Obstacles.Add({FMath::Max(BestGap, 0.05f), BestSpeed, CarMinGapM, Car.TimeHeadwaySeconds, TEXT("car ahead"), BestId});
	}
}

void FAITrafficSimulation::AddAgentObstacles(const FSimCar& Car, const TArray<FSimAgent>& Agents, FObstacleList& Obstacles) const
{
	struct FTarget
	{
		FVector2D Center;
		FVector2D Forward;
		FVector2D Velocity;
		float HalfLength;
		float HalfWidth;
		int32 Id;
		/** Whether to also test where it will be when the car gets there: outside vehicles aren't bound by the lanes. */
		bool bPredict;
	};
	TArray<FTarget, TInlineAllocator<8>> Targets;
	const FVector2D MyPosition(Car.FrontAxleM);
	for (const FSimAgent& Agent : Agents)
	{
		if (FVector2D::Distance(Agent.Position, MyPosition) < AgentCheckRangeM)
		{
			Targets.Add({Agent.Position, Agent.Forward, Agent.Velocity, Agent.HalfLengthM, Agent.HalfWidthM, -1, true});
		}
	}
	for (const TSharedPtr<FSimCar>& OtherPtr : Cars)
	{
		const FSimCar& Other = *OtherPtr;
		if (&Other == &Car || FVector2D::Distance(FVector2D(Other.FrontAxleM), MyPosition) > AgentCheckRangeM || IsOnMyLanes(Car, Other))
		{
			continue;
		}
		Targets.Add({Other.BodyCenter(), Other.Heading, Other.Heading * Other.SpeedMs, Other.LengthM * 0.5f, Other.WidthM * 0.5f, Other.Id, false});
	}
	if (Targets.IsEmpty())
	{
		return;
	}
	const float MyHalfWidth = Car.WidthM * 0.5f;
	FVector2D Previous(PointOnRoute(Car, Car.FrontOverhangM));
	for (float Distance = 0.f; Distance <= AgentLookAheadM; Distance += 1.f)
	{
		const FVector2D Point(PointOnRoute(Car, Car.FrontOverhangM + Distance));
		const FVector2D PathDirection = (Point - Previous).GetSafeNormal();
		Previous = Point;
		const float ArrivalSeconds = FMath::Min(Distance / FMath::Max(Car.SpeedMs, 2.f), 4.f);
		for (const FTarget& Target : Targets)
		{
			const bool bHitsNow = PointInRectangle(Point, Target.Center, Target.Forward, Target.HalfLength + 0.3f, Target.HalfWidth + MyHalfWidth + 0.15f);
			const bool bHitsLater = Target.bPredict && Target.Velocity.SizeSquared() > 1.0
				&& PointInRectangle(Point, Target.Center + Target.Velocity * ArrivalSeconds, Target.Forward, Target.HalfLength + 0.3f, Target.HalfWidth + MyHalfWidth + 0.15f);
			if (!bHitsNow && !bHitsLater)
			{
				continue;
			}
			const float AlongSpeed = FMath::Max(float(FVector2D::DotProduct(Target.Velocity, PathDirection)), 0.f);
			Obstacles.Add({FMath::Max(Distance, 0.05f), AlongSpeed, CarMinGapM, 0.5f, TEXT("vehicle in the way"), Target.Id});
			return;
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------- scanning

float FAITrafficSimulation::DesiredSpeed(const FSimCar& Car) const
{
	int32 Slot = Car.RouteIndex;
	while (Slot + 1 < Car.Route.Num() && Car.FrontOverhangM > Car.LaneStart[Slot + 1])
	{
		++Slot;
	}
	return FMath::Max(Lanes->GetLane(Car.Route[Slot]).LimitMs() * Car.DesiredSpeedFactor, 2.f);
}

void FAITrafficSimulation::ScanLaneCurves(const FSimCar& Car, int32 Slot, const FTrafficLane& Lane, float Horizon, FObstacleList& Obstacles) const
{
	if (Lane.CurveSpeedMs.IsEmpty())
	{
		return;
	}
	const float LaneCap = Lane.LimitMs() * Car.DesiredSpeedFactor;
	const float Start = Car.LaneStart[Slot];
	float LastGap = -100.f;
	for (int32 Index = 0; Index < Lane.Points.Num(); ++Index)
	{
		const float Gap = Start + Lane.Arc[Index];
		if (Gap < -0.5f)
		{
			continue;
		}
		if (Gap > Horizon)
		{
			break;
		}
		const float Speed = Lane.CurveSpeedMs[Index];
		if (Speed >= LaneCap - 0.3f || Gap - LastGap < 2.5f)
		{
			continue;
		}
		LastGap = Gap;
		Obstacles.Add({FMath::Max(Gap, 0.05f), Speed, 0.f, 0.f, TEXT("curve")});
	}
}

bool FAITrafficSimulation::MustStopForSignal(float GapM, float SpeedMs, int32 ApproachId, double TrafficTimeSeconds) const
{
	const FSignalState Now = Signals->GetApproachState(ApproachId, TrafficTimeSeconds);
	// The same fairness test the rule checker applies: from the first sight of amber, a driver who reacts in
	// AmberReactionSeconds and brakes with AmberStopDecelerationMs2 has to be able to stop before the line.
	const bool bCouldStopFairly = GapM >= SpeedMs * AmberReactionSeconds + Square(SpeedMs) / (2.f * AmberStopDecelerationMs2);
	const bool bCanStopComfortably = GapM >= Square(SpeedMs) / (2.f * ComfortStopDecelerationMs2);
	const bool bCanStopHard = GapM >= Square(SpeedMs) / (2.f * HardStopDecelerationMs2);
	switch (Now.Aspect)
	{
	case ESignalAspect::Red:
	case ESignalAspect::RedAmber:
		return true;
	case ESignalAspect::Amber:
		return bCouldStopFairly || bCanStopComfortably;
	case ESignalAspect::Green:
		break;
	}
	const float ArrivalSeconds = GapM / FMath::Max(SpeedMs, 1.f);
	const FSignalState AtArrival = Signals->GetApproachState(ApproachId, TrafficTimeSeconds + ArrivalSeconds);
	if (AtArrival.Aspect == ESignalAspect::Red || AtArrival.Aspect == ESignalAspect::RedAmber)
	{
		return bCanStopComfortably || bCanStopHard;
	}
	return false;
}

void FAITrafficSimulation::ScanLaneSignals(FSimCar& Car, int32 Slot, const FTrafficLane& Lane, double TrafficTimeSeconds, FObstacleList& Obstacles) const
{
	for (const FLaneStopLine& Stop : Lane.Stops)
	{
		const float Gap = Car.LaneStart[Slot] + Stop.SMeters - Car.FrontOverhangM;
		if (Gap < -0.3f)
		{
			continue;
		}
		bool bStop = false;
		if (Car.LatchedStopApproach == Stop.ApproachId)
		{
			bStop = Signals->GetApproachState(Stop.ApproachId, TrafficTimeSeconds).Aspect != ESignalAspect::Green;
			Car.LatchedStopApproach = bStop ? Stop.ApproachId : INDEX_NONE;
		}
		else
		{
			bStop = MustStopForSignal(Gap, Car.SpeedMs, Stop.ApproachId, TrafficTimeSeconds);
			Car.LatchedStopApproach = bStop ? Stop.ApproachId : Car.LatchedStopApproach;
		}
		if (bStop)
		{
			Obstacles.Add({Gap + StopLineMarginM, 0.f, StopLineMinGapM, 0.f, TEXT("signal")});
		}
		else if (Car.LoggedGoApproach != Stop.ApproachId && Signals->GetApproachState(Stop.ApproachId, TrafficTimeSeconds).Aspect != ESignalAspect::Green)
		{
			Car.LoggedGoApproach = Stop.ApproachId;
			UE_LOG(LogAITrafficSim, Display, TEXT("AITRAFFIC goes on without green: approach %d aspect %d gap %.1f m speed %.1f m/s, %s"), Stop.ApproachId,
				int32(Signals->GetApproachState(Stop.ApproachId, TrafficTimeSeconds).Aspect), Gap, Car.SpeedMs, *Describe(Car));
		}
	}
}

void FAITrafficSimulation::ScanRoute(FSimCar& Car, double TrafficTimeSeconds, float DeltaSeconds, const TArray<FSimAgent>& Agents,
	FObstacleList& Obstacles)
{
	const float Horizon = FMath::Clamp(Square(Car.SpeedMs) / (2.f * 2.f) + 50.f, HorizonMinimumM, HorizonMaximumM);
	const int32 Count = Car.Route.Num();
	for (int32 Slot = Car.RouteIndex; Slot < Count; ++Slot)
	{
		const FTrafficLane& Lane = Lanes->GetLane(Car.Route[Slot]);
		const float Start = Car.LaneStart[Slot];
		if (Start - Car.FrontOverhangM > Horizon)
		{
			break;
		}
		if (Slot > Car.RouteIndex)
		{
			const float Cap = Lane.LimitMs() * Car.DesiredSpeedFactor;
			const float PreviousCap = Lanes->GetLane(Car.Route[Slot - 1]).LimitMs() * Car.DesiredSpeedFactor;
			if (Cap < PreviousCap - 0.3f)
			{
				Obstacles.Add({FMath::Max(Start - Car.FrontOverhangM, 0.05f), Cap, 0.f, 0.f, TEXT("speed limit")});
			}
		}
		ScanLaneCurves(Car, Slot, Lane, Horizon, Obstacles);
		if (!Lane.IsConnection())
		{
			ScanLaneSignals(Car, Slot, Lane, TrafficTimeSeconds, Obstacles);
		}
		if (Slot + 1 < Count && !Lane.IsConnection() && Lanes->GetLane(Car.Route[Slot + 1]).IsConnection())
		{
			const float GapToLine = Start + Lane.Length() - Car.FrontOverhangM;
			if (GapToLine > -0.5f)
			{
				ScanJunctionEntry(Car, Slot, GapToLine, DeltaSeconds, Obstacles, Agents);
			}
		}
		if (Slot == Count - 1 && Lane.Next.IsEmpty())
		{
			Obstacles.Add({FMath::Max(Start + Lane.Length() - Car.FrontOverhangM, 0.05f) + StopLineMarginM, 0.f, StopLineMinGapM, 0.f, TEXT("end of route")});
		}
	}
}

// -------------------------------------------------------------------------------------------------------------- junctions

bool FAITrafficSimulation::ConflictBlocks(const FSimCar& Car, const FLaneConflict& Conflict, float MyEnterSeconds, float MyExitSeconds,
	int32& BlockerId) const
{
	const TArray<FClaim>* Others = Claims.Find(Conflict.OtherLane);
	if (!Others)
	{
		return false;
	}
	for (const FClaim& Claim : *Others)
	{
		const FSimCar& Other = *Claim.Car;
		if (&Other == &Car)
		{
			continue;
		}
		const float LaneStart = Other.LaneStart[Claim.RouteSlot];
		const float StartDistance = LaneStart + Conflict.OtherStartMeters - Other.FrontOverhangM;
		const float EndDistance = LaneStart + Conflict.OtherEndMeters + Other.WheelbaseM + Other.RearOverhangM;
		if (EndDistance < 0.f)
		{
			continue;   // already clear of the shared area
		}
		if (Other.SpeedMs < 0.5f)
		{
			if (StartDistance <= 0.f)
			{
				BlockerId = Other.Id;
				return true;    // standing in the shared area
			}
			// A car that has right of way always goes first; among equals (right before left) the one that arrived first does.
			const bool bArrivedFirst = Other.WaitSeconds > Car.WaitSeconds + 0.2f
				|| (FMath::Abs(Other.WaitSeconds - Car.WaitSeconds) <= 0.2f && Other.Id < Car.Id);
			const bool bGoesFirst = Conflict.YieldKind == 2 || bArrivedFirst;
			const float PatienceSeconds = Conflict.YieldKind == 2 ? 2.f * PatientSeconds : PatientSeconds;
			if (StartDistance < 6.f && bGoesFirst && Car.WaitSeconds < PatienceSeconds)
			{
				BlockerId = Other.Id;
				return true;
			}
			continue;
		}
		const float OtherEnter = TimeToCover(FMath::Max(StartDistance, 0.f), Other.SpeedMs, 0.f, Other.SpeedMs);
		const float OtherExit = TimeToCover(EndDistance, Other.SpeedMs, 0.f, Other.SpeedMs);
		if (OtherEnter < MyExitSeconds + ConflictMarginSeconds && MyEnterSeconds < OtherExit + ConflictMarginSeconds)
		{
			BlockerId = Other.Id;
			return true;
		}
	}
	return false;
}

bool FAITrafficSimulation::IsExitCongested(const FSimCar& Car, const FTrafficLane& Connection, int32& BlockerId) const
{
	const TArray<FOccupant>* Occupants = Occupancy.Find(Connection.ToLane);
	if (!Occupants)
	{
		return false;
	}
	const float SpaceNeeded = Car.LengthM + 1.5f;
	for (const FOccupant& Other : *Occupants)
	{
		if (Other.Car != &Car && Other.RearS < SpaceNeeded && Other.Car->SpeedMs < 3.f)
		{
			BlockerId = Other.Car->Id;
			return true;
		}
	}
	return false;
}

bool FAITrafficSimulation::IsBlockedByAgent(const FSimCar& Car, const FTrafficLane& Connection, float EnterSeconds, float ExitSeconds,
	const TArray<FSimAgent>& Agents) const
{
	const float ClearanceM = Car.WidthM * 0.5f + 0.3f;
	for (const FSimAgent& Agent : Agents)
	{
		for (float Time = 0.f; Time <= 5.f; Time += 0.5f)
		{
			if (Time < EnterSeconds - 1.5f || Time > ExitSeconds + 1.0f)
			{
				continue;
			}
			const FVector2D Predicted = Agent.Position + Agent.Velocity * Time;
			for (int32 Index = 0; Index < Connection.Points.Num(); Index += 2)
			{
				if (FVector2D::Distance(Predicted, FVector2D(Connection.Points[Index])) < Agent.HalfWidthM + ClearanceM)
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool FAITrafficSimulation::IsConnectionBlocked(FSimCar& Car, int32 Slot, const TArray<FSimAgent>& Agents, const TCHAR*& Reason, int32& BlockerId) const
{
	const FTrafficLane& Connection = Lanes->GetLane(Car.Route[Slot + 1]);
	const float ConnectionStart = Car.LaneStart[Slot + 1];
	float MaxSpeed = FMath::Min(JunctionMaxSpeedMs, Connection.LimitMs());
	if (!Connection.CurveSpeedMs.IsEmpty())
	{
		float Slowest = MaxSpeed;
		for (const float Speed : Connection.CurveSpeedMs)
		{
			Slowest = FMath::Min(Slowest, Speed);
		}
		MaxSpeed = FMath::Max(Slowest, 2.5f);
	}
	if (IsExitCongested(Car, Connection, BlockerId) && Car.WaitSeconds < CongestionPatientSeconds)
	{
		Reason = TEXT("exit blocked");
		return true;
	}
	for (const FLaneConflict& Conflict : Connection.Conflicts)
	{
		if (!Conflict.Yields())
		{
			continue;
		}
		const float StartDistance = ConnectionStart + Conflict.StartMeters - Car.FrontOverhangM;
		const float EndDistance = ConnectionStart + Conflict.EndMeters + Car.WheelbaseM + Car.RearOverhangM;
		const float Enter = TimeToCover(FMath::Max(StartDistance, 0.f), Car.SpeedMs, JunctionAccelerationMs2, MaxSpeed);
		const float Exit = TimeToCover(EndDistance, Car.SpeedMs, JunctionAccelerationMs2, MaxSpeed);
		if (ConflictBlocks(Car, Conflict, Enter, Exit, BlockerId))
		{
			Reason = TEXT("right of way");
			return true;
		}
	}
	const float WholeStart = TimeToCover(FMath::Max(ConnectionStart - Car.FrontOverhangM, 0.f), Car.SpeedMs, JunctionAccelerationMs2, MaxSpeed);
	const float WholeEnd = TimeToCover(ConnectionStart + Connection.Length() + Car.WheelbaseM + Car.RearOverhangM, Car.SpeedMs, JunctionAccelerationMs2, MaxSpeed);
	if (IsBlockedByAgent(Car, Connection, WholeStart, WholeEnd, Agents))
	{
		Reason = TEXT("agent");
		return true;
	}
	return false;
}

void FAITrafficSimulation::ScanJunctionEntry(FSimCar& Car, int32 Slot, float GapToLineM, float DeltaSeconds, FObstacleList& Obstacles,
	const TArray<FSimAgent>& Agents)
{
	const FTrafficLane& Lane = Lanes->GetLane(Car.Route[Slot]);
	const bool bCurrent = Slot == Car.RouteIndex;
	switch (Lane.Control)
	{
	case ELaneControl::Stop:
		if (!(bCurrent && Car.bStopSatisfied))
		{
			Obstacles.Add({GapToLineM + StopLineMarginM, 0.f, StopLineMinGapM, 0.f, TEXT("stop sign")});
		}
		if (bCurrent)
		{
			if (GapToLineM < 2.5f && Car.SpeedMs < 0.25f)
			{
				Car.StopHoldSeconds += DeltaSeconds;
			}
			Car.bStopSatisfied = Car.bStopSatisfied || Car.StopHoldSeconds >= 0.8f;
		}
		break;
	case ELaneControl::Yield:
		Obstacles.Add({FMath::Max(GapToLineM, 0.05f), YieldApproachSpeedMs, 0.f, 0.f, TEXT("slow approach")});
		break;
	case ELaneControl::Equal:
		Obstacles.Add({FMath::Max(GapToLineM, 0.05f), EqualApproachSpeedMs, 0.f, 0.f, TEXT("slow approach")});
		break;
	default:
		break;
	}
	if (GapToLineM >= JunctionConsiderM)
	{
		return;
	}
	const TCHAR* Reason = TEXT("");
	int32 BlockerId = INDEX_NONE;
	if (IsConnectionBlocked(Car, Slot, Agents, Reason, BlockerId))
	{
		Obstacles.Add({GapToLineM + StopLineMarginM, 0.f, StopLineMinGapM, 0.f, Reason, BlockerId});
	}
}

// -------------------------------------------------------------------------------------------------------------- stepping

float FAITrafficSimulation::ComputeAcceleration(FSimCar& Car, double TrafficTimeSeconds, const TArray<FSimAgent>& Agents, float DeltaSeconds)
{
	FObstacleList Obstacles;
	ScanRoute(Car, TrafficTimeSeconds, DeltaSeconds, Agents, Obstacles);
	AddLeadCars(Car, Obstacles);
	AddAgentObstacles(Car, Agents, Obstacles);

	const float Speed = Car.SpeedMs;
	const float DesiredSpeedMs = DesiredSpeed(Car);
	const float BrakingTerm = 2.f * FMath::Sqrt(Car.MaxAccelerationMs2 * Car.ComfortDecelerationMs2);
	float Interaction = 0.f;
	const FObstacle* Dominant = nullptr;
	for (const FObstacle& Obstacle : Obstacles)
	{
		const float WantedGap = Obstacle.MinGapM + FMath::Max(0.f, Speed * Obstacle.HeadwaySeconds + Speed * (Speed - Obstacle.SpeedMs) / BrakingTerm);
		const float Ratio = Square(WantedGap / FMath::Max(Obstacle.GapM, 0.05f));
		if (Ratio > Interaction)
		{
			Interaction = Ratio;
			Dominant = &Obstacle;
		}
	}
	Car.BlockReason = TEXT("");
	Car.BlockerId = INDEX_NONE;
	if (Dominant && Interaction > 0.5f)
	{
		Car.BlockReason = Dominant->Reason;
		Car.BlockerId = Dominant->OtherId;
	}
	if (Car.StoppedSeconds > 20.f)
	{
		Car.ObstacleSummary.Reset();
		for (const FObstacle& Obstacle : Obstacles)
		{
			Car.ObstacleSummary += FString::Printf(TEXT("[%s gap %.1f speed %.1f other %d] "), Obstacle.Reason, Obstacle.GapM, Obstacle.SpeedMs, Obstacle.OtherId);
		}
	}
	const float Acceleration = Car.MaxAccelerationMs2 * (1.f - FMath::Pow(Speed / DesiredSpeedMs, 4.f) - Interaction);
	return FMath::Clamp(Acceleration, -MaxBrakingMs2, Car.MaxAccelerationMs2);
}

void FAITrafficSimulation::Advance(FSimCar& Car, float DeltaSeconds, float Acceleration)
{
	const float NewSpeed = FMath::Max(0.f, Car.SpeedMs + Acceleration * DeltaSeconds);
	const float Distance = 0.5f * (Car.SpeedMs + NewSpeed) * DeltaSeconds;
	Car.SpeedMs = NewSpeed;
	Car.AccelerationMs2 = Acceleration;
	Car.S += Distance;
	Car.WheelRollRadians += Distance / Car.WheelRadiusM;
	Stats.DistanceDrivenM += Distance;
	while (Car.S > Lanes->GetLane(Car.Route[Car.RouteIndex]).Length())
	{
		const float Length = Lanes->GetLane(Car.Route[Car.RouteIndex]).Length();
		if (Car.RouteIndex + 1 >= Car.Route.Num())
		{
			Car.S = Length;
			Car.SpeedMs = 0.f;
			break;
		}
		Car.S -= Length;
		++Car.RouteIndex;
		Car.bStopSatisfied = false;
		Car.StopHoldSeconds = 0.f;
		ExtendRoute(Car);
	}
}

void FAITrafficSimulation::UpdateSignals(FSimCar& Car) const
{
	Car.TurnSignal = 0;
	const int32 Last = FMath::Min(Car.Route.Num() - 1, Car.RouteIndex + 3);
	for (int32 Slot = Car.RouteIndex; Slot <= Last; ++Slot)
	{
		const FTrafficLane& Lane = Lanes->GetLane(Car.Route[Slot]);
		if (!Lane.IsConnection() || (Lane.Turn == 0 && Lane.Kind != ELaneKind::UTurn))
		{
			continue;
		}
		if (Car.LaneStart[Slot] < 35.f)
		{
			Car.TurnSignal = Lane.Kind == ELaneKind::UTurn ? -1 : Lane.Turn;
		}
		return;
	}
}

void FAITrafficSimulation::DetectContacts(const TArray<FSimAgent>& Agents)
{
	TSet<uint64> Touching;
	for (int32 Index = 0; Index < Cars.Num(); ++Index)
	{
		const FSimCar& A = *Cars[Index];
		for (int32 Other = Index + 1; Other < Cars.Num(); ++Other)
		{
			const FSimCar& B = *Cars[Other];
			const FVector2D CenterA = A.BodyCenter();
			const FVector2D CenterB = B.BodyCenter();
			if (FVector2D::Distance(CenterA, CenterB) > (A.LengthM + B.LengthM) * 0.5f + 0.5f)
			{
				continue;
			}
			if (!RectanglesOverlap(CenterA, A.Heading, A.LengthM * 0.5f, A.WidthM * 0.46f, CenterB, B.Heading, B.LengthM * 0.5f, B.WidthM * 0.46f))
			{
				continue;
			}
			const uint64 Key = (uint64(FMath::Min(A.Id, B.Id)) << 32) | uint64(FMath::Max(A.Id, B.Id));
			Touching.Add(Key);
			if (!ContactPairs.Contains(Key))
			{
				++Stats.Collisions;
				UE_LOG(LogAITrafficSim, Warning, TEXT("AITRAFFIC collision between %s and %s at (%.1f, %.1f)"), *Describe(A), *Describe(B),
					CenterA.X, CenterA.Y);
			}
		}
	}
	ContactPairs = MoveTemp(Touching);

	TSet<uint64> TouchingAgents;
	for (int32 AgentIndex = 0; AgentIndex < Agents.Num(); ++AgentIndex)
	{
		const FSimAgent& Agent = Agents[AgentIndex];
		for (const TSharedPtr<FSimCar>& Car : Cars)
		{
			if (!RectanglesOverlap(Car->BodyCenter(), Car->Heading, Car->LengthM * 0.5f, Car->WidthM * 0.46f, Agent.Position, Agent.Forward,
				Agent.HalfLengthM, Agent.HalfWidthM * 0.92f))
			{
				continue;
			}
			const uint64 Key = (uint64(AgentIndex + 1) << 32) | uint64(Car->Id);
			TouchingAgents.Add(Key);
			if (!AgentContactPairs.Contains(Key))
			{
				++Stats.AgentContacts;
				UE_LOG(LogAITrafficSim, Warning, TEXT("AITRAFFIC contact with an outside vehicle at (%.1f, %.1f): %s"), Agent.Position.X, Agent.Position.Y, *Describe(*Car));
			}
		}
	}
	AgentContactPairs = MoveTemp(TouchingAgents);
}

bool FAITrafficSimulation::IsLegitimateWait(const FSimCar& Car)
{
	// Follow the chain of cars waiting for each other to the car at its head and judge that one's reason.
	const FSimCar* Head = &Car;
	for (int32 Hop = 0; Hop < 24; ++Hop)
	{
		const FString Reason(Head->StopReason);
		const bool bWaitsForCar = Reason == TEXT("car ahead") || Reason == TEXT("right of way") || Reason == TEXT("exit blocked")
			|| Reason == TEXT("vehicle in the way");
		if (bWaitsForCar && Head->StopBlockerId >= 0)
		{
			const FSimCar* Next = FindCar(Head->StopBlockerId);
			if (!Next || Next == Head)
			{
				break;
			}
			Head = Next;
			continue;
		}
		if (Reason == TEXT("vehicle in the way") || Reason == TEXT("agent"))
		{
			return true;                        // held up by the player or another outside vehicle
		}
		if (Reason == TEXT("signal"))
		{
			return Head->StoppedSeconds < SignalWaitLimitSeconds;
		}
		break;
	}
	return false;
}

void FAITrafficSimulation::StepOnce(float DeltaSeconds, double TrafficTimeSeconds, const TArray<FSimAgent>& Agents)
{
	for (const TSharedPtr<FSimCar>& Car : Cars)
	{
		ExtendRoute(*Car);
		UpdateGeometry(*Car);
	}
	BuildOccupancy();
	TArray<float, TInlineAllocator<64>> Accelerations;
	for (const TSharedPtr<FSimCar>& Car : Cars)
	{
		Accelerations.Add(ComputeAcceleration(*Car, TrafficTimeSeconds, Agents, DeltaSeconds));
	}
	for (int32 Index = 0; Index < Cars.Num(); ++Index)
	{
		FSimCar& Car = *Cars[Index];
		Advance(Car, DeltaSeconds, Accelerations[Index]);
		UpdateGeometry(Car);
		UpdateSignals(Car);
		Car.SecondsAlive += DeltaSeconds;
		Car.WaitSeconds = Car.SpeedMs < 0.5f ? Car.WaitSeconds + DeltaSeconds : 0.f;
		Car.StoppedSeconds = Car.SpeedMs < 0.2f ? Car.StoppedSeconds + DeltaSeconds : 0.f;
		if (Car.SpeedMs < 0.2f && Car.BlockReason[0] != 0)
		{
			Car.StopReason = Car.BlockReason;
			Car.StopBlockerId = Car.BlockerId;
		}
		Car.bBrakeLight = Car.AccelerationMs2 < -0.4f || (Car.SpeedMs < 0.3f && Car.WaitSeconds > 0.1f);
		Stats.CarSeconds += DeltaSeconds;
		if (Car.StoppedSeconds > LongWaitLogSeconds && !Car.bLongWaitLogged)
		{
			Car.bLongWaitLogged = true;
			UE_LOG(LogAITrafficSim, Display, TEXT("AITRAFFIC long wait: %s"), *Describe(Car));
		}
		if (Car.StoppedSeconds > DeadlockSeconds && !Car.bDeadlockReported)
		{
			Car.bDeadlockReported = true;
			++Stats.LongWaits;
			if (!IsLegitimateWait(Car))
			{
				++Stats.Deadlocks;
				UE_LOG(LogAITrafficSim, Warning, TEXT("AITRAFFIC deadlock: %s"), *Describe(Car));
			}
		}
	}
	DetectContacts(Agents);
}

void FAITrafficSimulation::Step(float DeltaSeconds, double TrafficTimeSeconds, const TArray<FSimAgent>& Agents)
{
	if (!Lanes || !Signals || DeltaSeconds <= 0.f)
	{
		return;
	}
	const int32 Substeps = FMath::Clamp(FMath::CeilToInt(DeltaSeconds / MaxSubstepSeconds), 1, 8);
	const float SubstepSeconds = DeltaSeconds / float(Substeps);
	for (int32 Index = 0; Index < Substeps; ++Index)
	{
		StepOnce(SubstepSeconds, TrafficTimeSeconds - double(DeltaSeconds) + double(SubstepSeconds) * double(Index + 1), Agents);
	}
}

FString FAITrafficSimulation::Describe(const FSimCar& Car) const
{
	return FString::Printf(TEXT("car %d on lane %d (way %lld, control %d) s=%.1f/%.1f speed %.1f m/s, stopped %.0f s, reason '%s' blocker %d, at (%.1f, %.1f), route %d,%d,%d %s"),
		Car.Id, Car.Route[Car.RouteIndex], Lanes->GetLane(Car.Route[Car.RouteIndex]).WayId, int32(Lanes->GetLane(Car.Route[Car.RouteIndex]).Control), Car.S,
		Lanes->GetLane(Car.Route[Car.RouteIndex]).Length(), Car.SpeedMs, Car.StoppedSeconds, Car.BlockReason, Car.BlockerId, Car.FrontAxleM.X,
		Car.FrontAxleM.Y, Car.Route[Car.RouteIndex], Car.Route.IsValidIndex(Car.RouteIndex + 1) ? Car.Route[Car.RouteIndex + 1] : -1,
		Car.Route.IsValidIndex(Car.RouteIndex + 2) ? Car.Route[Car.RouteIndex + 2] : -1, *Car.ObstacleSummary);
}
