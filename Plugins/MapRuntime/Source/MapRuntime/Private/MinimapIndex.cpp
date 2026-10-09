#include "MinimapIndex.h"

#include "LaneNetwork.h"

namespace
{
/** Edge of a grid cell, metres. */
constexpr float CellSizeM = 128.f;

/** How far the simplified line may stray from the lane centre line, metres. */
constexpr float SimplifyToleranceM = 0.6f;

/** Distance between one-way arrows, and the shortest lane that gets one, metres. */
constexpr float ArrowSpacingM = 90.f;
constexpr float ArrowMinimumLaneM = 30.f;

/** A lane whose reverse twin starts and ends within this distance of its own end and start is part of a two-way road. */
constexpr float TwinToleranceM = 6.f;

/** Extra distance a lane running against the travel direction counts in FindNearestLane, metres. */
constexpr float OppositeDirectionPenaltyM = 30.f;

/** The point of a segment closest to a position. */
FVector2f ClosestPointOnSegment(const FVector2f& Point, const FVector2f& From, const FVector2f& To)
{
	const FVector2f Segment = To - From;
	const float LengthSquared = Segment.SizeSquared();
	if (LengthSquared < KINDA_SMALL_NUMBER)
	{
		return From;
	}
	const float Along = FMath::Clamp(FVector2f::DotProduct(Point - From, Segment) / LengthSquared, 0.f, 1.f);
	return From + Along * Segment;
}

FIntPoint CellOf(const FVector2f& PositionM)
{
	return FIntPoint(FMath::FloorToInt(PositionM.X / CellSizeM), FMath::FloorToInt(PositionM.Y / CellSizeM));
}
}

void MinimapGeometry::SimplifyLine(const TArray<FVector2f>& Line, TArray<FVector2f>& OutLine, float ToleranceMeters)
{
	OutLine.Reset();
	const int32 Count = Line.Num();
	if (Count < 3)
	{
		OutLine = Line;
		return;
	}
	TArray<bool> Keep;
	Keep.Init(false, Count);
	Keep[0] = true;
	Keep[Count - 1] = true;
	TArray<TPair<int32, int32>> Pending;
	Pending.Emplace(0, Count - 1);
	while (Pending.Num() > 0)
	{
		const TPair<int32, int32> Range = Pending.Pop(EAllowShrinking::No);
		float FarthestDistance = 0.f;
		int32 FarthestIndex = INDEX_NONE;
		for (int32 Index = Range.Key + 1; Index < Range.Value; ++Index)
		{
			const FVector2f Closest = ClosestPointOnSegment(Line[Index], Line[Range.Key], Line[Range.Value]);
			const float Distance = FVector2f::Distance(Line[Index], Closest);
			if (Distance > FarthestDistance)
			{
				FarthestDistance = Distance;
				FarthestIndex = Index;
			}
		}
		if (FarthestIndex == INDEX_NONE || FarthestDistance <= ToleranceMeters)
		{
			continue;
		}
		Keep[FarthestIndex] = true;
		Pending.Emplace(Range.Key, FarthestIndex);
		Pending.Emplace(FarthestIndex, Range.Value);
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Keep[Index])
		{
			OutLine.Add(Line[Index]);
		}
	}
}

int32 MinimapGeometry::MinTierForRadius(float RadiusMeters)
{
	if (RadiusMeters <= 250.f)
	{
		return 0;
	}
	if (RadiusMeters <= 500.f)
	{
		return 1;
	}
	if (RadiusMeters <= 1000.f)
	{
		return 2;
	}
	if (RadiusMeters <= 3000.f)
	{
		return 3;
	}
	return 4;
}

FVector2f MinimapGeometry::ToViewFrame(const FVector2f& WorldOffset, float HeadingRadians)
{
	const FVector2f Forward(FMath::Cos(HeadingRadians), FMath::Sin(HeadingRadians));
	const FVector2f Right(-Forward.Y, Forward.X);
	return FVector2f(FVector2f::DotProduct(WorldOffset, Right), FVector2f::DotProduct(WorldOffset, Forward));
}

