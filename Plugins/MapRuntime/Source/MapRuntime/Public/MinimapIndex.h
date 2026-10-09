#pragma once

#include "CoreMinimal.h"

class FLaneNetwork;

/** Stroke class of the planned route, drawn on top of the roads (road classes are 0 to 4). */
constexpr int32 MinimapRouteTier = 9;

/** One polyline of a map frame: a range of FMinimapFrame::Points and the road class that sets its width. */
struct FMinimapStroke
{
	int32 FirstPoint = 0;
	int32 PointCount = 0;
	/** Road class 0 (service) to 4 (primary and trunk) as in the lane graph, or MinimapRouteTier. */
	int32 Tier = 1;
};

/** A one-way arrow on a lane: position and direction of travel in the view's frame. */
struct FMinimapArrow
{
	FVector2f Position = FVector2f::ZeroVector;
	FVector2f Direction = FVector2f(0.f, 1.f);
};

/**
 * The roads around a map centre for one redraw, already in the view's frame (metres, +X right, +Y up on the screen) so
 * that the painters only scale and clip them. The arrays keep their capacity between redraws.
 */
struct FMinimapFrame
{
	TArray<FVector2f> Points;
	TArray<FMinimapStroke> Strokes;
	/** Arrows along one-way roads; the painter draws them when zoomed in. */
	TArray<FMinimapArrow> Arrows;

	void Reset()
	{
		Points.Reset();
		Strokes.Reset();
		Arrows.Reset();
	}
};

/** The place on a road lane that is closest to a query position. */
struct FMinimapLaneHit
{
	int32 LaneId = INDEX_NONE;
	float DistanceMeters = 0.f;
	FVector2f Point = FVector2f::ZeroVector;
};

namespace MinimapGeometry
{
/** Douglas-Peucker line simplification without recursion; the end points are always kept. */
MAPRUNTIME_API void SimplifyLine(const TArray<FVector2f>& Line, TArray<FVector2f>& OutLine, float ToleranceMeters);

/** Roads below this class are left out at a view radius; wide views show main roads only. */
MAPRUNTIME_API int32 MinTierForRadius(float RadiusMeters);

/** Moves a world offset into a view frame whose up direction is (cos, sin) of the heading. */
MAPRUNTIME_API FVector2f ToViewFrame(const FVector2f& WorldOffset, float HeadingRadians);
}

/**
 * The road centre lines of the lane graph, simplified once and sorted into a grid, so that a redraw only touches the
 * lanes near the view. Built on a worker thread from the road lanes of the AI traffic graph (junction connections are
 * left out: they would draw doubled lines through every crossing).
 */
class MAPRUNTIME_API FMinimapIndex
{
public:
	/** Simplifies every road lane and fills the grid. */
	void Build(const FLaneNetwork& Network);

	/**
	 * Fills OutFrame with the lanes of class MinTier and above that come within RadiusM of CentreM (world metres, x east,
	 * y south), rotated so that the direction (cos, sin) of HeadingRadians points up. Game thread only: it uses a visit stamp.
	 */
	void Query(const FVector2f& CentreM, float RadiusM, float HeadingRadians, int32 MinTier, FMinimapFrame& OutFrame) const;

	/**
	 * The road lane of the main loop of the graph closest to a position, within MaxDistanceM. With a travel direction
	 * (unit vector), lanes running against it count 30 m farther, so a car picks the lane it is driving on.
	 */
	bool FindNearestLane(const FVector2f& PositionM, float MaxDistanceM, const FVector2f* TravelDirection, FMinimapLaneHit& OutHit) const;

	bool IsEmpty() const { return Lanes.Num() == 0; }

private:
	struct FLane
	{
		int32 FirstPoint = 0;
		int32 PointCount = 0;
		int32 Tier = 1;
		int32 NetworkLaneId = INDEX_NONE;
		bool bGood = false;
		/** Range in Arrows of the one-way arrows along this lane. */
		int32 FirstArrow = 0;
		int32 ArrowCount = 0;
		FVector2f BoundsMin = FVector2f::ZeroVector;
		FVector2f BoundsMax = FVector2f::ZeroVector;
	};

	/** Adds one simplified lane and enters it into the grid cells its bounds touch. */
	void AddLane(const TArray<FVector2f>& SimplifiedPoints, const FLane& Template);

	/** Closest point of one lane's polyline to a position, with the lane's travel direction at that point. */
	float DistanceToLane(const FLane& Lane, const FVector2f& PositionM, FVector2f& OutPoint, FVector2f& OutDirection) const;

	/** Adds arrows every ArrowSpacingM along a lane's line, if the lane is long enough. */
	void AddArrows(const TArray<FVector2f>& Line, FLane& InOutLane);

	TArray<FVector2f> Points;
	/** One-way arrows as world position and direction. */
	TArray<FMinimapArrow> Arrows;
	TArray<FLane> Lanes;
	TMap<FIntPoint, TArray<int32>> Grid;
	mutable TArray<uint32> VisitStamp;
	mutable uint32 CurrentStamp = 0;
};
