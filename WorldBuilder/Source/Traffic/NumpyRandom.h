#pragma once

#include <cstdint>
#include <vector>

/**
 * numpy's default random generator (PCG64 seeded through SeedSequence), reproduced so that the parked cars come out
 * as the Python builder's: the same numbers for the same way ids.
 */
namespace WorldBuilder
{
class FNumpyRandom
{
public:
	/** The generator numpy.random.default_rng(Seed) makes, for a seed below 2^32. */
	explicit FNumpyRandom(uint32_t Seed);

	/** The next 64 random bits. */
	uint64_t NextUint64();

	/** A uniform number in [0, 1) (Generator.random()). */
	double Random();

	/** A uniform number in [Low, High) (Generator.uniform(Low, High)). */
	double Uniform(double Low, double High);

	/** An index drawn with the given probabilities, which sum to 1 (Generator.choice(count, p=...)). */
	int Choice(const std::vector<double>& Probabilities);

private:
	unsigned __int128 State = 0;
	unsigned __int128 Increment = 0;

	/** Moves the state one step on. */
	void Step();
};
}
