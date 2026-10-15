#include "CrossFall.h"

#include <algorithm>
#include <cmath>

namespace WorldBuilder
{
namespace
{
/** Curves wider than this keep the straight fall, metres. */
constexpr double BankingStartRadius = 1000.0;
/** Curves this tight or tighter bank at the most, metres. */
constexpr double FullBankingRadius = 200.0;
constexpr double MaxBank = 0.07;
/** The curvature is measured over this many samples either side. */
constexpr int CurvatureReach = 5;
/** Samples either side over which the bank changes (the run-out), about a metre each. */
constexpr int RunOut = 20;

/** The signed curvature at every sample, positive in right-hand curves. */
std::vector<double> SignedCurvatures(const FStreetPolyline& Points)
{
	const int Count = static_cast<int>(Points.size());
	std::vector<double> Curvatures(Count, 0.0);
	for (int Index = CurvatureReach; Index + CurvatureReach < Count; ++Index)
	{
		const FStreetPoint Before = Points[Index] - Points[Index - CurvatureReach];
		const FStreetPoint After = Points[Index + CurvatureReach] - Points[Index];
		const double Chord = Norm(Points[Index + CurvatureReach] - Points[Index - CurvatureReach]);
		if (Chord < 1e-6 || Norm(Before) < 1e-6 || Norm(After) < 1e-6)
		{
			continue;
		}
		// Twice the turn's sine over the chord is the curvature of the circle through the three points.
		const double Sine = Dot(Polyline::RightOf(Polyline::Unit(Before)), Polyline::Unit(After));
		Curvatures[Index] = 2.0 * Sine / Chord;
	}
	return Curvatures;
}

/** The bank a curve of this radius gets: from CrownFall at BankingStartRadius to MaxBank at FullBankingRadius. */
double BankForRadius(double Radius)
{
	if (Radius >= BankingStartRadius)
	{
		return 0.0;
	}
	const double Tightness = (1.0 / Radius - 1.0 / BankingStartRadius) / (1.0 / FullBankingRadius - 1.0 / BankingStartRadius);
	return CrownFall + (MaxBank - CrownFall) * std::clamp(Tightness, 0.0, 1.0);
}

/** The signed bank every sample aims at: its straight fall, or in a banked curve the bank towards the inside. */
std::vector<double> TargetBanks(const FStreetPolyline& Points, const std::vector<FCrossFallTraits>& Traits)
{
	const std::vector<double> Curvatures = SignedCurvatures(Points);
	std::vector<double> Banks(Points.size());
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		Banks[Index] = Traits[Index].StraightBank;
		if (!Traits[Index].bBanksInCurves || std::abs(Curvatures[Index]) < 1e-9)
		{
			continue;
		}
		const double Bank = BankForRadius(1.0 / std::abs(Curvatures[Index]));
		if (Bank > 0.0)
		{
			Banks[Index] = std::copysign(Bank, Curvatures[Index]);
		}
	}
	return Banks;
}

/** A moving average over RunOut samples either side, so the bank turns gradually. */
std::vector<double> RunOutAverage(const std::vector<double>& Banks)
{
	const int Count = static_cast<int>(Banks.size());
	std::vector<double> Smoothed(Count);
	for (int Index = 0; Index < Count; ++Index)
	{
		const int First = std::max(Index - RunOut, 0);
		const int Last = std::min(Index + RunOut, Count - 1);
		double Sum = 0.0;
		for (int Other = First; Other <= Last; ++Other)
		{
			Sum += Banks[Other];
		}
		Smoothed[Index] = Sum / (Last - First + 1);
	}
	return Smoothed;
}

/**
 * The cross fall of a signed bank: 0 is the crown; as it grows the outer half turns about the centreline until both
 * halves form one plane at CrownFall, then the plane steepens.
 */
FCrossFall FallOfBank(double Bank)
{
	const double Size = std::abs(Bank);
	const double Inner = std::max(CrownFall, Size);
	const double Outer = CrownFall - 2.0 * CrownFall * std::min(Size / CrownFall, 1.0) - std::max(Size - CrownFall, 0.0);
	if (Bank >= 0.0)
	{
		return {Outer, Inner};
	}
	return {Inner, Outer};
}
}

std::vector<FCrossFall> CrossFalls(const FStreetPolyline& Points, const std::vector<FCrossFallTraits>& Traits)
{
	const std::vector<double> Banks = RunOutAverage(TargetBanks(Points, Traits));
	std::vector<FCrossFall> Falls;
	Falls.reserve(Banks.size());
	for (const double Bank : Banks)
	{
		Falls.push_back(FallOfBank(Bank));
	}
	return Falls;
}

double CrossFallDrop(const FCrossFall& Fall, double Offset)
{
	if (Offset >= 0.0)
	{
		return Fall.Right * Offset;
	}
	return Fall.Left * -Offset;
}
}
