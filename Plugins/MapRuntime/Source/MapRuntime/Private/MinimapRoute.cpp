#include "MinimapRoute.h"

#include "LaneNetwork.h"

namespace
{
/** Faster than any speed limit in the graph, so the straight line heuristic never overestimates, m/s. */
constexpr float HeuristicSpeedMs = 36.f;

/** Slowest speed counted when a lane's limit is tiny, m/s. */
constexpr float MinimumSpeedMs = 3.f;

/** Extra seconds a U-turn costs, so routes avoid them unless nothing else works. */
constexpr float UTurnPenaltySeconds = 40.f;

/** How far the drawn route may stray from the lane centre lines, metres. */
constexpr float RouteSimplifyToleranceM = 0.5f;

struct FOpenEntry
{
	float EstimatedTotal = 0.f;
	int32 LaneId = INDEX_NONE;

	bool operator<(const FOpenEntry& Other) const { return EstimatedTotal < Other.EstimatedTotal; }
};

FVector2f LaneEnd(const FTrafficLane& Lane)
{
	return FVector2f(static_cast<float>(Lane.Points.Last().X), static_cast<float>(Lane.Points.Last().Y));
}

/** Seconds needed to drive through a whole lane. */
float LaneSeconds(const FTrafficLane& Lane)
{
	const float Penalty = Lane.Kind == ELaneKind::UTurn ? UTurnPenaltySeconds : 0.f;
	return Lane.Length() / FMath::Max(Lane.LimitMs(), MinimumSpeedMs) + Penalty;
}

/** Lane ids from the start to the goal by following the parent links back. */
void CollectPath(const TArray<int32>& Parent, int32 StartLane, int32 GoalLane, TArray<int32>& OutLaneIds)
{
	for (int32 LaneId = GoalLane; LaneId != INDEX_NONE; LaneId = Parent[LaneId])
	{
		OutLaneIds.Add(LaneId);
		if (LaneId == StartLane)
		{
			break;
		}
	}
	Algo::Reverse(OutLaneIds);
}

/** The lanes' points joined into one simplified line with its arc lengths. */
void BuildRouteGeometry(const FLaneNetwork& Network, FMinimapRoute& InOutRoute)
{
	TArray<FVector2f> Raw;
	for (const int32 LaneId : InOutRoute.LaneIds)
	{
		const FTrafficLane& Lane = Network.GetLane(LaneId);
		for (const FVector& Point : Lane.Points)
		{
			const FVector2f Point2D(static_cast<float>(Point.X), static_cast<float>(Point.Y));
			if (Raw.Num() == 0 || FVector2f::DistSquared(Raw.Last(), Point2D) > 0.01f)
			{
				Raw.Add(Point2D);
			}
		}
	}
	MinimapGeometry::SimplifyLine(Raw, InOutRoute.Points, RouteSimplifyToleranceM);
	InOutRoute.Arc.Reset();
	float Distance = 0.f;
	for (int32 Index = 0; Index < InOutRoute.Points.Num(); ++Index)
	{
		if (Index > 0)
		{
			Distance += FVector2f::Distance(InOutRoute.Points[Index - 1], InOutRoute.Points[Index]);
		}
		InOutRoute.Arc.Add(Distance);
	}
	InOutRoute.TotalMeters = Distance;
}
}

