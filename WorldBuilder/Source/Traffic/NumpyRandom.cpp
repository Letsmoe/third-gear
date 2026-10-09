#include "NumpyRandom.h"

#include <algorithm>
#include <array>

namespace WorldBuilder
{
namespace
{
// numpy's SeedSequence constants (numpy/random/bit_generator.pyx).
constexpr uint32_t InitA = 0x43b0d7e5;
constexpr uint32_t MultA = 0x931e8875;
constexpr uint32_t InitB = 0x8b51f9dd;
constexpr uint32_t MultB = 0x58f38ded;
constexpr uint32_t MixMultLeft = 0xca01f9dd;
constexpr uint32_t MixMultRight = 0x4973f715;
constexpr int ShiftBits = 16;
constexpr int PoolSize = 4;

/** SeedSequence's hashmix: mixes a value with a constant that changes on every call. */
uint32_t HashMix(uint32_t Value, uint32_t& HashConstant)
{
	Value ^= HashConstant;
	HashConstant *= MultA;
	Value *= HashConstant;
	Value ^= Value >> ShiftBits;
	return Value;
}

/** SeedSequence's mix of two words. */
uint32_t Mix(uint32_t First, uint32_t Second)
{
	uint32_t Result = MixMultLeft * First - MixMultRight * Second;
	Result ^= Result >> ShiftBits;
	return Result;
}

/** The pool SeedSequence builds from a single 32-bit entropy word. */
std::array<uint32_t, PoolSize> MakePool(uint32_t Entropy)
{
	std::array<uint32_t, PoolSize> Pool{};
	uint32_t HashConstant = InitA;
	for (int Index = 0; Index < PoolSize; ++Index)
	{
		Pool[Index] = HashMix(Index == 0 ? Entropy : 0, HashConstant);
	}
	for (int Source = 0; Source < PoolSize; ++Source)
	{
		for (int Destination = 0; Destination < PoolSize; ++Destination)
		{
			if (Source != Destination)
			{
				Pool[Destination] = Mix(Pool[Destination], HashMix(Pool[Source], HashConstant));
			}
		}
	}
	return Pool;
}

constexpr unsigned __int128 Multiplier =
	(static_cast<unsigned __int128>(0x2360ED051FC65DA4ULL) << 64) | 0x4385DF649FCCF645ULL;
}

FNumpyRandom::FNumpyRandom(uint32_t Seed)
{
	// SeedSequence.generate_state(4, uint64): eight 32-bit words, read as four little-endian 64-bit words.
	const std::array<uint32_t, PoolSize> Pool = MakePool(Seed);
	uint32_t HashConstant = InitB;
	std::array<uint32_t, 8> Words{};
	for (int Index = 0; Index < 8; ++Index)
	{
		uint32_t Value = Pool[Index % PoolSize];
		Value ^= HashConstant;
		HashConstant *= MultB;
		Value *= HashConstant;
		Value ^= Value >> ShiftBits;
		Words[Index] = Value;
	}
	std::array<uint64_t, 4> Seeds{};
	for (int Index = 0; Index < 4; ++Index)
	{
		Seeds[Index] = static_cast<uint64_t>(Words[2 * Index]) | (static_cast<uint64_t>(Words[2 * Index + 1]) << 32);
	}
	const unsigned __int128 InitialState = (static_cast<unsigned __int128>(Seeds[0]) << 64) | Seeds[1];
	const unsigned __int128 Sequence = (static_cast<unsigned __int128>(Seeds[2]) << 64) | Seeds[3];
	// pcg64_srandom_r
	State = 0;
	Increment = (Sequence << 1) | 1;
	Step();
	State += InitialState;
	Step();
}

void FNumpyRandom::Step()
{
	State = State * Multiplier + Increment;
}

uint64_t FNumpyRandom::NextUint64()
{
	Step();
	const uint64_t Folded = static_cast<uint64_t>(State >> 64) ^ static_cast<uint64_t>(State);
	const unsigned Rotation = static_cast<unsigned>(State >> 122);
	if (Rotation == 0)
	{
		return Folded;
	}
	return (Folded >> Rotation) | (Folded << (64 - Rotation));
}

double FNumpyRandom::Random()
{
	return static_cast<double>(NextUint64() >> 11) * (1.0 / 9007199254740992.0);
}

double FNumpyRandom::Uniform(double Low, double High)
{
	return Low + (High - Low) * Random();
}

int FNumpyRandom::Choice(const std::vector<double>& Probabilities)
{
	std::vector<double> CumulativeSums(Probabilities.size());
	double Total = 0.0;
	for (size_t Index = 0; Index < Probabilities.size(); ++Index)
	{
		Total += Probabilities[Index];
		CumulativeSums[Index] = Total;
	}
	for (double& Sum : CumulativeSums)
	{
		Sum /= CumulativeSums.back();
	}
	const double Sample = Random();
	// searchsorted(side="right"): the first sum greater than the sample.
	return static_cast<int>(std::upper_bound(CumulativeSums.begin(), CumulativeSums.end(), Sample) - CumulativeSums.begin());
}
}
