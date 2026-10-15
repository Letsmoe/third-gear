#include "VerticalProfile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include <Eigen/Sparse>

namespace WorldBuilder
{
namespace
{
/** Ground further than this from the fit counts less, in proportion (the Huber threshold), metres. */
constexpr double OutlierDistance = 0.3;
/** Weight of holding a grade at its limit, against 1 for a metre of road one metre off the ground. */
constexpr double GradeWeight = 1e6;
/** Refits for the outlier weights and the grade limits to settle. */
constexpr int Rounds = 8;

using FSparse = Eigen::SparseMatrix<double>;
using FTriplets = std::vector<Eigen::Triplet<double>>;

/** A sample step held at the steepest grade: chain, first sample, and the grade it is held at. */
using FGradeKey = std::pair<size_t, size_t>;
using FGradeLimits = std::map<FGradeKey, double>;

/** Adds Weight * (Σ Coefficients[k] z[Unknowns[k]] - Target)² to the system. */
void AddSquare(FTriplets& Entries, Eigen::VectorXd& Right, const int* Unknowns, const double* Coefficients, int Count,
			   double Target, double Weight)
{
	for (int First = 0; First < Count; ++First)
	{
		Right[Unknowns[First]] += Weight * Coefficients[First] * Target;
		for (int Second = 0; Second < Count; ++Second)
		{
			Entries.emplace_back(Unknowns[First], Unknowns[Second], Weight * Coefficients[First] * Coefficients[Second]);
		}
	}
}

/** The ground term: every sample near its ground height, weighted by the road length it stands for. */
void AddGround(const FProfileChain& Chain, const std::vector<double>& OutlierWeights, FTriplets& Entries,
			   Eigen::VectorXd& Right)
{
	const size_t Count = Chain.Unknowns.size();
	for (size_t Index = 0; Index < Count; ++Index)
	{
		const double Before = Index > 0 ? Chain.Spacing[Index - 1] : 0.0;
		const double After = Index + 1 < Count ? Chain.Spacing[Index] : 0.0;
		const double Length = std::max(0.5 * (Before + After), 0.1);
		const double Coefficient = 1.0;
		AddSquare(Entries, Right, &Chain.Unknowns[Index], &Coefficient, 1, Chain.Ground[Index],
				  Length * OutlierWeights[Index] * Chain.GroundTrust[Index]);
	}
}

/**
 * The bending term: the change of grade at every inner sample, from the grades of the steps either side, squared and
 * weighted by Smoothing⁴ and the length it stands for.
 */
void AddBending(const FProfileChain& Chain, FTriplets& Entries, Eigen::VectorXd& Right)
{
	const double Stiffness = std::pow(Chain.Smoothing, 4.0);
	for (size_t Index = 1; Index + 1 < Chain.Unknowns.size(); ++Index)
	{
		const double Before = std::max(Chain.Spacing[Index - 1], 0.05);
		const double After = std::max(Chain.Spacing[Index], 0.05);
		const double Span = 0.5 * (Before + After);
		const int Unknowns[3] = {Chain.Unknowns[Index - 1], Chain.Unknowns[Index], Chain.Unknowns[Index + 1]};
		const double Coefficients[3] = {1.0 / (Before * Span), -(1.0 / Before + 1.0 / After) / Span, 1.0 / (After * Span)};
		AddSquare(Entries, Right, Unknowns, Coefficients, 3, 0.0, Stiffness * Span);
	}
}

/** Holds every step in Limits at its grade. */
void AddGradeLimits(const std::vector<FProfileChain>& Chains, const FGradeLimits& Limits, FTriplets& Entries,
					Eigen::VectorXd& Right)
{
	for (const auto& [Key, Grade] : Limits)
	{
		const FProfileChain& Chain = Chains[Key.first];
		const size_t Index = Key.second;
		const double Step = std::max(Chain.Spacing[Index], 0.05);
		const int Unknowns[2] = {Chain.Unknowns[Index], Chain.Unknowns[Index + 1]};
		const double Coefficients[2] = {-1.0 / Step, 1.0 / Step};
		AddSquare(Entries, Right, Unknowns, Coefficients, 2, Grade, GradeWeight * Step);
	}
}

/** Huber weights from how far each sample's ground lies from the fit. */
void UpdateOutlierWeights(const FProfileChain& Chain, const Eigen::VectorXd& Heights, std::vector<double>& Weights)
{
	for (size_t Index = 0; Index < Chain.Unknowns.size(); ++Index)
	{
		const double Residual = std::abs(Heights[Chain.Unknowns[Index]] - Chain.Ground[Index]);
		Weights[Index] = Residual > OutlierDistance ? OutlierDistance / Residual : 1.0;
	}
}

/** Adds every step steeper than its chain's limit to Limits, held at the limit from now on. */
void FindSteepSteps(const std::vector<FProfileChain>& Chains, const Eigen::VectorXd& Heights, FGradeLimits& Limits)
{
	for (size_t ChainIndex = 0; ChainIndex < Chains.size(); ++ChainIndex)
	{
		const FProfileChain& Chain = Chains[ChainIndex];
		for (size_t Index = 0; Index + 1 < Chain.Unknowns.size(); ++Index)
		{
			const double Step = std::max(Chain.Spacing[Index], 0.05);
			const double Grade = (Heights[Chain.Unknowns[Index + 1]] - Heights[Chain.Unknowns[Index]]) / Step;
			if (std::abs(Grade) > Chain.MaxGrade * 1.001)
			{
				Limits[{ChainIndex, Index}] = std::copysign(Chain.MaxGrade, Grade);
			}
		}
	}
}
}

FProfileResult SolveProfiles(int UnknownCount, const std::vector<FProfileChain>& Chains)
{
	FProfileResult Result;
	if (UnknownCount == 0)
	{
		return Result;
	}
	std::vector<std::vector<double>> OutlierWeights(Chains.size());
	for (size_t Index = 0; Index < Chains.size(); ++Index)
	{
		OutlierWeights[Index].assign(Chains[Index].Unknowns.size(), 1.0);
	}
	FGradeLimits Limits;
	Eigen::VectorXd Heights;
	for (int Round = 0; Round < Rounds; ++Round)
	{
		FTriplets Entries;
		Eigen::VectorXd Right = Eigen::VectorXd::Zero(UnknownCount);
		for (size_t Index = 0; Index < Chains.size(); ++Index)
		{
			AddGround(Chains[Index], OutlierWeights[Index], Entries, Right);
			AddBending(Chains[Index], Entries, Right);
		}
		AddGradeLimits(Chains, Limits, Entries, Right);
		FSparse System(UnknownCount, UnknownCount);
		System.setFromTriplets(Entries.begin(), Entries.end());
		Eigen::SimplicialLDLT<FSparse> Solver(System);
		Heights = Solver.solve(Right);
		for (size_t Index = 0; Index < Chains.size(); ++Index)
		{
			UpdateOutlierWeights(Chains[Index], Heights, OutlierWeights[Index]);
		}
		FindSteepSteps(Chains, Heights, Limits);
	}
	Result.Heights.assign(Heights.data(), Heights.data() + Heights.size());
	Result.GradeLimited = static_cast<int>(Limits.size());
	return Result;
}
}
