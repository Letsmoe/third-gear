#pragma once

#include <vector>

#include "Projection.h"
#include "Raster.h"

/** Polygons and polylines in world metres, and the operations on them the tile output needs. */
namespace WorldBuilder
{
using FPolyline = std::vector<FWorldPoint>;

/** One polygon: the outline first, then its holes, each without a repeated end point. */
struct FPolygonWithHoles
{
	std::vector<FPolyline> Rings;
};

struct FBox
{
	double X0 = 0.0;
	double Y0 = 0.0;
	double X1 = 0.0;
	double Y1 = 0.0;

	bool Overlaps(const FBox& Other) const
	{
		return X0 <= Other.X1 && Other.X0 <= X1 && Y0 <= Other.Y1 && Other.Y0 <= Y1;
	}
};

/** Signed area of a ring by the shoelace formula, in the world frame (x east, y south). */
double RingArea(const FPolyline& Ring);

/** Area of a polygon: its outline minus its holes. */
double PolygonArea(const FPolygonWithHoles& Polygon);

/** The bounding box of a set of rings. */
FBox BoundsOf(const FPolygons& Polygons);

/** Clipper2 paths of polygons, all rings in one list (filled even-odd). */
FPolygons ToPaths(const std::vector<FPolygonWithHoles>& Polygons);

/** The polygons of a filled path set, each outline with the holes inside it. */
std::vector<FPolygonWithHoles> ToPolygons(const FPolygons& Paths);

/** The part of filled paths inside a box. */
FPolygons ClipToBox(const FPolygons& Paths, const FBox& Box);

/** The pieces of a polyline inside a box, each with its distance along the polyline at its start. */
struct FPolylinePiece
{
	FPolyline Points;
	double StartDistance = 0.0;
};
std::vector<FPolylinePiece> ClipPolylineToBox(const FPolyline& Line, const FBox& Box);

/** Length of a polyline. */
double PolylineLength(const FPolyline& Line);

enum class ECapStyle
{
	Round,
	Flat,
};

/**
 * The area within Radius of a polyline, as shapely's buffer with round joins: SegmentsPerQuarter segments per
 * quarter circle at the round caps and joins.
 */
FPolygons BufferPolyline(const FPolyline& Line, double Radius, ECapStyle Cap, int SegmentsPerQuarter);

/** Grows (positive) or shrinks (negative) filled polygons with round corners. */
FPolygons OffsetPolygons(const FPolygons& Polygons, double Distance, int SegmentsPerQuarter);

/** Boolean operations on filled polygons (non-zero filling, so overlapping pieces merge). */
FPolygons UnionOf(const FPolygons& Polygons);
FPolygons Difference(const FPolygons& Subject, const FPolygons& Clip);
FPolygons Intersection(const FPolygons& Subject, const FPolygons& Clip);

/** True when the point lies inside the filled polygons (even-odd). */
bool Contains(const FPolygons& Polygons, double X, double Y);
}
