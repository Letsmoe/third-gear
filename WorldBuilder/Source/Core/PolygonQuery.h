#pragma once

#include <optional>
#include <vector>

#include "Geometry.h"

namespace WorldBuilder
{
/**
 * Point queries on a set of filled polygons (even-odd), with their edges in square buckets: whether a point is inside,
 * and how far it is from the outline. Meant for a tile's window, where a few hundred to a few thousand points are
 * asked about polygons with many edges.
 */
class FPolygonQuery
{
public:
	explicit FPolygonQuery(const FPolygons& Polygons, double BucketSize = 4.0);

	bool Empty() const { return Edges.empty(); }

	/** True when the point lies inside (even-odd). */
	bool Contains(double X, double Y) const;

	/** Distance from the point to the nearest outline point, or Limit when none is within Limit. */
	double DistanceToOutline(double X, double Y, double Limit) const;

	/** Distance to the polygons: 0 inside, else to the outline, at most Limit. */
	double Distance(double X, double Y, double Limit) const;

	/** The nearest outline point within Limit, or nothing. */
	std::optional<FWorldPoint> NearestOutlinePoint(double X, double Y, double Limit) const;

private:
	struct FEdge
	{
		double X0;
		double Y0;
		double X1;
		double Y1;
	};
	std::vector<FEdge> Edges;
	double BucketSize;
	double OriginX = 0.0;
	double OriginY = 0.0;
	int Columns = 0;
	int Rows = 0;
	/** Edge indices per square bucket (row-major), and per row of buckets for the inside test. */
	std::vector<std::vector<int>> Buckets;
	std::vector<std::vector<int>> RowEdges;

	int ColumnOf(double X) const;
	int RowOf(double Y) const;
	/** Visits the edges in buckets within a square of Radius around a point, each possibly more than once. */
	template <typename FVisit>
	void ForEdgesNear(double X, double Y, double Radius, FVisit&& Visit) const;
};
}
