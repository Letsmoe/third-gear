#pragma once

#include <vector>

#include "Polyline.h"

/** Smooth road centrelines fitted to the corners of the OSM line. */
namespace WorldBuilder
{
struct FCentrelineFit
{
	/** The smoothed points, one per sample. */
	FStreetPolyline Points;
	/** Largest distance from the OSM line, metres. */
	double MaxDeviation = 0.0;
};

/**
 * Smooths a road's samples (about one metre apart) into a curve without corners: a least squares fit that keeps the
 * points near their samples and the change of direction small over Smoothing metres. Points that end up further than
 * MaxDeviation from Original (the OSM line) are pulled back until none is. Samples listed in Pinned stay where they
 * are, which is how chains meeting at a junction keep meeting there.
 */
FCentrelineFit FitCentreline(const FStreetPolyline& Samples, const FStreetPolyline& Original, const std::vector<int>& Pinned,
							 double Smoothing, double MaxDeviation);

/** Radius of the circle through the points Reach samples either side of every sample, infinite where straight. */
std::vector<double> CurveRadii(const FStreetPolyline& Points, int Reach);
}
