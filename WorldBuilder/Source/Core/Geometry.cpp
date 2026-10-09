#include "Geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace WorldBuilder
{
namespace
{
/** Clipper2 works on integers; this many decimal places of a metre (0.1 mm) survive its operations. */
constexpr int ClipperPrecision = 4;

FPolyline ToPolyline(const Clipper2Lib::PathD& Path)
{
	FPolyline Ring;
	Ring.reserve(Path.size());
	for (const Clipper2Lib::PointD& Point : Path)
	{
		Ring.push_back({Point.x, Point.y});
	}
	return Ring;
}

/** Adds the polygons of one tree node that is an outline, then those of its islands, recursively. */
void CollectPolygons(const Clipper2Lib::PolyPathD& Outline, std::vector<FPolygonWithHoles>& Result)
{
	FPolygonWithHoles& Polygon = Result.emplace_back();
	Polygon.Rings.push_back(ToPolyline(Outline.Polygon()));
	for (const auto& Hole : Outline)
	{
		Polygon.Rings.push_back(ToPolyline(Hole->Polygon()));
	}
	for (const auto& Hole : Outline)
	{
		for (const auto& Island : *Hole)
		{
			CollectPolygons(*Island, Result);
		}
	}
}

/** Where a segment leaves and enters a box, as fractions along it (Liang and Barsky); false when it misses. */
bool ClipSegment(const FWorldPoint& Start, const FWorldPoint& End, const FBox& Box, double& Enter, double& Leave)
{
	const double DeltaX = End.X - Start.X;
	const double DeltaY = End.Y - Start.Y;
	const double Directions[4] = {-DeltaX, DeltaX, -DeltaY, DeltaY};
	const double Distances[4] = {Start.X - Box.X0, Box.X1 - Start.X, Start.Y - Box.Y0, Box.Y1 - Start.Y};
	Enter = 0.0;
	Leave = 1.0;
	for (int Side = 0; Side < 4; ++Side)
	{
		if (Directions[Side] == 0.0)
		{
			if (Distances[Side] < 0.0)
			{
				return false;
			}
			continue;
		}
		const double Fraction = Distances[Side] / Directions[Side];
		if (Directions[Side] < 0.0)
		{
			Enter = std::max(Enter, Fraction);
		}
		else
		{
			Leave = std::min(Leave, Fraction);
		}
	}
	return Enter <= Leave;
}

FWorldPoint Lerp(const FWorldPoint& Start, const FWorldPoint& End, double Fraction)
{
	return {Start.X + (End.X - Start.X) * Fraction, Start.Y + (End.Y - Start.Y) * Fraction};
}
}

double RingArea(const FPolyline& Ring)
{
	double Twice = 0.0;
	for (size_t Index = 0; Index < Ring.size(); ++Index)
	{
		const FWorldPoint& Point = Ring[Index];
		const FWorldPoint& Next = Ring[(Index + 1) % Ring.size()];
		Twice += Point.X * Next.Y - Next.X * Point.Y;
	}
	return Twice / 2.0;
}

double PolygonArea(const FPolygonWithHoles& Polygon)
{
	double Area = 0.0;
	for (size_t Index = 0; Index < Polygon.Rings.size(); ++Index)
	{
		const double RingSize = std::abs(RingArea(Polygon.Rings[Index]));
		Area += Index == 0 ? RingSize : -RingSize;
	}
	return Area;
}

FBox BoundsOf(const FPolygons& Polygons)
{
	FBox Box{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
			 std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
	for (const Clipper2Lib::PathD& Path : Polygons)
	{
		for (const Clipper2Lib::PointD& Point : Path)
		{
			Box.X0 = std::min(Box.X0, Point.x);
			Box.Y0 = std::min(Box.Y0, Point.y);
			Box.X1 = std::max(Box.X1, Point.x);
			Box.Y1 = std::max(Box.Y1, Point.y);
		}
	}
	return Box;
}

FPolygons ToPaths(const std::vector<FPolygonWithHoles>& Polygons)
{
	FPolygons Paths;
	for (const FPolygonWithHoles& Polygon : Polygons)
	{
		for (const FPolyline& Ring : Polygon.Rings)
		{
			Clipper2Lib::PathD& Path = Paths.emplace_back();
			Path.reserve(Ring.size());
			for (const FWorldPoint& Point : Ring)
			{
				Path.emplace_back(Point.X, Point.Y);
			}
		}
	}
	return Paths;
}

std::vector<FPolygonWithHoles> ToPolygons(const FPolygons& Paths)
{
	Clipper2Lib::ClipperD Clipper(ClipperPrecision);
	Clipper.AddSubject(Paths);
	Clipper2Lib::PolyTreeD Tree;
	Clipper.Execute(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::EvenOdd, Tree);
	std::vector<FPolygonWithHoles> Result;
	for (const auto& Outline : Tree)
	{
		CollectPolygons(*Outline, Result);
	}
	return Result;
}

FPolygons ClipToBox(const FPolygons& Paths, const FBox& Box)
{
	const FPolygons Rectangle = {{{Box.X0, Box.Y0}, {Box.X1, Box.Y0}, {Box.X1, Box.Y1}, {Box.X0, Box.Y1}}};
	return Clipper2Lib::Intersect(Paths, Rectangle, Clipper2Lib::FillRule::EvenOdd, ClipperPrecision);
}

std::vector<FPolylinePiece> ClipPolylineToBox(const FPolyline& Line, const FBox& Box)
{
	std::vector<FPolylinePiece> Pieces;
	FPolylinePiece* Open = nullptr;
	double Travelled = 0.0;
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		const FWorldPoint& Start = Line[Index];
		const FWorldPoint& End = Line[Index + 1];
		const double SegmentLength = std::hypot(End.X - Start.X, End.Y - Start.Y);
		double Enter = 0.0;
		double Leave = 0.0;
		if (!ClipSegment(Start, End, Box, Enter, Leave))
		{
			Open = nullptr;
			Travelled += SegmentLength;
			continue;
		}
		// A piece continues while the line stays inside from one segment to the next.
		if (Open == nullptr || Enter > 0.0)
		{
			Open = &Pieces.emplace_back();
			Open->StartDistance = Travelled + Enter * SegmentLength;
			Open->Points.push_back(Lerp(Start, End, Enter));
		}
		Open->Points.push_back(Lerp(Start, End, Leave));
		if (Leave < 1.0)
		{
			Open = nullptr;
		}
		Travelled += SegmentLength;
	}
	return Pieces;
}

double PolylineLength(const FPolyline& Line)
{
	double Length = 0.0;
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		Length += std::hypot(Line[Index + 1].X - Line[Index].X, Line[Index + 1].Y - Line[Index].Y);
	}
	return Length;
}
}

