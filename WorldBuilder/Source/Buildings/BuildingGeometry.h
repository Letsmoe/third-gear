#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "Projection.h"

/**
 * The polygon operations the building code takes from shapely: simplify, validity, minimum rotated rectangle,
 * centroid, representative point and distances, and an STR tree whose query order matches GEOS (the order of the
 * results decides which building a row of attached buildings takes its random draws from).
 */
namespace WorldBuilder
{
/** A closed ring without a repeated end point. */
using FRing = std::vector<FWorldPoint>;

/** One polygon: the outline and its holes. */
struct FBuildingPolygon
{
	FRing Outline;
	std::vector<FRing> Holes;
};

/** An axis aligned box. */
struct FBox2
{
	double MinX = 1e300;
	double MinY = 1e300;
	double MaxX = -1e300;
	double MaxY = -1e300;

	/** Grows the box to contain the point. */
	void Add(const FWorldPoint& Point);
	/** True when the boxes overlap or touch. */
	bool Intersects(const FBox2& Other) const;
	/** The box grown by Distance on every side. */
	FBox2 Expanded(double Distance) const;
	/** The distance between the boxes, 0 when they overlap. */
	double DistanceTo(const FBox2& Other) const;
};

/** Where a point lies relative to a polygon. */
enum class EPointLocation
{
	Outside,
	Boundary,
	Inside,
};

FBox2 BoundingBox(const FRing& Ring);
FBox2 BoundingBox(const FBuildingPolygon& Polygon);

/** Signed area of a ring, positive when it runs counter-clockwise in a y-up frame. */
double SignedArea(const FRing& Ring);

/** Area of the polygon: the outline minus the holes. */
double PolygonArea(const FBuildingPolygon& Polygon);

/** Area centroid of the polygon (shapely's centroid). */
FWorldPoint PolygonCentroid(const FBuildingPolygon& Polygon);

/** GEOS's interior point of a polygon: the middle of the widest span along a horizontal line through the polygon. */
FWorldPoint RepresentativePoint(const FBuildingPolygon& Polygon);

/** Classifies a point against the polygon, holes included. */
EPointLocation LocatePoint(const FBuildingPolygon& Polygon, const FWorldPoint& Point);

/** Distance from a point to the segment from A to B. */
double PointSegmentDistance(const FWorldPoint& Point, const FWorldPoint& A, const FWorldPoint& B);

/** Distance between the segments A-B and C-D, 0 when they cross or touch. */
double SegmentDistance(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D);

/**
 * True when the segments meet at a point that is not an end point of both, or overlap: the intersection test of
 * GEOS's topology preserving simplifier.
 */
bool HasInteriorIntersection(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D);

/** Distance between two polygons, 0 when they overlap; may return any value above Limit once it is known to exceed it. */
double PolygonDistance(const FBuildingPolygon& First, const FBuildingPolygon& Second, double Limit);

/** Distance from a polygon to a segment, 0 when they overlap; may return any value above Limit. */
double PolygonSegmentDistance(const FBuildingPolygon& Polygon, const FBox2& PolygonBox, const FWorldPoint& A,
	const FWorldPoint& B, double Limit);

/** True when no ring of the polygon crosses itself or touches itself other than at neighbouring points. */
bool IsSimplePolygon(const FBuildingPolygon& Polygon);

/** The polygon cut into valid polygons where its rings cross themselves (what make_valid does for the rare bowtie). */
std::vector<FBuildingPolygon> MakeValidPolygon(const FBuildingPolygon& Polygon);

/** The union of polygons, with holes (shapely.union_all). */
std::vector<FBuildingPolygon> UnionPolygons(const std::vector<FBuildingPolygon>& Polygons);

/** Douglas Peucker simplification that keeps the rings valid (shapely's simplify with preserve_topology). */
FBuildingPolygon SimplifyPolygon(const FBuildingPolygon& Polygon, double Tolerance);

/**
 * The minimum rotated rectangle as GEOS returns it: the smallest rectangle with a side along an edge of the convex
 * hull, corners clockwise from the start of that edge. Rectangles of equal area (a rectangular hull, a triangle) are
 * decided by rounding in GEOS and by the first edge here. Returns false when the polygon is a line or a point.
 */
bool MinimumRotatedRectangle(const FBuildingPolygon& Polygon, std::array<FWorldPoint, 4>& Corners);

/** The polygon grown by Distance with mitred corners (limit 4), as the largest piece; false if nothing is left. */
bool BufferMitre(const FBuildingPolygon& Polygon, double Distance, FBuildingPolygon& Result);

/**
 * The intersection of the infinite lines through P1-P2 and Q1-Q2, rounded like GEOS's Intersection::intersection;
 * false when the lines are parallel.
 */
bool LineIntersectionPoint(const FWorldPoint& P1, const FWorldPoint& P2, const FWorldPoint& Q1, const FWorldPoint& Q2,
	FWorldPoint& Result);

/**
 * The buffer of the polygon with round joins (16 segments per quarter circle), as the pieces of the result. A polygon
 * without holes whose offset curve does not cross itself gets the outline GEOS builds, vertex for vertex; any other
 * is grown by Clipper, whose arcs differ a little.
 */
std::vector<FBuildingPolygon> BufferRound(const FBuildingPolygon& Polygon, double Distance);

/** A line of connected segments with its length, for distances along it. */
struct FMeasuredLine
{
	std::vector<FWorldPoint> Points;
	double Length = 0.0;