/** For every road lane whether a lane runs the other way along it; lanes without one belong to one-way roads. */
static TArray<bool> FindTwoWayLanes(const FLaneNetwork& Network)
{
	const TArray<FTrafficLane>& NetworkLanes = Network.GetLanes();
	TMap<int64, TArray<int32>> LanesByWay;
	for (const FTrafficLane& Lane : NetworkLanes)
	{
		if (!Lane.IsConnection() && Lane.Points.Num() >= 2)
		{
			LanesByWay.FindOrAdd(Lane.WayId).Add(Lane.Id);
		}
	}
	TArray<bool> bTwoWay;
	bTwoWay.Init(false, NetworkLanes.Num());
	for (const TPair<int64, TArray<int32>>& Way : LanesByWay)
	{
		for (const int32 LaneId : Way.Value)
		{
			const FTrafficLane& Lane = NetworkLanes[LaneId];
			for (const int32 OtherId : Way.Value)
			{
				const FTrafficLane& Other = NetworkLanes[OtherId];
				const bool bReverse = FVector::Dist2D(Other.Points[0], Lane.Points.Last()) < TwinToleranceM
					&& FVector::Dist2D(Other.Points.Last(), Lane.Points[0]) < TwinToleranceM;
				if (OtherId != LaneId && bReverse)
				{
					bTwoWay[LaneId] = true;
					break;
				}
			}
		}
	}
	return bTwoWay;
}

void FMinimapIndex::Build(const FLaneNetwork& Network)
{
	Points.Reset();
	Arrows.Reset();
	Lanes.Reset();
	Grid.Reset();
	const TArray<bool> bTwoWay = FindTwoWayLanes(Network);
	TArray<FVector2f> RawLine;
	TArray<FVector2f> Simplified;
	for (const FTrafficLane& Lane : Network.GetLanes())
	{
		if (Lane.IsConnection() || Lane.Points.Num() < 2)
		{
			continue;
		}
		RawLine.Reset();
		for (const FVector& Point : Lane.Points)
		{
			RawLine.Add(FVector2f(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
		}
		MinimapGeometry::SimplifyLine(RawLine, Simplified, SimplifyToleranceM);
		FLane Entry;
		Entry.Tier = Lane.Tier;
		Entry.NetworkLaneId = Lane.Id;
		Entry.bGood = Lane.bGood;
		Entry.FirstArrow = Arrows.Num();
		if (!bTwoWay[Lane.Id])
		{
			AddArrows(RawLine, Entry);
		}
		AddLane(Simplified, Entry);
	}
	VisitStamp.Init(0, Lanes.Num());
	CurrentStamp = 0;
}

void FMinimapIndex::AddArrows(const TArray<FVector2f>& Line, FLane& InOutLane)
{
	float Travelled = 0.f;
	float NextArrowAt = 0.5f * ArrowSpacingM;
	float TotalLength = 0.f;
	for (int32 Index = 1; Index < Line.Num(); ++Index)
	{
		TotalLength += FVector2f::Distance(Line[Index - 1], Line[Index]);
	}
	if (TotalLength < ArrowMinimumLaneM)
	{
		return;
	}
	NextArrowAt = FMath::Min(NextArrowAt, 0.5f * TotalLength);
	for (int32 Index = 1; Index < Line.Num(); ++Index)
	{
		const float SegmentLength = FVector2f::Distance(Line[Index - 1], Line[Index]);
		while (SegmentLength > KINDA_SMALL_NUMBER && Travelled + SegmentLength >= NextArrowAt)
		{
			const float Along = (NextArrowAt - Travelled) / SegmentLength;
			FMinimapArrow Arrow;
			Arrow.Position = FMath::Lerp(Line[Index - 1], Line[Index], Along);
			Arrow.Direction = (Line[Index] - Line[Index - 1]).GetSafeNormal();
			Arrows.Add(Arrow);
			++InOutLane.ArrowCount;
			NextArrowAt += ArrowSpacingM;
		}
		Travelled += SegmentLength;
	}
}

void FMinimapIndex::AddLane(const TArray<FVector2f>& SimplifiedPoints, const FLane& Template)
{
	FLane Lane = Template;
	Lane.FirstPoint = Points.Num();
	Lane.PointCount = SimplifiedPoints.Num();
	Lane.BoundsMin = SimplifiedPoints[0];
	Lane.BoundsMax = SimplifiedPoints[0];
	for (const FVector2f& Point : SimplifiedPoints)
	{
		Lane.BoundsMin = FVector2f::Min(Lane.BoundsMin, Point);
		Lane.BoundsMax = FVector2f::Max(Lane.BoundsMax, Point);
	}
	Points.Append(SimplifiedPoints);

	const int32 LaneIndex = Lanes.Add(Lane);
	const FIntPoint MinCell = CellOf(Lane.BoundsMin);
	const FIntPoint MaxCell = CellOf(Lane.BoundsMax);
	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			Grid.FindOrAdd(FIntPoint(CellX, CellY)).Add(LaneIndex);
		}
	}
}

