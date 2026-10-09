#include "TrafficNetwork.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr float ApproachCellCm = 5000.f;
constexpr float SegmentCellCm = 10000.f;
constexpr float RedAmberSeconds = 1.f;
/** A vehicle counts as in line with an approach when its heading is within about 35 degrees of the approach direction. */
constexpr float MinimumHeadingDot = 0.82f;
constexpr float StopLineMarginCm = 150.f;
constexpr float RoadMatchMarginCm = 400.f;
constexpr float MisalignedPenaltyCm = 2500.f;

FVector2D ReadPoint(const TArray<TSharedPtr<FJsonValue>>& Values, int32 First)
{
	return FVector2D(Values[First]->AsNumber(), Values[First + 1]->AsNumber()) * 100.0;
}

float SafeInverse(float Value)
{
	return Value > 1e-4f ? 1.f / Value : 0.f;
}

/** Distance from a point to a segment, with the segment's unit direction. */
float DistanceToSegment(const FVector2D& Point, const FVector2D& A, const FVector2D& B, FVector2D& OutDirection)
{
	const FVector2D Edge = B - A;
	const double LengthSquared = FMath::Max(Edge.SizeSquared(), 1e-6);
	const double T = FMath::Clamp(FVector2D::DotProduct(Point - A, Edge) / LengthSquared, 0.0, 1.0);
	OutDirection = Edge / FMath::Sqrt(LengthSquared);
	return float(FVector2D::Distance(Point, A + Edge * T));
}
}

bool FTrafficNetwork::Load(const FString& Path, FString& Error)
{
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		Error = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	Junctions.Reset();
	Approaches.Reset();
	Ways.Reset();
	for (const TSharedPtr<FJsonValue>& JunctionValue : Root->GetArrayField(TEXT("junctions")))
	{
		const TSharedPtr<FJsonObject> Entry = JunctionValue->AsObject();
		FSignalJunction& Junction = Junctions.AddDefaulted_GetRef();
		const int32 JunctionIndex = Junctions.Num() - 1;
		Junction.Id = Entry->GetIntegerField(TEXT("id"));
		Junction.Center = FVector2D(Entry->GetNumberField(TEXT("x")), Entry->GetNumberField(TEXT("y"))) * 100.0;
		Junction.bCrossingOnly = Entry->GetBoolField(TEXT("crossing_only"));
		float Start = 0.f;
		for (const TSharedPtr<FJsonValue>& PhaseValue : Entry->GetArrayField(TEXT("phases")))
		{
			const TSharedPtr<FJsonObject> PhaseEntry = PhaseValue->AsObject();
			FSignalPhase& Phase = Junction.Phases.AddDefaulted_GetRef();
			Phase.GreenSeconds = PhaseEntry->GetNumberField(TEXT("green"));
			Phase.AmberSeconds = PhaseEntry->GetNumberField(TEXT("amber"));
			Phase.ClearanceSeconds = PhaseEntry->GetNumberField(TEXT("clearance"));
			Phase.bUnserved = PhaseEntry->GetBoolField(TEXT("pedestrian"));
			Phase.StartSeconds = Start;
			Start += Phase.TotalSeconds();
		}
		Junction.CycleSeconds = FMath::Max(Start, 1.f);
		Junction.OffsetSeconds = float((uint32(Junction.Id) * 2654435761u) % 1000u) / 1000.f * Junction.CycleSeconds;
		for (const TSharedPtr<FJsonValue>& ApproachValue : Entry->GetArrayField(TEXT("approaches")))
		{
			const TSharedPtr<FJsonObject> ApproachEntry = ApproachValue->AsObject();
			FTrafficApproach& Approach = Approaches.AddDefaulted_GetRef();
			Approach.Id = ApproachEntry->GetIntegerField(TEXT("id"));
			Approach.JunctionIndex = JunctionIndex;
			Approach.Phase = ApproachEntry->GetIntegerField(TEXT("phase"));
			Approach.WayId = int64(ApproachEntry->GetNumberField(TEXT("way")));
			const TArray<TSharedPtr<FJsonValue>>& Line = ApproachEntry->GetArrayField(TEXT("stop_line"));
			Approach.StopLineLeft = ReadPoint(Line, 0);
			Approach.StopLineRight = ReadPoint(Line, 2);
			const TArray<TSharedPtr<FJsonValue>>& Direction = ApproachEntry->GetArrayField(TEXT("direction"));
			Approach.Direction = FVector2D(Direction[0]->AsNumber(), Direction[1]->AsNumber());
			Approach.Lanes = ApproachEntry->GetIntegerField(TEXT("lanes"));
			Approach.SpeedKmh = ApproachEntry->GetNumberField(TEXT("speed"));
			Junction.ApproachIds.Add(Approach.Id);
		}
	}
	for (const TSharedPtr<FJsonValue>& WayValue : Root->GetArrayField(TEXT("speed_ways")))
	{
		const TSharedPtr<FJsonObject> Entry = WayValue->AsObject();
		FSpeedWay& Way = Ways.AddDefaulted_GetRef();
		Way.Id = int64(Entry->GetNumberField(TEXT("id")));
		Way.LimitKmh = Entry->GetNumberField(TEXT("limit"));
		Way.WidthCm = Entry->GetNumberField(TEXT("width")) * 100.f;
		Way.bOneway = Entry->GetBoolField(TEXT("oneway"));
		for (const TSharedPtr<FJsonValue>& PointValue : Entry->GetArrayField(TEXT("points")))
		{
			Way.Points.Add(ReadPoint(PointValue->AsArray(), 0));
		}
	}
	BuildIndices();
	bLoaded = true;
	return true;
}

