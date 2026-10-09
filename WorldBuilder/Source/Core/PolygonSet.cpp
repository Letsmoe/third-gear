#include "PolygonSet.h"

#include <algorithm>
#include <cmath>

namespace WorldBuilder
{
namespace
{
/** Squared distance from a point to a segment. */
double SegmentDistanceSquared(double X, double Y, const Clipper2Lib::PointD& Start, const Clipper2Lib::PointD& End)
{
	const double DeltaX = End.x - Start.x;
	const double DeltaY = End.y - Start.y;
	const double LengthSquared = DeltaX * DeltaX + DeltaY * DeltaY;
	double Along = 0.0;
	if (LengthSquared > 0.0)
	{
		Along = std::clamp(((X - Start.x) * DeltaX + (Y - Start.y) * DeltaY) / LengthSquared, 0.0, 1.0);
	}
	const double NearestX = Start.x + DeltaX * Along - X;
	const double NearestY = Start.y + DeltaY * Along - Y;
	return NearestX * NearestX + NearestY * NearestY;
}

FBox PointBox(double X, double Y, double Reach)
{
	return {X - Reach, Y - Reach, X + Reach, Y + Reach};
}
}

void FPolygonSet::Add(FPolygons Piece)
{
	if (Piece.empty())
	{
		return;
	}
	Index.Insert(BoundsOf(Piece), static_cast<int>(Pieces.size()));
	Pieces.push_back(std::move(Piece));
}

bool FPolygonSet::Contains(double X, double Y) const
{
	for (const int Item : Index.Query(PointBox(X, Y, 0.0)))
	{
		if (WorldBuilder::Contains(Pieces[Item], X, Y))
		{
			return true;
		}
	}
	return false;
}

double FPolygonSet::Distance(double X, double Y, double Limit) const
{
	double Best = Limit * Limit;
	for (const int Item : Index.Query(PointBox(X, Y, Limit)))
	{
		const FPolygons& Piece = Pieces[Item];
		if (WorldBuilder::Contains(Piece, X, Y))
		{
			return 0.0;
		}
		for (const Clipper2Lib::PathD& Ring : Piece)
		{
			for (size_t Point = 0; Point < Ring.size(); ++Point)
			{
				Best = std::min(Best, SegmentDistanceSquared(X, Y, Ring[Point], Ring[(Point + 1) % Ring.size()]));
			}
		}
	}
	return std::sqrt(Best);
}

FPolygons FPolygonSet::InWindow(const FBox& Window) const
{
	FPolygons Clipped;
	for (const int Item : Index.Query(Window))
	{
		const FPolygons Part = ClipToBox(Pieces[Item], Window);
		Clipped.insert(Clipped.end(), Part.begin(), Part.end());
	}
	return UnionOf(Clipped);
}

double FPolygonSet::CoveredArea(const FPolygons& Polygon) const
{
	const FPolygons Covered = Intersection(UnionOf(Polygon), InWindow(BoundsOf(Polygon)));
	double Area = 0.0;
	for (const FPolygonWithHoles& Part : ToPolygons(Covered))
	{
		Area += PolygonArea(Part);
	}
	return Area;
}

std::vector<FPolyline> FPolygonSet::ClipLine(const FPolyline& Line) const
{
	if (Line.size() < 2)
	{
		return {};
	}
	FBox Bounds{Line[0].X, Line[0].Y, Line[0].X, Line[0].Y};
	Clipper2Lib::PathsD Open(1);
	for (const FWorldPoint& Point : Line)
	{
		Bounds.X0 = std::min(Bounds.X0, Point.X);
		Bounds.Y0 = std::min(Bounds.Y0, Point.Y);
		Bounds.X1 = std::max(Bounds.X1, Point.X);
		Bounds.Y1 = std::max(Bounds.Y1, Point.Y);
		Open[0].emplace_back(Point.X, Point.Y);
	}
	Clipper2Lib::ClipperD Clipper(4);
	Clipper.AddOpenSubject(Open);
	Clipper.AddClip(InWindow({Bounds.X0 - 1.0, Bounds.Y0 - 1.0, Bounds.X1 + 1.0, Bounds.Y1 + 1.0}));
	Clipper2Lib::PathsD Closed;
	Clipper2Lib::PathsD Inside;
	Clipper.Execute(Clipper2Lib::ClipType::Intersection, Clipper2Lib::FillRule::NonZero, Closed, Inside);
	std::vector<FPolyline> Result;
	for (const Clipper2Lib::PathD& Path : Inside)
	{
		FPolyline& Piece = Result.emplace_back();
		for (const Clipper2Lib::PointD& Point : Path)
		{
			Piece.push_back({Point.x, Point.y});
		}
	}
	return Result;
}
}