void FMinimapIndex::Query(const FVector2f& CentreM, float RadiusM, float HeadingRadians, int32 MinTier, FMinimapFrame& OutFrame) const
{
	OutFrame.Reset();
	if (Lanes.Num() == 0)
	{
		return;
	}
	++CurrentStamp;
	const FIntPoint MinCell = CellOf(CentreM - FVector2f(RadiusM, RadiusM));
	const FIntPoint MaxCell = CellOf(CentreM + FVector2f(RadiusM, RadiusM));
	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			const TArray<int32>* CellLanes = Grid.Find(FIntPoint(CellX, CellY));
			if (!CellLanes)
			{
				continue;
			}
			for (const int32 LaneIndex : *CellLanes)
			{
				const FLane& Lane = Lanes[LaneIndex];
				const bool bSeen = VisitStamp[LaneIndex] == CurrentStamp;
				const bool bOutside = Lane.BoundsMax.X < CentreM.X - RadiusM || Lane.BoundsMin.X > CentreM.X + RadiusM
					|| Lane.BoundsMax.Y < CentreM.Y - RadiusM || Lane.BoundsMin.Y > CentreM.Y + RadiusM;
				if (bSeen || bOutside || Lane.Tier < MinTier)
				{
					continue;
				}
				VisitStamp[LaneIndex] = CurrentStamp;
				FMinimapStroke Stroke;
				Stroke.FirstPoint = OutFrame.Points.Num();
				Stroke.PointCount = Lane.PointCount;
				Stroke.Tier = Lane.Tier;
				for (int32 Offset = 0; Offset < Lane.PointCount; ++Offset)
				{
					OutFrame.Points.Add(MinimapGeometry::ToViewFrame(Points[Lane.FirstPoint + Offset] - CentreM, HeadingRadians));
				}
				OutFrame.Strokes.Add(Stroke);
				for (int32 Offset = 0; Offset < Lane.ArrowCount; ++Offset)
				{
					const FMinimapArrow& Arrow = Arrows[Lane.FirstArrow + Offset];
					FMinimapArrow ViewArrow;
					ViewArrow.Position = MinimapGeometry::ToViewFrame(Arrow.Position - CentreM, HeadingRadians);
					ViewArrow.Direction = MinimapGeometry::ToViewFrame(Arrow.Direction, HeadingRadians);
					OutFrame.Arrows.Add(ViewArrow);
				}
			}
		}
	}
}

float FMinimapIndex::DistanceToLane(const FLane& Lane, const FVector2f& PositionM, FVector2f& OutPoint, FVector2f& OutDirection) const
{
	float BestDistance = TNumericLimits<float>::Max();
	for (int32 Offset = 0; Offset + 1 < Lane.PointCount; ++Offset)
	{
		const FVector2f& From = Points[Lane.FirstPoint + Offset];
		const FVector2f& To = Points[Lane.FirstPoint + Offset + 1];
		const FVector2f Closest = ClosestPointOnSegment(PositionM, From, To);
		const float Distance = FVector2f::Distance(PositionM, Closest);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			OutPoint = Closest;
			OutDirection = (To - From).GetSafeNormal();
		}
	}
	return BestDistance;
}

bool FMinimapIndex::FindNearestLane(const FVector2f& PositionM, float MaxDistanceM, const FVector2f* TravelDirection, FMinimapLaneHit& OutHit) const
{
	OutHit = FMinimapLaneHit();
	float BestScore = TNumericLimits<float>::Max();
	const FIntPoint MinCell = CellOf(PositionM - FVector2f(MaxDistanceM, MaxDistanceM));
	const FIntPoint MaxCell = CellOf(PositionM + FVector2f(MaxDistanceM, MaxDistanceM));
	for (int32 CellY = MinCell.Y; CellY <= MaxCell.Y; ++CellY)
	{
		for (int32 CellX = MinCell.X; CellX <= MaxCell.X; ++CellX)
		{
			const TArray<int32>* CellLanes = Grid.Find(FIntPoint(CellX, CellY));
			if (!CellLanes)
			{
				continue;
			}
			for (const int32 LaneIndex : *CellLanes)
			{
				const FLane& Lane = Lanes[LaneIndex];
				if (!Lane.bGood)
				{
					continue;
				}
				FVector2f Point = FVector2f::ZeroVector;
				FVector2f Direction = FVector2f::ZeroVector;
				const float Distance = DistanceToLane(Lane, PositionM, Point, Direction);
				float Score = Distance;
				if (TravelDirection)
				{
					Score += OppositeDirectionPenaltyM * 0.5f * (1.f - FVector2f::DotProduct(Direction, *TravelDirection));
				}
				if (Distance > MaxDistanceM || Score >= BestScore)
				{
					continue;
				}
				BestScore = Score;
				OutHit.LaneId = Lane.NetworkLaneId;
				OutHit.DistanceMeters = Distance;
				OutHit.Point = Point;
			}
		}
	}
	return OutHit.LaneId != INDEX_NONE;
}
