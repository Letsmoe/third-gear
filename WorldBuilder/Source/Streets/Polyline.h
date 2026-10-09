#pragma once

#include <cmath>
#include <utility>
#include <vector>

/**
 * Polyline helpers in the world frame (x east, y south): offsets, cuts, curves (Python: streets/polyline.py, which
 * works on numpy arrays).
 *
 * The frame is mirrored, so "right" here is the physical right of someone moving along the line.
 */
namespace WorldBuilder
{
/** A point or vector in the world frame, in metres. */
struct FStreetPoint
{
	double X = 0.0;
	double Y = 0.0;

	FStreetPoint operator+(const FStreetPoint& Other) const { return {X + Other.X, Y + Other.Y}; }
	FStreetPoint operator-(const FStreetPoint& Other) const { return {X - Other.X, Y - Other.Y}; }
	FStreetPoint operator-() const { return {-X, -Y}; }
	FStreetPoint operator*(double Scale) const { return {X * Scale, Y * Scale}; }
	FStreetPoint operator/(double Divisor) const { return {X / Divisor, Y / Divisor}; }
	bool operator==(const FStreetPoint& Other) const { return X == Other.X && Y == Other.Y; }
};

using FStreetPolyline = std::vector<FStreetPoint>;

/** The dot product of two vectors. */
inline double Dot(const FStreetPoint& First, const FStreetPoint& Second)
{
	return First.X * Second.X + First.Y * Second.Y;
}

/** The length of a vector. */
inline double Norm(const FStreetPoint& Vector)
{
	return std::hypot(Vector.X, Vector.Y);
}

namespace Polyline
{
/** The vector scaled to length one; (1, 0) for a zero vector. */
FStreetPoint Unit(const FStreetPoint& Vector);

/** The unit vector to the right of a direction. */
FStreetPoint RightOf(const FStreetPoint& Direction);

/** Cumulative distance along the polyline at every point. */
std::vector<double> Arclength(const FStreetPolyline& Line);

/** Length of the polyline. */
double Length(const FStreetPolyline& Line);

/**
 * Moves a polyline sideways by Distance to the right of its direction, with mitred corners. Distance is one number
 * for the whole line.
 */
FStreetPolyline OffsetPolyline(const FStreetPolyline& Line, double Distance);

/** Moves a polyline sideways by one distance per point, with mitred corners. */
FStreetPolyline OffsetPolyline(const FStreetPolyline& Line, const std::vector<double>& Distances);

/** Equally spaced points along a polyline, keeping its end points. */
FStreetPolyline Resample(const FStreetPolyline& Line, double Spacing);

/** The point at arc length S (clamped to the line). */
FStreetPoint PointAt(const FStreetPolyline& Line, double S);

/** PointAt for a line whose cumulative distances are already known (Arclength). */
FStreetPoint PointAt(const FStreetPolyline& Line, const std::vector<double>& Along, double S);

/** The unit direction of the polyline around arc length S, measured over Reach metres either side. */
FStreetPoint DirectionAt(const FStreetPolyline& Line, double S, double Reach = 2.0);

/** DirectionAt for a line whose cumulative distances are already known (Arclength). */
FStreetPoint DirectionAt(const FStreetPolyline& Line, const std::vector<double>& Along, double S, double Reach = 2.0);

/** The part of a polyline between two arc lengths. */
FStreetPolyline CutPolyline(const FStreetPolyline& Line, double StartS, double EndS);

/** Cubic Bezier curve sampled about every Spacing metres. */
FStreetPolyline Bezier(const FStreetPoint& P0, const FStreetPoint& P1, const FStreetPoint& P2, const FStreetPoint& P3,
					   double Spacing);

/**
 * A curve from Start, leaving along StartDirection, to End, arriving along EndDirection: a cubic Bezier whose handles
 * are a third of the gap long, so straight continuations stay straight.
 */
FStreetPolyline SmoothCurve(const FStreetPoint& Start, const FStreetPoint& StartDirection, const FStreetPoint& End,
							const FStreetPoint& EndDirection, double Spacing);

/** (arc length, distance) of the closest point of a polyline to a point; the first closest segment wins ties. */
std::pair<double, double> ProjectOnPolyline(const FStreetPolyline& Line, const FStreetPoint& Point);

/** The distance from a point to the nearest point of the polyline. */
double DistanceTo(const FStreetPolyline& Line, const FStreetPoint& Point);

/** The distance from a point to the segment between two points. */
double DistanceToSegment(const FStreetPoint& Start, const FStreetPoint& End, const FStreetPoint& Point);

/** Linear interpolation like numpy.interp: Values at the sorted Positions, clamped past both ends. */
double Interpolate(double Target, const std::vector<double>& Positions, const std::vector<double>& Values);

/** The reversed polyline. */
FStreetPolyline Reversed(const FStreetPolyline& Line);
}
}