bool MinimapRouting::FindRoute(const FLaneNetwork& Network, int32 StartLane, int32 GoalLane, FMinimapRoute& OutRoute)
{
	const double StartSeconds = FPlatformTime::Seconds();
	OutRoute = FMinimapRoute();
	const TArray<FTrafficLane>& Lanes = Network.GetLanes();
	if (!Lanes.IsValidIndex(StartLane) || !Lanes.IsValidIndex(GoalLane))
	{
		return false;
	}
	const FVector2f GoalPoint = LaneEnd(Lanes[GoalLane]);

	TArray<float> CostSoFar;
	CostSoFar.Init(TNumericLimits<float>::Max(), Lanes.Num());
	TArray<int32> Parent;
	Parent.Init(INDEX_NONE, Lanes.Num());
	TArray<bool> Closed;
	Closed.Init(false, Lanes.Num());
	TArray<FOpenEntry> Open;

	CostSoFar[StartLane] = LaneSeconds(Lanes[StartLane]);
	Open.HeapPush({CostSoFar[StartLane], StartLane});
	bool bFound = false;
	while (Open.Num() > 0)
	{
		FOpenEntry Current;
		Open.HeapPop(Current, EAllowShrinking::No);
		if (Closed[Current.LaneId])
		{
			continue;
		}
		Closed[Current.LaneId] = true;
		if (Current.LaneId == GoalLane)
		{
			bFound = true;
			break;
		}
		for (const int32 NextId : Lanes[Current.LaneId].Next)
		{
			const FTrafficLane& Next = Lanes[NextId];
			const float NextCost = CostSoFar[Current.LaneId] + LaneSeconds(Next);
			if (!Next.bGood || Closed[NextId] || NextCost >= CostSoFar[NextId])
			{
				continue;
			}
			CostSoFar[NextId] = NextCost;
			Parent[NextId] = Current.LaneId;
			const float Remaining = FVector2f::Distance(LaneEnd(Next), GoalPoint) / HeuristicSpeedMs;
			Open.HeapPush({NextCost + Remaining, NextId});
		}
	}
	if (bFound)
	{
		CollectPath(Parent, StartLane, GoalLane, OutRoute.LaneIds);
		BuildRouteGeometry(Network, OutRoute);
	}
	OutRoute.SearchMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	return bFound && OutRoute.IsValid();
}

bool FMinimapRoute::Locate(const FVector2f& PositionM, float& OutDistanceFromRouteM, float& OutRemainingM) const
{
	if (!IsValid())
	{
		return false;
	}
	OutDistanceFromRouteM = TNumericLimits<float>::Max();
	for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
	{
		const FVector2f Segment = Points[Index + 1] - Points[Index];
		const float LengthSquared = Segment.SizeSquared();
		const float Along = LengthSquared > KINDA_SMALL_NUMBER ? FMath::Clamp(FVector2f::DotProduct(PositionM - Points[Index], Segment) / LengthSquared, 0.f, 1.f) : 0.f;
		const float Distance = FVector2f::Distance(PositionM, Points[Index] + Along * Segment);
		if (Distance < OutDistanceFromRouteM)
		{
			OutDistanceFromRouteM = Distance;
			OutRemainingM = TotalMeters - (Arc[Index] + Along * (Arc[Index + 1] - Arc[Index]));
		}
	}
	return true;
}

void FMinimapRoute::AppendToFrame(const FVector2f& CentreM, float RadiusM, float HeadingRadians, FMinimapFrame& OutFrame) const
{
	const float RadiusSquared = RadiusM * RadiusM;
	const int32 Count = Points.Num();
	const auto IsNear = [&](int32 Index) { return FVector2f::DistSquared(Points[Index], CentreM) < RadiusSquared; };
	FMinimapStroke Stroke;
	Stroke.Tier = MinimapRouteTier;
	bool bOpen = false;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		// A point is kept when it or a neighbour is in range, so segments that cross the edge still reach it.
		const bool bKeep = IsNear(Index) || (Index > 0 && IsNear(Index - 1)) || (Index + 1 < Count && IsNear(Index + 1));
		if (bKeep && !bOpen)
		{
			Stroke.FirstPoint = OutFrame.Points.Num();
			Stroke.PointCount = 0;
			bOpen = true;
		}
		if (bKeep)
		{
			OutFrame.Points.Add(MinimapGeometry::ToViewFrame(Points[Index] - CentreM, HeadingRadians));
			++Stroke.PointCount;
		}
		const bool bClose = bOpen && (!bKeep || Index + 1 == Count);
		if (bClose)
		{
			OutFrame.Strokes.Add(Stroke);
			bOpen = false;
		}
	}
}
