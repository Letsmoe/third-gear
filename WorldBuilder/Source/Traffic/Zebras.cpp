#include "Zebras.h"

#include <algorithm>
#include <cmath>

namespace WorldBuilder
{
namespace
{
constexpr double CrossingReach = 25.0;
constexpr double StopLineInset = 0.15;
constexpr double StopLineBack = 0.25;

/** The convex hull of the points, counter-clockwise in a y-up frame (Andrew's monotone chain). */
std::vector<FStreetPoint> ConvexHull(std::vector<FStreetPoint> Points)
{
	std::sort(Points.begin(), Points.end(), [](const FStreetPoint& Left, const FStreetPoint& Right) {
		return Left.X < Right.X || (Left.X == Right.X && Left.Y < Right.Y);
	});
	const auto Cross = [](const FStreetPoint& Origin, const FStreetPoint& First, const FStreetPoint& Second) {
		return (First.X - Origin.X) * (Second.Y - Origin.Y) - (First.Y - Origin.Y) * (Second.X - Origin.X);
	};
	std::vector<FStreetPoint> Hull(2 * Points.size());
	size_t Count = 0;
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		while (Count >= 2 && Cross(Hull[Count - 2], Hull[Count - 1], Points[Index]) <= 0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index];
	}
	for (size_t Index = Points.size() - 1, Lower = Count + 1; Index > 0; --Index)
	{
		while (Count >= Lower && Cross(Hull[Count - 2], Hull[Count - 1], Points[Index - 1]) <= 0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index - 1];
	}
	Hull.resize(Count - 1);
	return Hull;
}
}

std::vector<std::pair<double, double>> ZebraSpans(const FZebra& Zebra, const FPolygonSet& Carriageway)
{
	const double Limit = Zebra.Width / 2 + ZebraWidthSlack;
	const double NormalX = -Zebra.Direction.Y;
	const double NormalY = Zebra.Direction.X;
	const FPolyline Cross = {{Zebra.X - NormalX * CrossingReach, Zebra.Y - NormalY * CrossingReach},
							 {Zebra.X + NormalX * CrossingReach, Zebra.Y + NormalY * CrossingReach}};
	std::vector<std::pair<double, double>> Spans;
	for (const FPolyline& Part : Carriageway.ClipLine(Cross))
	{
		if (Part.empty())
		{
			continue;
		}
		double Lowest = 1e300;
		double Highest = -1e300;
		for (const FWorldPoint& Point : Part)
		{
			const double Lateral = (Point.X - Zebra.X) * NormalX + (Point.Y - Zebra.Y) * NormalY;
			Lowest = std::min(Lowest, Lateral);
			Highest = std::max(Highest, Lateral);
		}
		const double Low = std::max(Lowest, -Limit);
		const double High = std::min(Highest, Limit);
		if (High - Low >= ZebraBarWidth)
		{
			Spans.emplace_back(Low, High);
		}
	}
	std::sort(Spans.begin(), Spans.end());
	return Spans;
}

FZebraBars MakeZebraBars(const FZebra& Zebra, const FPolygonSet& Carriageway)
{
	const double UnitX = Zebra.Direction.X;
	const double UnitY = Zebra.Direction.Y;
	const double NormalX = -UnitY;
	const double NormalY = UnitX;
	std::vector<std::pair<double, double>> Spans = ZebraSpans(Zebra, Carriageway);
	if (Spans.empty())
	{
		Spans.emplace_back(-Zebra.Width / 2, Zebra.Width / 2);
	}
	const double Length = Zebra.Width >= ZebraWideRoad ? ZebraBarLengthWideRoad : ZebraBarLength;
	FZebraBars Result;
	std::vector<FStreetPoint> Corners;
	for (const auto& [Low, High] : Spans)
	{
		const int Count = std::max(static_cast<int>((High - Low + ZebraBarWidth) / (2 * ZebraBarWidth)), 1);
		const double Used = Count * 2 * ZebraBarWidth - ZebraBarWidth;
		const double First = (Low + High) / 2 - Used / 2 + ZebraBarWidth / 2;
		Result.Spans.push_back({Low, High, First - ZebraBarWidth / 2,
								First + (Count - 1) * 2 * ZebraBarWidth + ZebraBarWidth / 2});
		for (int Index = 0; Index < Count; ++Index)
		{
			const double Lateral = First + Index * 2 * ZebraBarWidth;
			const double CentreX = Zebra.X + NormalX * Lateral;
			const double CentreY = Zebra.Y + NormalY * Lateral;
			const FStreetPoint Start = {CentreX - UnitX * Length / 2, CentreY - UnitY * Length / 2};
			const FStreetPoint End = {CentreX + UnitX * Length / 2, CentreY + UnitY * Length / 2};
			Result.Bars.push_back({Start, End});
			for (const FStreetPoint& End_ : {Start, End})
			{
				Corners.push_back({End_.X + NormalX * ZebraBarWidth, End_.Y + NormalY * ZebraBarWidth});
				Corners.push_back({End_.X - NormalX * ZebraBarWidth, End_.Y - NormalY * ZebraBarWidth});
			}
		}
	}
	Clipper2Lib::PathD Hull;
	for (const FStreetPoint& Corner : ConvexHull(Corners))
	{
		Hull.emplace_back(Corner.X, Corner.Y);
	}
	Result.Outline.push_back(std::move(Hull));
	return Result;
}

std::optional<FStreetPolyline> StopLineGeometry(const FApproachRecord& Approach)
{
	const double X0 = Approach.StopLine[0];
	const double Y0 = Approach.StopLine[1];
	const double X1 = Approach.StopLine[2];
	const double Y1 = Approach.StopLine[3];
	const double Length = std::hypot(X1 - X0, Y1 - Y0);
	if (Length < 1.0)
	{
		return std::nullopt;
	}
	const double UnitX = (X1 - X0) / Length;
	const double UnitY = (Y1 - Y0) / Length;
	const double BackX = -Approach.Direction[0] * StopLineBack;
	const double BackY = -Approach.Direction[1] * StopLineBack;
	return FStreetPolyline{{X0 + UnitX * StopLineInset + BackX, Y0 + UnitY * StopLineInset + BackY},
						   {X1 - UnitX * StopLineInset + BackX, Y1 - UnitY * StopLineInset + BackY}};
}
}