namespace WorldBuilder
{
namespace
{
/** Clipper2's arc tolerance for a radius and a number of segments per quarter circle. */
double ArcTolerance(double Radius, int SegmentsPerQuarter)
{
	const double HalfStep = 3.14159265358979323846 / (4.0 * SegmentsPerQuarter);
	return std::max(1e-4, std::abs(Radius) * (1.0 - std::cos(HalfStep)));
}

Clipper2Lib::PathsD AsPaths(const FPolyline& Line)
{
	Clipper2Lib::PathD Path;
	Path.reserve(Line.size());
	for (const FWorldPoint& Point : Line)
	{
		Path.emplace_back(Point.X, Point.Y);
	}
	return {Path};
}
}

FPolygons BufferPolyline(const FPolyline& Line, double Radius, ECapStyle Cap, int SegmentsPerQuarter)
{
	const Clipper2Lib::EndType End = Cap == ECapStyle::Round ? Clipper2Lib::EndType::Round : Clipper2Lib::EndType::Butt;
	return Clipper2Lib::InflatePaths(AsPaths(Line), Radius, Clipper2Lib::JoinType::Round, End, 2.0, ClipperPrecision,
									 ArcTolerance(Radius, SegmentsPerQuarter));
}

FPolygons OffsetPolygons(const FPolygons& Polygons, double Distance, int SegmentsPerQuarter)
{
	return Clipper2Lib::InflatePaths(Polygons, Distance, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon,
									 2.0, ClipperPrecision, ArcTolerance(Distance, SegmentsPerQuarter));
}

FPolygons UnionOf(const FPolygons& Polygons)
{
	return Clipper2Lib::Union(Polygons, Clipper2Lib::FillRule::NonZero, ClipperPrecision);
}

FPolygons Difference(const FPolygons& Subject, const FPolygons& Clip)
{
	return Clipper2Lib::Difference(Subject, Clip, Clipper2Lib::FillRule::NonZero, ClipperPrecision);
}

FPolygons Intersection(const FPolygons& Subject, const FPolygons& Clip)
{
	return Clipper2Lib::Intersect(Subject, Clip, Clipper2Lib::FillRule::NonZero, ClipperPrecision);
}

bool Contains(const FPolygons& Polygons, double X, double Y)
{
	bool bInside = false;
	for (const Clipper2Lib::PathD& Ring : Polygons)
	{
		for (size_t Index = 0, Previous = Ring.size() - 1; Index < Ring.size(); Previous = Index++)
		{
			const Clipper2Lib::PointD& Point = Ring[Index];
			const Clipper2Lib::PointD& Before = Ring[Previous];
			if ((Point.y > Y) != (Before.y > Y) && X < (Before.x - Point.x) * (Y - Point.y) / (Before.y - Point.y) + Point.x)
			{
				bInside = !bInside;
			}
		}
	}
	return bInside;
}
}