void FTrafficNetwork::BuildIndices()
{
	ApproachCells.Reset();
	SegmentCells.Reset();
	ApproachById.Reset();
	for (int32 Index = 0; Index < Approaches.Num(); ++Index)
	{
		const FTrafficApproach& Approach = Approaches[Index];
		ApproachById.Add(Approach.Id, Index);
		const FVector2D Center = Approach.StopLineCenter();
		ApproachCells.FindOrAdd(CellKey(FMath::FloorToInt(Center.X / ApproachCellCm), FMath::FloorToInt(Center.Y / ApproachCellCm))).Add(Index);
	}
	for (int32 WayIndex = 0; WayIndex < Ways.Num(); ++WayIndex)
	{
		const FSpeedWay& Way = Ways[WayIndex];
		for (int32 PointIndex = 0; PointIndex + 1 < Way.Points.Num(); ++PointIndex)
		{
			// Register the segment in every cell its bounding box touches.
			const FVector2D A = Way.Points[PointIndex];
			const FVector2D B = Way.Points[PointIndex + 1];
			const int32 MinX = FMath::FloorToInt(FMath::Min(A.X, B.X) / SegmentCellCm);
			const int32 MaxX = FMath::FloorToInt(FMath::Max(A.X, B.X) / SegmentCellCm);
			const int32 MinY = FMath::FloorToInt(FMath::Min(A.Y, B.Y) / SegmentCellCm);
			const int32 MaxY = FMath::FloorToInt(FMath::Max(A.Y, B.Y) / SegmentCellCm);
			for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
			{
				for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
				{
					SegmentCells.FindOrAdd(CellKey(CellX, CellY)).Add((int64(WayIndex) << 32) | int64(PointIndex));
				}
			}
		}
	}
}

const FTrafficApproach* FTrafficNetwork::FindApproachById(int32 ApproachId) const
{
	const int32* Index = ApproachById.Find(ApproachId);
	return Index ? &Approaches[*Index] : nullptr;
}

FSignalState FTrafficNetwork::StateOfPhase(const FSignalJunction& Junction, int32 PhaseIndex, double TimeSeconds)
{
	const FSignalPhase& Phase = Junction.Phases[PhaseIndex];
	const float Cycle = Junction.CycleSeconds;
	const float Position = float(FMath::Fmod(TimeSeconds + Junction.OffsetSeconds, double(Cycle)));
	float Local = Position - Phase.StartSeconds;
	if (Local < 0.f)
	{
		Local += Cycle;
	}
	FSignalState State;
	if (Phase.bUnserved)
	{
		State.Aspect = ESignalAspect::Red;
		State.SecondsUntilChange = Phase.GreenSeconds;
		State.SecondsInState = Local;
		return State;
	}
	if (Local < Phase.GreenSeconds)
	{
		State.Aspect = ESignalAspect::Green;
		State.SecondsUntilChange = Phase.GreenSeconds - Local;
		State.SecondsInState = Local;
	}
	else if (Local < Phase.GreenSeconds + Phase.AmberSeconds)
	{
		State.Aspect = ESignalAspect::Amber;
		State.SecondsUntilChange = Phase.GreenSeconds + Phase.AmberSeconds - Local;
		State.SecondsInState = Local - Phase.GreenSeconds;
	}
	else if (Local < Cycle - RedAmberSeconds)
	{
		State.Aspect = ESignalAspect::Red;
		State.SecondsUntilChange = Cycle - RedAmberSeconds - Local;
		State.SecondsInState = Local - Phase.GreenSeconds - Phase.AmberSeconds;
	}
	else
	{
		State.Aspect = ESignalAspect::RedAmber;
		State.SecondsUntilChange = Cycle - Local;
		State.SecondsInState = Local - (Cycle - RedAmberSeconds);
	}
	return State;
}