	/** Fills Length from the points. */
	void ComputeLength();
	/** Distance along the line of the point on it that is closest to Point (the first closest on ties). */
	double Project(const FWorldPoint& Point) const;
	/** The point at Distance along the line, clamped to its ends. */
	FWorldPoint Interpolate(double Distance) const;
};

/** Grid over segments for distance queries from polygons. */
class FSegmentGrid
{
public:
	/** Indexes every segment of the polylines; the id of a segment is its polyline's index. */
	void Build(const std::vector<FMeasuredLine>& Polylines, double CellSize);

	/**
	 * Calls Visit(polyline index, segment start, segment end) for every segment that may lie within Distance of the
	 * box (a segment can be visited more than once per polyline, never twice for the same segment).
	 */
	void Query(const FBox2& Box, double Distance,
		const std::function<void(size_t, const FWorldPoint&, const FWorldPoint&)>& Visit) const;

private:
	struct FEntry
	{
		uint32_t Polyline;
		uint32_t Segment;
	};
	int64_t CellOfX(double X) const;
	int64_t CellOfY(double Y) const;
	/** Adds a segment to every cell its box touches. */
	void InsertSegment(uint32_t Polyline, uint32_t Segment, const FWorldPoint& Start, const FWorldPoint& End);
	/** Reports the segments of one cell that are home in it (see Query) and near enough. */
	void VisitCell(int64_t CellX, int64_t CellY, int64_t FirstX, int64_t FirstY, const FBox2& Box, double Distance,
		const std::function<void(size_t, const FWorldPoint&, const FWorldPoint&)>& Visit) const;
	const std::vector<FMeasuredLine>* Lines = nullptr;
	double Cell = 50.0;
	int64_t OriginX = 0;
	int64_t OriginY = 0;
	int64_t CountX = 0;
	int64_t CountY = 0;
	std::vector<std::vector<FEntry>> Cells;
};

/**
 * An STR tree with node capacity 10 built and traversed like GEOS's (used by shapely), so that a query returns the
 * items in the same order.
 */
class FStrTree
{
public:
	/** Builds the tree over the boxes; the item of a box is its index. */
	void Build(const std::vector<FBox2>& Boxes);

	/** Indices of the boxes that intersect the query box, in GEOS's order. */
	void Query(const FBox2& Box, std::vector<uint32_t>& Result) const;

private:
	struct FNode
	{
		FBox2 Box;
		uint32_t Item = 0;
		uint32_t FirstChild = 0;
		uint32_t EndChild = 0;
		bool bLeaf = true;
	};
	/** Adds a node over the children from GroupBegin up to GroupEnd. */
	void AddParentNode(size_t GroupBegin, size_t GroupEnd);
	/** Groups the nodes from Begin up to End into parents the way GEOS does: vertical slices, each sorted by y. */
	void CreateParentNodes(size_t Begin, size_t End);
	void QueryNode(const FNode& Node, const FBox2& Box, std::vector<uint32_t>& Result) const;

	std::vector<FNode> Nodes;
};
}
