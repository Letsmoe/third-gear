#include "LaneNetwork.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
ELaneControl ParseControl(const FString& Name)
{
	if (Name == TEXT("signal"))
	{
		return ELaneControl::Signal;
	}
	if (Name == TEXT("priority"))
	{
		return ELaneControl::Priority;
	}
	if (Name == TEXT("equal"))
	{
		return ELaneControl::Equal;
	}
	if (Name == TEXT("yield"))
	{
		return ELaneControl::Yield;
	}
	if (Name == TEXT("stop"))
	{
		return ELaneControl::Stop;
	}
	return ELaneControl::None;
}

/** Reads the flat x, y, z list of a lane into points and cumulative arc lengths. */
void ReadPoints(const TArray<TSharedPtr<FJsonValue>>& Flat, FTrafficLane& Lane)
{
	const int32 Count = Flat.Num() / 3;
	Lane.Points.Reserve(Count);
	Lane.Arc.Reserve(Count);
	float Travelled = 0.f;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector Point(Flat[Index * 3]->AsNumber(), Flat[Index * 3 + 1]->AsNumber(), Flat[Index * 3 + 2]->AsNumber());
		if (Index > 0)
		{
			Travelled += float(FVector::Dist(Point, Lane.Points.Last()));
		}
		Lane.Points.Add(Point);
		Lane.Arc.Add(Travelled);
	}
}

void ReadConflicts(const TArray<TSharedPtr<FJsonValue>>& Entries, FTrafficLane& Lane)
{
	for (const TSharedPtr<FJsonValue>& Entry : Entries)
	{
		const TArray<TSharedPtr<FJsonValue>>& Fields = Entry->AsArray();
		FLaneConflict& Conflict = Lane.Conflicts.AddDefaulted_GetRef();
		Conflict.OtherLane = int32(Fields[0]->AsNumber());
		Conflict.StartMeters = float(Fields[1]->AsNumber());
		Conflict.EndMeters = float(Fields[2]->AsNumber());
		Conflict.OtherStartMeters = float(Fields[3]->AsNumber());
		Conflict.OtherEndMeters = float(Fields[4]->AsNumber());
		Conflict.YieldKind = int32(Fields[5]->AsNumber());
	}
}

void ReadLane(const TSharedPtr<FJsonObject>& Entry, FTrafficLane& Lane)
{
	Lane.Id = Entry->GetIntegerField(TEXT("id"));
	Lane.Kind = ELaneKind(Entry->GetIntegerField(TEXT("kind")));
	Lane.WayId = int64(Entry->GetNumberField(TEXT("way")));
	Lane.LimitKmh = Entry->GetNumberField(TEXT("limit"));
	Lane.Tier = Entry->GetIntegerField(TEXT("tier"));
	Lane.bGood = Entry->GetIntegerField(TEXT("good")) != 0;
	Lane.bRoundabout = Entry->HasField(TEXT("ring"));
	ReadPoints(Entry->GetArrayField(TEXT("pts")), Lane);
	for (const TSharedPtr<FJsonValue>& Value : Entry->GetArrayField(TEXT("next")))
	{
		Lane.Next.Add(int32(Value->AsNumber()));
	}
	const TArray<TSharedPtr<FJsonValue>>* CurveSpeeds = nullptr;
	if (Entry->TryGetArrayField(TEXT("vc"), CurveSpeeds))
	{
		for (const TSharedPtr<FJsonValue>& Value : *CurveSpeeds)
		{
			Lane.CurveSpeedMs.Add(float(Value->AsNumber()) / 3.6f);
		}
	}
	if (Lane.Kind == ELaneKind::Road)
	{
		FString Control;
		if (Entry->TryGetStringField(TEXT("ctl"), Control))
		{
			Lane.Control = ParseControl(Control);
		}
		const TArray<TSharedPtr<FJsonValue>>* Stops = nullptr;
		if (Entry->TryGetArrayField(TEXT("stops"), Stops))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Stops)
			{
				const TArray<TSharedPtr<FJsonValue>>& Fields = Value->AsArray();
				Lane.Stops.Add({float(Fields[0]->AsNumber()), int32(Fields[1]->AsNumber())});
			}
		}
		return;
	}
	Lane.FromLane = Entry->GetIntegerField(TEXT("from"));
	Lane.ToLane = Entry->GetIntegerField(TEXT("to"));
	Lane.Turn = Entry->GetIntegerField(TEXT("turn"));
	const TArray<TSharedPtr<FJsonValue>>* Conflicts = nullptr;
	if (Entry->TryGetArrayField(TEXT("conf"), Conflicts))
	{
		ReadConflicts(*Conflicts, Lane);
	}
}
}

bool FLaneNetwork::Load(const FString& Path, FString& Error)
{
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		Error = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	Lanes.Reset();
	SpawnLanes.Reset();
	const TArray<TSharedPtr<FJsonValue>>& Entries = Root->GetArrayField(TEXT("lanes"));
	Lanes.SetNum(Entries.Num());
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		ReadLane(Entries[Index]->AsObject(), Lanes[Index]);
		if (Lanes[Index].Kind == ELaneKind::Road && Lanes[Index].bGood && Lanes[Index].Length() > 20.f)
		{
			SpawnLanes.Add(Index);
		}
	}
	return true;
}

int32 FLaneNetwork::FindPointIndex(const FTrafficLane& Lane, float S) const
{
	const int32 Count = Lane.Arc.Num();
	if (Count < 2 || S <= 0.f)
	{
		return 0;
	}
	if (S >= Lane.Arc.Last())
	{
		return Count - 2;
	}
	int32 Low = 0;
	int32 High = Count - 1;
	while (High - Low > 1)
	{
		const int32 Middle = (Low + High) / 2;
		if (Lane.Arc[Middle] <= S)
		{
			Low = Middle;
		}
		else
		{
			High = Middle;
		}
	}
	return Low;
}

FVector FLaneNetwork::PositionAt(const FTrafficLane& Lane, float S) const
{
	if (Lane.Points.Num() == 0)
	{
		return FVector::ZeroVector;
	}
	if (Lane.Points.Num() == 1)
	{
		return Lane.Points[0];
	}
	const int32 Index = FindPointIndex(Lane, S);
	const float Span = FMath::Max(Lane.Arc[Index + 1] - Lane.Arc[Index], 1e-4f);
	const float Alpha = FMath::Clamp((S - Lane.Arc[Index]) / Span, 0.f, 1.f);
	return FMath::Lerp(Lane.Points[Index], Lane.Points[Index + 1], double(Alpha));
}

FVector2D FLaneNetwork::DirectionAt(const FTrafficLane& Lane, float S) const
{
	if (Lane.Points.Num() < 2)
	{
		return FVector2D(1.0, 0.0);
	}
	// Central difference over two neighbouring segments so the heading turns smoothly along the polyline.
	const int32 Index = FindPointIndex(Lane, S);
	const int32 Before = FMath::Max(Index - 1, 0);
	const int32 After = FMath::Min(Index + 2, Lane.Points.Num() - 1);
	const FVector Delta = Lane.Points[After] - Lane.Points[Before];
	return FVector2D(Delta.X, Delta.Y).GetSafeNormal();
}