FSignalState FTrafficNetwork::GetApproachState(int32 ApproachId, double TimeSeconds) const
{
	const FTrafficApproach* Approach = FindApproachById(ApproachId);
	if (!Approach || !Junctions.IsValidIndex(Approach->JunctionIndex))
	{
		return FSignalState();
	}
	const FSignalJunction& Junction = Junctions[Approach->JunctionIndex];
	if (!Junction.Phases.IsValidIndex(Approach->Phase))
	{
		return FSignalState();
	}
	return StateOfPhase(Junction, Approach->Phase, TimeSeconds);
}

bool FTrafficNetwork::FindApproachAhead(const FVector2D& Location, const FVector2D& Forward, float MaxDistanceCm, float BehindCm,
	double TimeSeconds, FApproachQuery& Out, int32 PreferApproachId) const
{
	const int32 Reach = FMath::CeilToInt((MaxDistanceCm + 500.f) / ApproachCellCm);
	const int32 CenterX = FMath::FloorToInt(Location.X / ApproachCellCm);
	const int32 CenterY = FMath::FloorToInt(Location.Y / ApproachCellCm);
	float BestDistance = TNumericLimits<float>::Max();
	int32 BestIndex = INDEX_NONE;
	for (int32 CellX = CenterX - Reach; CellX <= CenterX + Reach; ++CellX)
	{
		for (int32 CellY = CenterY - Reach; CellY <= CenterY + Reach; ++CellY)
		{
			const TArray<int32>* Cell = ApproachCells.Find(CellKey(CellX, CellY));
			if (!Cell)
			{
				continue;
			}
			for (const int32 Index : *Cell)
			{
				const FTrafficApproach& Approach = Approaches[Index];
				if (FVector2D::DotProduct(Approach.Direction, Forward) < MinimumHeadingDot)
				{
					continue;
				}
				const FVector2D Center = Approach.StopLineCenter();
				const float Along = float(FVector2D::DotProduct(Center - Location, Approach.Direction));
				if (Along > MaxDistanceCm || Along < -BehindCm)
				{
					continue;
				}
				const FVector2D Axis = Approach.StopLineRight - Approach.StopLineLeft;
				const float Width = float(Axis.Size());
				const float Across = float(FVector2D::DotProduct(Location - Approach.StopLineLeft, Axis * SafeInverse(Width)));
				if (Across < -StopLineMarginCm || Across > Width + StopLineMarginCm)
				{
					continue;
				}
				// Prefer the closest line ahead; a line just behind only wins when nothing is ahead.
				float Rank = Along >= 0.f ? Along : 100000.f - Along;
				if (Approach.Id == PreferApproachId)
				{
					Rank = -1.f; // the line already being tracked wins until it is out of range
				}
				if (Rank < BestDistance)
				{
					BestDistance = Rank;
					BestIndex = Index;
				}
			}
		}
	}
	if (BestIndex == INDEX_NONE)
	{
		return false;
	}
	const FTrafficApproach& Best = Approaches[BestIndex];
	Out.ApproachId = Best.Id;
	Out.DistanceToStopLineCm = float(FVector2D::DotProduct(Best.StopLineCenter() - Location, Best.Direction));
	Out.State = GetApproachState(Best.Id, TimeSeconds);
	return true;
}

FSpeedLimitResult FTrafficNetwork::GetSpeedLimit(const FVector2D& Location, const FVector2D& Forward) const
{
	FSpeedLimitResult Result;
	float BestScore = TNumericLimits<float>::Max();
	const int32 CellX = FMath::FloorToInt(Location.X / SegmentCellCm);
	const int32 CellY = FMath::FloorToInt(Location.Y / SegmentCellCm);
	for (int32 DX = -1; DX <= 1; ++DX)
	{
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			const TArray<int64>* Cell = SegmentCells.Find(CellKey(CellX + DX, CellY + DY));
			if (!Cell)
			{
				continue;
			}
			for (const int64 Packed : *Cell)
			{
				const FSpeedWay& Way = Ways[int32(Packed >> 32)];
				const int32 PointIndex = int32(Packed & 0xFFFFFFFF);
				FVector2D Direction;
				const float Distance = DistanceToSegment(Location, Way.Points[PointIndex], Way.Points[PointIndex + 1], Direction);
				if (Distance > Way.WidthCm * 0.5f + RoadMatchMarginCm)
				{
					continue;
				}
				const float Alignment = float(FVector2D::DotProduct(Direction, Forward));
				// A way crossing the heading, or a one-way street against it, is not the road being driven on.
				float Score = Distance;
				if (FMath::Abs(Alignment) < 0.5f || (Way.bOneway && Alignment < -0.5f))
				{
					Score += MisalignedPenaltyCm;
				}
				if (Score < BestScore)
				{
					BestScore = Score;
					Result.bFound = true;
					Result.LimitKmh = Way.LimitKmh;
					Result.WayId = Way.Id;
				}
			}
		}
	}
	return Result;
}
