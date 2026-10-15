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
	double MaxGrade = 0.12;
};

struct FProfileResult
{
	/** The height of every unknown. */
	std::vector<double> Heights;
	/** How many sample steps ended up held at their chain's steepest grade. */
	int GradeLimited = 0;
};

/**
 * Fits the heights: close to the ground, with the change of grade small over each chain's smoothing length and no
 * grade steeper than its limit. Ground that sticks out from the fit by more than a few decimetres counts less and less
 * (a Huber fit), so a stray survey point or a parked lorry in the data bends no road.
 */
FProfileResult SolveProfiles(int UnknownCount, const std::vector<FProfileChain>& Chains);
}
