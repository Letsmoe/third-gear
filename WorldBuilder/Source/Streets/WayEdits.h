#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Network.h"

/**
 * Redrawing ways: their nodes moved to new places and shape points added between them, in every way that uses them.
 * The nodes keep their ids, so whatever refers to a node (a signal, a crossing) stays on its road.
 */
namespace WorldBuilder
{
/** A shape point added to a way: a made-up node id and where it lies. */
struct FAddedPoint
{
	int64_t NodeId = 0;
	FStreetPoint Point;
};

struct FWayEdits
{
	std::unordered_map<int64_t, FStreetPoint> MovedNodes;
	/** The points added between two neighbouring nodes, in the order from the first node to the second. */
	std::map<std::pair<int64_t, int64_t>, std::vector<FAddedPoint>> AddedPoints;
	/** The id the next added point gets; each kind of redrawing counts down from its own range. */
	int64_t NextSyntheticId = 0;

	/** Points along Line (from node First to node Second) about Spacing apart, added between the two nodes both ways. */
	void AddPoints(int64_t First, int64_t Second, const FStreetPolyline& Line, double StartS, double EndS, double Spacing);
};

/** The way with its nodes moved and shape points added, or the way itself when nothing changed. */
FStreetWay WithEdits(const FStreetWay& Way, const FWayEdits& Edits);

/** Every way with the edits applied. */
std::vector<FStreetWay> WithEdits(const std::vector<FStreetWay>& Ways, const FWayEdits& Edits);
}
