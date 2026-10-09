#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "CrossSection.h"
#include "OsmData.h"
#include "Polyline.h"

/**
 * The road network as segments: ways cut at junctions, and what lies at each end of a segment (Python:
 * streets/network.py).
 *
 * A node where three or more segment ends meet is a junction. A node where exactly two meet is a continuation (one
 * road becomes the next, or a way's end meets the next way's start). A node with one end is a dead end.
 */
namespace WorldBuilder
{
// How far out along an arm the clearance search looks, and its step.
inline constexpr double MouthSearchLimit = 60.0;
inline constexpr double MouthSearchStep = 0.5;
// Points across an arm's carriageway tested against the other arms.
inline constexpr int MouthProbePoints = 9;

/** A road way of the street model: the OSM way with its points in the world frame. */
struct FStreetWay
{
	int64_t Id = 0;
	FTags Tags;
	std::vector<int64_t> NodeIds;
	FStreetPolyline Points;
};

/** The street model's way for an OSM road. */
FStreetWay MakeStreetWay(const FOsmWay& Road);

/** Cross-sections of ways by way id. */
using FSectionsByWay = std::unordered_map<int64_t, FCrossSection>;

enum class ENodeKind
{
	Junction,
	Continuation,
	DeadEnd,
};

/** A piece of a way between two nodes that are junctions or way ends. */
struct FSegment
{
	int Id = 0;
	const FStreetWay* Way = nullptr;
	/** Along the way's node order. */
	FStreetPolyline Xy;
	int64_t StartNode = 0;
	int64_t EndNode = 0;
	FCrossSection Section;
	/** The length of Xy. */
	double Length = 0.0;
};

/** One end of a segment: at its start (the way's first node of the piece) or at its end. */
struct FSegmentEnd
{
	int Segment = 0;
	bool bAtStart = false;

	bool operator==(const FSegmentEnd& Other) const
	{
		return Segment == Other.Segment && bAtStart == Other.bAtStart;
	}

	bool operator!=(const FSegmentEnd& Other) const
	{
		return !(*this == Other);
	}

	bool operator<(const FSegmentEnd& Other) const
	{
		if (Segment != Other.Segment)
		{
			return Segment < Other.Segment;
		}
		return bAtStart < Other.bAtStart;
	}
};

/**
 * Segments of the given ways and the ends meeting at every node. The segments point into the ways, which must outlive
 * the network.
 */
class FSegmentNetwork
{
public:
	FSegmentNetwork(const std::vector<FStreetWay>& Ways, const FSectionsByWay& Sections);

	std::vector<FSegment> Segments;
	std::unordered_map<int64_t, FStreetPoint> NodePoints;
	/** The segment ends at every node. */
	std::unordered_map<int64_t, std::vector<FSegmentEnd>> EndsAt;
	/** The nodes of EndsAt in the order they were first met, which keeps the output order stable. */
	std::vector<int64_t> NodeOrder;

	ENodeKind NodeKind(int64_t Node) const;

	int64_t NodeOf(const FSegmentEnd& End) const;

	/** The other segment end at a continuation node, or nothing. */
	std::optional<FSegmentEnd> OtherEnd(const FSegmentEnd& End) const;

	/** The segment's centre line leaving the node at this end (reversed when the end is the segment's end). */
	FStreetPolyline ArmLine(const FSegmentEnd& End) const;

	/** The unit direction in which the segment leaves the node, measured over its first metres. */
	FStreetPoint ArmDirection(const FSegmentEnd& End, double Reach = 6.0) const;

	/**
	 * How far from the node the arm's carriageway, across its whole width, is clear of the other arms' (and at least
	 * Gap away from them).
	 */
	double ClearDistance(const FSegmentEnd& End, const std::vector<FSegmentEnd>& Others, double Gap = 0.0) const;

private:
	/** Cuts a way at its inner junction nodes into segments. */
	void SplitWay(const FStreetWay& Way, const FCrossSection& Section,
				  const std::unordered_map<int64_t, int>& Degrees);
};

/** How many way ends and passes touch each node: a pass counts twice, so three or more means a junction. */
std::unordered_map<int64_t, int> NodeDegrees(const std::vector<FStreetWay>& Ways);
}
