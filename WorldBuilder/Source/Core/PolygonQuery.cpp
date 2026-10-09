#include "PolygonQuery.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace WorldBuilder
{
namespace
{
/** The point on a segment nearest to a point, and the squared distance to it. */
FWorldPoint NearestOnSegment(double X, double Y, double X0, double Y0, double X1, double Y1, double& DistanceSquared)
{
	const double DeltaX = X1 - X0;
	const double DeltaY = Y1 - Y0;
	const double LengthSquared = DeltaX * DeltaX + DeltaY * DeltaY;
	double Along = 0.0;
	if (LengthSquared > 0.0)
	{
		Along = std::clamp(((X - X0) * DeltaX + (Y - Y0) * DeltaY) / LengthSquared, 0.0, 1.0);
	}
	const FWorldPoint Nearest{X0 + DeltaX * Along, Y0 + DeltaY * Along};
	DistanceSquared = (Nearest.X - X) * (Nearest.X - X) + (Nearest.Y - Y) * (Nearest.Y - Y);
	return Nearest;
}
}

FPolygonQuery::FPolygonQuery(const FPolygons& Polygons, double InBucketSize) : BucketSize(InBucketSize)
{
	for (const Clipper2Lib::PathD& Ring : Polygons)
	{
		for (size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const Clipper2Lib::PointD& Start = Ring[Index];
			const Clipper2Lib::PointD& End = Ring[(Index + 1) % Ring.size()];
			Edges.push_back({Start.x, Start.y, End.x, End.y});
		}
	}
	if (Edges.empty())
	{
		return;
	}
	const FBox Bounds = BoundsOf(Polygons);
	OriginX = Bounds.X0;
	OriginY = Bounds.Y0;
	Columns = std::max(1, static_cast<int>(std::ceil((Bounds.X1 - Bounds.X0) / BucketSize)) + 1);
	Rows = std::max(1, static_cast<int>(std::ceil((Bounds.Y1 - Bounds.Y0) / BucketSize)) + 1);
	Buckets.resize(static_cast<size_t>(Columns) * Rows);
	RowEdges.resize(Rows);
	for (int Item = 0; Item < static_cast<int>(Edges.size()); ++Item)
	{
		const FEdge& Edge = Edges[Item];
		const int FirstColumn = ColumnOf(std::min(Edge.X0, Edge.X1));
		const int LastColumn = ColumnOf(std::max(Edge.X0, Edge.X1));
		const int FirstRow = RowOf(std::min(Edge.Y0, Edge.Y1));
		const int LastRow = RowOf(std::max(Edge.Y0, Edge.Y1));
		for (int Row = FirstRow; Row <= LastRow; ++Row)
		{
			RowEdges[Row].push_back(Item);
			for (int Column = FirstColumn; Column <= LastColumn; ++Column)
			{
				Buckets[static_cast<size_t>(Row) * Columns + Column].push_back(Item);
			}
		}
	}
}

int FPolygonQuery::ColumnOf(double X) const
{
	return std::clamp(static_cast<int>(std::floor((X - OriginX) / BucketSize)), 0, Columns - 1);
}

int FPolygonQuery::RowOf(double Y) const
{
	return std::clamp(static_cast<int>(std::floor((Y - OriginY) / BucketSize)), 0, Rows - 1);
}

template <typename FVisit>
void FPolygonQuery::ForEdgesNear(double X, double Y, double Radius, FVisit&& Visit) const
{
	const int FirstColumn = ColumnOf(X - Radius);
	const int LastColumn = ColumnOf(X + Radius);
	const int FirstRow = RowOf(Y - Radius);
	const int LastRow = RowOf(Y + Radius);
	for (int Row = FirstRow; Row <= LastRow; ++Row)
	{
		for (int Column = FirstColumn; Column <= LastColumn; ++Column)
		{
			for (const int Item : Buckets[static_cast<size_t>(Row) * Columns + Column])
			{
				Visit(Edges[Item]);
			}
		}
	}
}

bool FPolygonQuery::Contains(double X, double Y) const
{
	if (Edges.empty() || Y < OriginY || Y > OriginY + Rows * BucketSize)
	{
		return false;
	}
	// Crossings of a ray to the east, among the edges that span this point's row of buckets.
	bool bInside = false;
	for (const int Item : RowEdges[RowOf(Y)])
	{
		const FEdge& Edge = Edges[Item];
		if ((Edge.Y0 > Y) == (Edge.Y1 > Y))
		{
			continue;
		}
		const double CrossingX = Edge.X0 + (Y - Edge.Y0) / (Edge.Y1 - Edge.Y0) * (Edge.X1 - Edge.X0);
		if (X < CrossingX)
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

double FPolygonQuery::DistanceToOutline(double X, double Y, double Limit) const
{
	if (Edges.empty())
	{
		return Limit;
	}
	double Best = Limit * Limit;
	ForEdgesNear(X, Y, Limit, [&](const FEdge& Edge) {
		double DistanceSquared = 0.0;
		NearestOnSegment(X, Y, Edge.X0, Edge.Y0, Edge.X1, Edge.Y1, DistanceSquared);
		Best = std::min(Best, DistanceSquared);
	});
	return std::sqrt(Best);
}

double FPolygonQuery::Distance(double X, double Y, double Limit) const
{
	if (Contains(X, Y))
	{
		return 0.0;
	}
	return DistanceToOutline(X, Y, Limit);
}

std::optional<FWorldPoint> FPolygonQuery::NearestOutlinePoint(double X, double Y, double Limit) const
{
	double Best = Limit * Limit;
	std::optional<FWorldPoint> Nearest;
	ForEdgesNear(X, Y, Limit, [&](const FEdge& Edge) {
		double DistanceSquared = 0.0;
		const FWorldPoint Candidate = NearestOnSegment(X, Y, Edge.X0, Edge.Y0, Edge.X1, Edge.Y1, DistanceSquared);
		if (DistanceSquared < Best)
		{
			Best = DistanceSquared;
			Nearest = Candidate;
		}
	});
	return Nearest;
}
}
