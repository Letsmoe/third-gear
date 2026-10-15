#pragma once

#include <vector>

/**
 * Designed road heights: one least squares fit over a whole network, so every road meeting at a junction has the same
 * height there.
 */
namespace WorldBuilder
{
/** One chain's samples in the fit. */
struct FProfileChain
{
	/** The height unknown of every sample; samples of different chains at one junction share theirs. */
	std::vector<int> Unknowns;
	/** The ground height under every sample. */
	std::vector<double> Ground;
	/** How far every sample's ground is to be trusted: 1 normally, near 0 where it says nothing about the road. */
	std::vector<double> GroundTrust;
	/** Distance from every sample to the next, metres (one fewer than samples). */
	std::vector<double> Spacing;
	/** Length over which the change of grade is smoothed, metres. */
	double Smoothing = 10.0;
	/**
	 * A factor on the bending stiffness at every sample, 1 normally; lower near a junction where the road yields, so
	 * its profile can bend from the main road's surface to its own grade as quickly as real junction approaches do.
	 * Empty means 1 everywhere.
	 */
	std::vector<double> Stiffness;
	double MaxGrade = 0.12;
};

/** A height held to a target: a side street's sample inside the main road, held to the main road's surface there. */
struct FProfileTie
{
	int Unknown = 0;
	double Target = 0.0;
};

struct FProfileResult
{
	/** The height of every unknown. */
	std::vector<double> Heights;
	/** How many sample steps ended up held at their chain's steepest grade. */
	int GradeLimited = 0;
};

/**
 * Fits the heights: close to the ground, with the change of grade small over each chain's smoothing length, no grade
 * steeper than its limit, and every tie held. Ground that sticks out from the fit by more than a few decimetres counts
 * less and less (a Huber fit), so a stray survey point or a parked lorry in the data bends no road.
 */
FProfileResult SolveProfiles(int UnknownCount, const std::vector<FProfileChain>& Chains,
							 const std::vector<FProfileTie>& Ties);
}
