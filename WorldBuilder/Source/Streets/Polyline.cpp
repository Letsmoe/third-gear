#include "Polyline.h"

#include <algorithm>

namespace WorldBuilder::Polyline
{
FStreetPoint Unit(const FStreetPoint& Vector)
{
	const double Norm = std::hypot(Vector.X, Vector.Y);
	if (Norm > 1e-9)
	{
		return Vector / Norm;
	}
	return {1.0, 0.0};
}

FStreetPoint RightOf(const FStreetPoint& Direction)
{
	return {-Direction.Y, Direction.X};
}

std::vector<double> Arclength(const FStreetPolyline& Line)
{
	std::vector<double> Along;
	Along.reserve(Line.size());
	double Total = 0.0;
	Along.push_back(0.0);
	for (size_t Index = 1; Index < Line.size(); ++Index)
	{
		Total += std::hypot(Line[Index].X - Line[Index - 1].X, Line[Index].Y - Line[Index - 1].Y);
		Along.push_back(Total);
	}
	return Along;
}

double Length(const FStreetPolyline& Line)
{
	double Total = 0.0;
	for (size_t Index = 1; Index < Line.size(); ++Index)
	{
		Total += std::hypot(Line[Index].X - Line[Index - 1].X, Line[Index].Y - Line[Index - 1].Y);
	}
	return Total;
}

double Interpolate(double Target, const std::vector<double>& Positions, const std::vector<double>& Values)
{
	if (Target <= Positions.front())
	{
		return Values.front();
	}
	if (Target >= Positions.back())
	{
		return Values.back();
	}
	// The last index whose position is not past the target.
	const size_t Index = static_cast<size_t>(std::upper_bound(Positions.begin(), Positions.end(), Target) - Positions.begin()) - 1;
	if (Target == Positions[Index])
	{
		return Values[Index];
	}
	const double Slope = (Values[Index + 1] - Values[Index]) / (Positions[Index + 1] - Positions[Index]);
	return Slope * (Target - Positions[Index]) + Values[Index];
}

namespace
{
/** The point at arc length S of a line whose cumulative distances are known, interpolating between points. */
FStreetPoint InterpolatedPoint(const FStreetPolyline& Line, const std::vector<double>& Along, double S)
{
	if (S <= Along.front())
	{
		return Line.front();
	}
	if (S >= Along.back())
	{
		return Line.back();
	}
	const size_t Index = static_cast<size_t>(std::upper_bound(Along.begin(), Along.end(), S) - Along.begin()) - 1;
	if (S == Along[Index])
	{
		return Line[Index];
	}
	const double Span = Along[Index + 1] - Along[Index];
	const double SlopeX = (Line[Index + 1].X - Line[Index].X) / Span;
	const double SlopeY = (Line[Index + 1].Y - Line[Index].Y) / Span;
	return {SlopeX * (S - Along[Index]) + Line[Index].X, SlopeY * (S - Along[Index]) + Line[Index].Y};
}

/** The line moved sideways by one distance for all points, or by one per point, with mitred corners. */
FStreetPolyline OffsetWith(const FStreetPolyline& Line, const std::vector<double>& Distances, bool bPerPoint,
						   double SingleDistance)
{
	std::vector<FStreetPoint> Normals;
	for (size_t Index = 1; Index < Line.size(); ++Index)
	{
		const FStreetPoint Step = Line[Index] - Line[Index - 1];
		const double StepLength = std::hypot(Step.X, Step.Y);
		if (StepLength > 1e-9)
		{
			const FStreetPoint Direction = Step / StepLength;
			Normals.push_back({-Direction.Y, Direction.X});
		}
	}
	if (Normals.empty())
	{
		return Line;
	}
	const size_t Count = Line.size();
	FStreetPolyline Result(Count);
	for (size_t Index = 0; Index < Count; ++Index)
	{
		FStreetPoint VertexNormal = Normals.front();
		double Scale = 1.0;
		if (Index == Count - 1)
		{
			VertexNormal = Normals.back();
		}
		else if (Index > 0)
		{
			const FStreetPoint& Before = Normals[std::min(Index - 1, Normals.size() - 1)];
			const FStreetPoint& After = Normals[std::min(Index, Normals.size() - 1)];
			VertexNormal = Unit(Before + After);
			Scale = 1.0 / std::max(Dot(VertexNormal, Before), 0.5);
		}
		const double Distance = bPerPoint ? Distances[Index] : SingleDistance;
		Result[Index] = Line[Index] + VertexNormal * (Distance * Scale);
	}
	return Result;
}
}

FStreetPolyline OffsetPolyline(const FStreetPolyline& Line, double Distance)
{
	if (std::abs(Distance) < 1e-6)
	{
		return Line;
	}
	return OffsetWith(Line, {}, false, Distance);
}

FStreetPolyline OffsetPolyline(const FStreetPolyline& Line, const std::vector<double>& Distances)
{
	return OffsetWith(Line, Distances, true, 0.0);
}

FStreetPolyline Resample(const FStreetPolyline& Line, double Spacing)
{
	const std::vector<double> Along = Arclength(Line);
	const double Total = Along.back();
	if (Total < 1e-6)
	{
		return FStreetPolyline(Line.begin(), Line.begin() + 1);
	}
	const int Count = std::max(static_cast<int>(std::nearbyint(Total / Spacing)), 1) + 1;
	const double Step = Total / (Count - 1);
	FStreetPolyline Result;
	Result.reserve(Count);
	for (int Index = 0; Index < Count; ++Index)
	{
		const double Target = Index == Count - 1 ? Total : Index * Step;
		Result.push_back(InterpolatedPoint(Line, Along, Target));
	}
	return Result;
}

FStreetPoint PointAt(const FStreetPolyline& Line, const std::vector<double>& Along, double S)
{
	S = std::min(std::max(S, 0.0), Along.back());
	return InterpolatedPoint(Line, Along, S);
}

FStreetPoint PointAt(const FStreetPolyline& Line, double S)
{
	return PointAt(Line, Arclength(Line), S);
}

FStreetPoint DirectionAt(const FStreetPolyline& Line, const std::vector<double>& Along, double S, double Reach)
{
	const double Total = Along.back();
	const FStreetPoint Ahead = PointAt(Line, Along, std::min(S + Reach, Total));
	const FStreetPoint Behind = PointAt(Line, Along, std::max(S - Reach, 0.0));
	return Unit(Ahead - Behind);
}

FStreetPoint DirectionAt(const FStreetPolyline& Line, double S, double Reach)
{
	return DirectionAt(Line, Arclength(Line), S, Reach);
}

FStreetPolyline CutPolyline(const FStreetPolyline& Line, double StartS, double EndS)
{
	const std::vector<double> Along = Arclength(Line);
	const double Total = Along.back();
	StartS = std::min(std::max(StartS, 0.0), Total);
	EndS = std::min(std::max(EndS, StartS), Total);
	FStreetPolyline Result;
	Result.push_back(InterpolatedPoint(Line, Along, StartS));
	for (size_t Index = 0; Index < Line.size(); ++Index)
	{
		if (Along[Index] > StartS + 1e-6 && Along[Index] < EndS - 1e-6)
		{
			Result.push_back(Line[Index]);
		}
	}
	Result.push_back(InterpolatedPoint(Line, Along, EndS));
	return Result;
}

FStreetPolyline Bezier(const FStreetPoint& P0, const FStreetPoint& P1, const FStreetPoint& P2, const FStreetPoint& P3,
					   double Spacing)
{
	constexpr int CoarseCount = 40;
	FStreetPolyline Coarse;
	Coarse.reserve(CoarseCount);
	for (int Index = 0; Index < CoarseCount; ++Index)
	{
		const double T = Index == CoarseCount - 1 ? 1.0 : Index * (1.0 / (CoarseCount - 1));
		const double Inverse = 1 - T;
		Coarse.push_back(P0 * (Inverse * Inverse * Inverse) + P1 * (3 * Inverse * Inverse * T)
						 + P2 * (3 * Inverse * T * T) + P3 * (T * T * T));
	}
	return Resample(Coarse, Spacing);
}

FStreetPolyline SmoothCurve(const FStreetPoint& Start, const FStreetPoint& StartDirection, const FStreetPoint& End,
							const FStreetPoint& EndDirection, double Spacing)
{
	const double Reach = Norm(End - Start) / 3.0;
	return Bezier(Start, Start + StartDirection * Reach, End - EndDirection * Reach, End, Spacing);
}

double DistanceToSegment(const FStreetPoint& Start, const FStreetPoint& End, const FStreetPoint& Point)
{
	const FStreetPoint Edge = End - Start;
	const double Squared = std::max(Edge.X * Edge.X + Edge.Y * Edge.Y, 1e-12);
	const FStreetPoint Relative = Point - Start;
	const double T = std::min(std::max((Relative.X * Edge.X + Relative.Y * Edge.Y) / Squared, 0.0), 1.0);
	const FStreetPoint Nearest = Start + Edge * T;
	return std::hypot(Point.X - Nearest.X, Point.Y - Nearest.Y);
}

double DistanceTo(const FStreetPolyline& Line, const FStreetPoint& Point)
{
	double Best = 1e300;
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		Best = std::min(Best, DistanceToSegment(Line[Index], Line[Index + 1], Point));
	}
	return Best;
}

FStreetPolyline Reversed(const FStreetPolyline& Line)
{
	return FStreetPolyline(Line.rbegin(), Line.rend());
}
}
