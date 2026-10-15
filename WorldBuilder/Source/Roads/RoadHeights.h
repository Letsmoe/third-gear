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
	 * The road surface height at a point, cross fall included. Inside a carriageway it is that road's surface, or
	 * where carriageways overlap at a junction, the surface of the road the others yield to. Outside every carriageway
	 * the kerb heights of the roads around are blended by distance. Nothing when no road comes within its half width
	 * and Reach.
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

	/** The foot of a point on a chain's centreline, with the road's shape there. */
	struct FNearest
	{
		int Chain = -1;
		double Distance = 1e300;
		/** Distance of the foot along the centreline. */
		double Along = 0.0;
		/** The point's offset from the centreline, positive to the right. */
		double Offset = 0.0;
		double Centre = 0.0;
		double HalfWidth = 0.0;
		FCrossFall Fall;
	};

	FNearest NearestOnRun(const FRun& Run, double X, double Y) const;

	/** The nearest foot on every chain whose runs are indexed around the point. */
	std::vector<FNearest> NearestByChain(double X, double Y) const;

	/** The nearest foot on one chain, or one with Chain -1 when the chain doesn't come near. */
	FNearest NearestOnChain(int Chain, double X, double Y) const;

	/**
	 * A chain's surface at the point, held at the kerb height beyond its kerb, and near a junction where it yields
	 * blended from the major road's kerb height to its own over AdaptLength.
	 */
	double SurfaceAt(const FNearest& Nearest, double X, double Y) const;

	/** True when the first chain yields to the second at one of its junctions. */
	bool YieldsTo(int Minor, int Major) const;

	const FRoadModel* Model = nullptr;
	std::vector<FRun> Runs;
	FSpatialIndex Index{32.0};
};
}
