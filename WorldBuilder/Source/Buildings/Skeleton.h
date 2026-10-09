#pragma once

#include <vector>

#include "BuildingGeometry.h"

/**
 * Straight skeleton of a polygon with holes, as the roof planes of a hipped roof (skeleton.py).
 *
 * Every edge of the outline moves inward at unit speed; the vertices ride the bisectors. The wavefront changes at two
 * kinds of events: an edge shrinks to nothing (edge event, which makes ridges and hips) or a reflex vertex runs into
 * a wavefront edge (split event, which makes valleys and separates two blocks that meet). The time of a point on the
 * skeleton is its distance from the outline, so a roof of pitch p has height tan(p) times that time.
 *
 * The implementation recomputes every event each step, which is O(n^3) in the vertex count and fine for building
 * outlines (a few dozen vertices); callers give up on outlines with more than MaxSkeletonVertices.
 */
namespace WorldBuilder
{
constexpr size_t MaxSkeletonVertices = 70;

/** A point of the skeleton: its place and the time (distance from the outline) it is reached. */
struct FSkeletonNode
{
	double X = 0.0;
	double Y = 0.0;
	double Time = 0.0;
};

/** One face fragment: the outline edge it belongs to and its nodes in order. */
struct FSkeletonFace
{
	int Edge = 0;
	std::vector<int> Nodes;
};

/** An arc of the skeleton between two nodes; bIsValley when it comes from a reflex vertex. */
struct FSkeletonArc
{
	int NodeA = 0;
	int NodeB = 0;
	bool bIsValley = false;
};

/** The start point and unit direction of one outline edge. */
struct FSkeletonEdge
{
	FWorldPoint Origin;
	FWorldPoint Direction;
};

struct FSkeleton
{
	std::vector<FSkeletonNode> Nodes;
	std::vector<FSkeletonFace> Faces;
	std::vector<FSkeletonArc> Arcs;
	bool bOk = true;
	/** Per original edge. */
	std::vector<FSkeletonEdge> Edges;
};

/** Skeleton of the polygon given as rings: the outline counter-clockwise (positive shoelace area), then holes clockwise. */
FSkeleton StraightSkeleton(const std::vector<FRing>& Rings);
}
