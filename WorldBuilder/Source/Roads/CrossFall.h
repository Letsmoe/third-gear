#pragma once

#include <vector>

#include "Polyline.h"

/**
 * The cross fall of a road: how its surface falls from the crown to the kerbs, and how it banks in curves outside
 * town (German RAL and RASt guidance).
 */
namespace WorldBuilder
{
/** The usual fall from the crown to the kerbs, rise over run. */
inline constexpr double CrownFall = 0.025;

/** The fall of the left and right half of a road, outward from the centreline: positive falls, negative rises. */
struct FCrossFall
{
	double Left = CrownFall;
	double Right = CrownFall;
};

/** What a sample's road is, as far as its cross fall goes. */
struct FCrossFallTraits
{
	/**
	 * The fall the road has on the straight, signed: 0 for a crowned two-way road, +CrownFall for a one-way road
	 * whose traffic runs along the samples (it falls to the right of travel), -CrownFall against them.
	 */
	double StraightBank = 0.0;
	/** True outside town, where curves are banked. */
	bool bBanksInCurves = false;
};

/**
 * The cross fall at every sample of a centreline (about a metre apart): its straight fall, banked towards the inside
 * of curves tighter than 1000 m outside town, with the change spread smoothly along the road.
 */
std::vector<FCrossFall> CrossFalls(const FStreetPolyline& Points, const std::vector<FCrossFallTraits>& Traits);

/** The height of the surface Offset metres right of the centreline (negative is left), below a centre at 0. */
double CrossFallDrop(const FCrossFall& Fall, double Offset);
}
