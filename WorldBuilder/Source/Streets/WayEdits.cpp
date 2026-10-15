#include "WayEdits.h"

namespace WorldBuilder
{
void FWayEdits::AddPoints(int64_t First, int64_t Second, const FStreetPolyline& Line, double StartS, double EndS,
						  double Spacing)
{
	const std::vector<double> Along = Polyline::Arclength(Line);
	const int Count = static_cast<int>((EndS - StartS) / Spacing);
	std::vector<FAddedPoint> Points;
	for (int Step = 1; Step <= Count; ++Step)
	{
		Points.push_back({NextSyntheticId--, Polyline::PointAt(Line, Along, StartS + (EndS - StartS) * Step / (Count + 1))});
	}
	AddedPoints[{First, Second}] = Points;
	AddedPoints[{Second, First}] = std::vector<FAddedPoint>(Points.rbegin(), Points.rend());
}

FStreetWay WithEdits(const FStreetWay& Way, const FWayEdits& Edits)
{
	bool bTouched = false;
	for (size_t Index = 0; Index < Way.NodeIds.size() && !bTouched; ++Index)
	{
		bTouched = Edits.MovedNodes.count(Way.NodeIds[Index]) > 0;
		if (Index > 0)
		{
			bTouched = bTouched || Edits.AddedPoints.count({Way.NodeIds[Index - 1], Way.NodeIds[Index]}) > 0;
		}
	}
	if (!bTouched)
	{
		return Way;
	}
	FStreetWay Result;
	Result.Id = Way.Id;
	Result.Tags = Way.Tags;
	for (size_t Index = 0; Index < Way.NodeIds.size(); ++Index)
	{
		if (Index > 0)
		{
			const auto Added = Edits.AddedPoints.find({Way.NodeIds[Index - 1], Way.NodeIds[Index]});
			if (Added != Edits.AddedPoints.end())
			{
				for (const FAddedPoint& Point : Added->second)
				{
					Result.NodeIds.push_back(Point.NodeId);
					Result.Points.push_back(Point.Point);
				}
			}
		}
		Result.NodeIds.push_back(Way.NodeIds[Index]);
		const auto Moved = Edits.MovedNodes.find(Way.NodeIds[Index]);
		Result.Points.push_back(Moved != Edits.MovedNodes.end() ? Moved->second : Way.Points[Index]);
	}
	return Result;
}

std::vector<FStreetWay> WithEdits(const std::vector<FStreetWay>& Ways, const FWayEdits& Edits)
{
	std::vector<FStreetWay> Result;
	Result.reserve(Ways.size());
	for (const FStreetWay& Way : Ways)
	{
		Result.push_back(WithEdits(Way, Edits));
	}
	return Result;
}
}
