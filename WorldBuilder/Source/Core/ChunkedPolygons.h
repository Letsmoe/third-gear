#pragma once

#include <unordered_map>
#include <vector>

#include "Geometry.h"

namespace WorldBuilder
{
/**
 * Polygons cut into square chunks aligned to a fixed grid, each piece remembering which polygon (owner) it came from.
 * A large polygon like the Elbe has tens of thousands of vertices; cut once, a tile only handles the few pieces it
 * touches instead of clipping the whole outline again.
 */
class FChunkedPolygons
{
public:
	explicit FChunkedPolygons(double InChunkSize = 250.0) : ChunkSize(InChunkSize) {}

	/** Cuts a polygon (filled even-odd or non-zero, consistently oriented) into chunks under an owner number. */
	void Add(int Owner, const FPolygons& Polygon);

	/** The pieces in a window, per owner, in owner order; pieces of one owner are disjoint and together exact. */
	std::vector<std::pair<int, FPolygons>> InWindow(const FBox& Window) const;

	/** All pieces in a window, merged into one set. */
	FPolygons UnionInWindow(const FBox& Window) const;

private:
	struct FPiece
	{
		int Owner;
		FPolygons Polygon;
	};
	double ChunkSize;
	std::unordered_map<int64_t, std::vector<FPiece>> Chunks;

	int64_t Key(int64_t Column, int64_t Row) const { return Row * 2000003 + Column; }
	/** Splits the polygon in halves along the box's longer side until a half is one chunk, then stores it. */
	void Split(int Owner, const FPolygons& Polygon, int64_t FirstColumn, int64_t FirstRow, int64_t Columns, int64_t Rows);
};
}
