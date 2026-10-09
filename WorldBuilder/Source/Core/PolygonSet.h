#pragma once

#include <vector>

#include "Geometry.h"
#include "SpatialIndex.h"

namespace WorldBuilder
{
/**
 * Many polygons that may overlap, answered as their union without ever merging them: a point is inside when any
 * piece holds it. Meant for region-wide point and line questions (is this pole on the road, where does this crossing
 * line run over the carriageway), where a merged polygon of a whole city would be too big to build.
 */
class FPolygonSet
{
public:
	explicit FPolygonSet(double BucketSize = 100.0) : Index(BucketSize) {}

	/** Adds one piece; its rings are filled even-odd. */
	void Add(FPolygons Piece);

	size_t Size() const { return Pieces.size(); }
	const std::vector<FPolygons>& All() const { return Pieces; }

	/** True when any piece contains the point. */
	bool Contains(double X, double Y) const;

	/** Distance to the union: 0 inside, else to the nearest piece's outline, at most Limit. */
	double Distance(double X, double Y, double Limit) const;

	/** The union of the pieces touching a window, clipped to it. */
	FPolygons InWindow(const FBox& Window) const;

	/** The area of a polygon (filled even-odd) covered by the union. */
	double CoveredArea(const FPolygons& Polygon) const;

	/** The parts of a polyline inside the union, as pieces of polyline. */
	std::vector<FPolyline> ClipLine(const FPolyline& Line) const;

private:
	std::vector<FPolygons> Pieces;
	FSpatialIndex Index;
};
}
