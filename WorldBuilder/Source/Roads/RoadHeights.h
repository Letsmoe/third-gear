#pragma once

#include <optional>
#include <vector>

#include "RoadModel.h"
#include "SpatialIndex.h"

/** The road model's height at any point on or near a road, for the tiles' road height grid. */
namespace WorldBuilder
{
class FRoadHeights
{
public:
	/** Indexes the model, which must outlive this. */
	explicit FRoadHeights(const FRoadModel& Model);

	/**
	 * The road surface height at a point, cross fall included: from the nearest centreline of every chain whose road
	 * reaches it, blended by how far the point lies outside each road's half width. Nothing when no road comes within
	 * its half width and Reach.
	 */
	std::optional<double> At(double X, double Y) const;

	/** How far beyond a road's kerb its height still counts, metres: enough for the kerb radii at junction corners. */
	static constexpr double Reach = 6.0;

private:
	/** A run of consecutive samples of one chain, the unit the spatial index holds. */
	struct FRun
	{
		int Chain = 0;
		int First = 0;
		int Last = 0;
	};

	/**
	 * The nearest point of a run's centreline: its distance, the road's half width there, and the surface height with
	 * the cross fall at the point, or at the kerb when the point lies beyond it.
	 */
	struct FNearest
	{
		double Distance = 1e300;
		double Height = 0.0;
		double HalfWidth = 0.0;
	};

	FNearest NearestOnRun(const FRun& Run, double X, double Y) const;

	const FRoadModel* Model = nullptr;
	std::vector<FRun> Runs;
	FSpatialIndex Index{32.0};
};
}
