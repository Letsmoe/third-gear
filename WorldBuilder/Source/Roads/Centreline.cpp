#include "Centreline.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/Sparse>

namespace WorldBuilder
{
namespace
{
/** Weight of a pinned sample against the others' 1. */
constexpr double PinnedWeight = 1e8;
/** Rounds of pulling stray points back; each one refits the whole line. */
constexpr int MaxRefits = 12;
/** A fit counts as within the limit when it overshoots by less than this share. */
constexpr double DeviationTolerance = 0.02;

using FSparse = Eigen::SparseMatrix<double>;

/** Stiffness * Dᵀ D for the second differences D of Count samples: the bending energy of the line. */
FSparse BendingMatrix(int Count, double Stiffness)
{
	std::vector<Eigen::Triplet<double>> Entries;
	const double Row[3] = {1.0, -2.0, 1.0};
	for (int Middle = 1; Middle + 1 < Count; ++Middle)
	{
		for (int First = 0; First < 3; ++First)
		{
			for (int Second = 0; Second < 3; ++Second)
			{
				Entries.emplace_back(Middle - 1 + First, Middle - 1 + Second, Stiffness * Row[First] * Row[Second]);
			}
		}
	}
	FSparse Matrix(Count, Count);
	Matrix.setFromTriplets(Entries.begin(), Entries.end());
	return Matrix;
}

/** The points that minimise Σ Weight |point - sample|² plus the bending energy. */
FStreetPolyline SolveFit(const FStreetPolyline& Samples, const std::vector<double>& Weights, const FSparse& Bending)
{
	const int Count = static_cast<int>(Samples.size());
	FSparse System = Bending;
	Eigen::VectorXd RightX(Count);
	Eigen::VectorXd RightY(Count);
	for (int Index = 0; Index < Count; ++Index)
	{
		System.coeffRef(Index, Index) += Weights[Index];
		RightX[Index] = Weights[Index] * Samples[Index].X;
		RightY[Index] = Weights[Index] * Samples[Index].Y;
	}
	Eigen::SimplicialLDLT<FSparse> Solver(System);
	const Eigen::VectorXd X = Solver.solve(RightX);
	const Eigen::VectorXd Y = Solver.solve(RightY);
	FStreetPolyline Points(Count);
	for (int Index = 0; Index < Count; ++Index)
	{
		Points[Index] = {X[Index], Y[Index]};
	}
	return Points;
}

/** Distance of every point from the OSM line. */
std::vector<double> Deviations(const FStreetPolyline& Points, const FStreetPolyline& Original)
{
	std::vector<double> Distances(Points.size());
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		Distances[Index] = Polyline::DistanceTo(Original, Points[Index]);
	}
	return Distances;
}

/** Raises the weight of every point beyond the limit, more the further out it is; false when none is. */
bool PullBackStrays(const std::vector<double>& Distances, double MaxDeviation, std::vector<double>& Weights)
{
	bool bAnyStray = false;
	for (size_t Index = 0; Index < Distances.size(); ++Index)
	{
		const double Excess = Distances[Index] / MaxDeviation;
		if (Excess <= 1.0 + DeviationTolerance)
		{
			continue;
		}
		Weights[Index] *= std::pow(Excess, 4.0);
		bAnyStray = true;
	}
	return bAnyStray;
}
}

FCentrelineFit FitCentreline(const FStreetPolyline& Samples, const FStreetPolyline& Original, const std::vector<int>& Pinned,
							 double Smoothing, double MaxDeviation)
{
	FCentrelineFit Fit;
	if (Samples.size() < 3)
	{
		Fit.Points = Samples;
		return Fit;
	}
	const int Count = static_cast<int>(Samples.size());
	// With samples a metre apart, the second difference is the curvature; its weight sets the smoothing length.
	const FSparse Bending = BendingMatrix(Count, std::pow(Smoothing, 4.0));
	std::vector<double> Weights(Count, 1.0);
	for (const int Index : Pinned)
	{
		Weights[Index] = PinnedWeight;
	}
	std::vector<double> Distances;
	for (int Round = 0; Round < MaxRefits; ++Round)
	{
		Fit.Points = SolveFit(Samples, Weights, Bending);
		Distances = Deviations(Fit.Points, Original);
		if (!PullBackStrays(Distances, MaxDeviation, Weights))
		{
			break;
		}
	}
	Fit.MaxDeviation = *std::max_element(Distances.begin(), Distances.end());
	return Fit;
}

std::vector<double> CurveRadii(const FStreetPolyline& Points, int Reach)
{
	const int Count = static_cast<int>(Points.size());
	std::vector<double> Radii(Count, std::numeric_limits<double>::infinity());
	for (int Index = Reach; Index + Reach < Count; ++Index)
	{
		const FStreetPoint& Before = Points[Index - Reach];
		const FStreetPoint& Here = Points[Index];
		const FStreetPoint& After = Points[Index + Reach];
		const FStreetPoint First = Here - Before;
		const FStreetPoint Second = After - Here;
		const double TwiceArea = std::abs(First.X * Second.Y - First.Y * Second.X);
		if (TwiceArea < 1e-9)
		{
			continue;
		}
		// Circumradius: product of the side lengths over four times the triangle's area.
		Radii[Index] = Norm(First) * Norm(Second) * Norm(After - Before) / (2.0 * TwiceArea);
	}
	return Radii;
}
}
