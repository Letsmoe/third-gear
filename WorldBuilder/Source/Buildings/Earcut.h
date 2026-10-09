#pragma once

#include <cstdint>
#include <vector>

#include "Projection.h"

/**
 * Ear clipping triangulation of a simple polygon, after mapbox earcut (the algorithm behind the Python mapbox_earcut
 * that roofs.py calls). Only one ring without holes, and without the z-order hash that speeds up polygons of
 * hundreds of vertices; the triangles come out the same.
 */
namespace WorldBuilder
{
/** Triangle corner indices (three per triangle) into the points of the ring. */
std::vector<uint32_t> TriangulateRing(const std::vector<FWorldPoint>& Ring);
}
