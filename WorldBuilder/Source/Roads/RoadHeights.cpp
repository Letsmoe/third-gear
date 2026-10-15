#include "RoadHeights.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace WorldBuilder
{
namespace
{
/** Samples per run in the spatial index. */
constexpr int RunLength = 16;
/**
 * Softening of the blend weights 1 / (distance outside the kerb + this)², metres: a point beside one road takes mostly
 * that road's kerb height, a point between two (a corner) both.
 */
constexpr double BlendSoftening = 0.25;
/** Distance from the major road's kerb line over which a yielding road turns from its kerb height to its own shape. */
constexpr double AdaptLength = FRoadHeights::Reach;
/** Slack along the minor road beyond the major road's half width and AdaptLength, for skewed junctions, metres. */
constexpr double AdaptSlack = 4.0;

/** The bounding box of a run's samples grown by its widest half width and the reach. */
FBox RunBox(const FRoadChainModel& Chain, int First, int Last)
{
	FBox Box{1e300, 1e300, -1e300, -1e300};
	double Widest = 0.0;
	for (int Index = First; Index <= Last; ++Index)
	{
		const FStreetPoint& Point = Chain.Points[Index];
		Box = {std::min(Box.X0, Point.X), std::min(Box.Y0, Point.Y), std::max(Box.X1, Point.X), std::max(Box.Y1, Point.Y)};
		Widest = std::max(Widest, Chain.HalfWidths[Index]);
	}
	const double Grow = Widest + FRoadHeights::Reach;
	return {Box.X0 - Grow, Box.Y0 - Grow, Box.X1 + Grow, Box.Y1 + Grow};
}

/** Linear interpolation between two values. */
double Lerp(double First, double Second, double Fraction)
{
	return First * (1.0 - Fraction) + Second * Fraction;
}

/** 0 at 0, 1 at 1 and beyond, with level ends. */
double SmoothStep(double Fraction)
{
	const double Clamped = std::clamp(Fraction, 0.0, 1.0);
	return Clamped * Clamped * (3.0 - 2.0 * Clamped);
}

/** How far a foot lies outside its road's kerb, 0 inside. */
double Outside(double Distance, double HalfWidth)
{
	return std::max(Distance - HalfWidth, 0.0);
}
}

FRoadHeights::FRoadHeights(const FRoadModel& InModel) : Model(&InModel)
{
	for (int ChainIndex = 0; ChainIndex < static_cast<int>(InModel.Chains.size()); ++ChainIndex)
	{
		const FRoadChainModel& Chain = InModel.Chains[ChainIndex];
		const int Count = static_cast<int>(Chain.Points.size());
		for (int First = 0; First + 1 < Count; First += RunLength)
		{
			// Runs overlap by one sample, so every step between samples belongs to a run.
			const int Last = std::min(First + RunLength, Count - 1);
			Index.Insert(RunBox(Chain, First, Last), static_cast<int>(Runs.size()));
			Runs.push_back({ChainIndex, First, Last});
		}
	}
}

FRoadHeights::FNearest FRoadHeights::NearestOnRun(const FRun& Run, double X, double Y) const
{
	const FRoadChainModel& Chain = Model->Chains[Run.Chain];
	const FStreetPoint Point{X, Y};
	FNearest Nearest;
	for (int Index = Run.First; Index < Run.Last; ++Index)
	{
		const FStreetPoint& Start = Chain.Points[Index];
		const FStreetPoint Step = Chain.Points[Index + 1] - Start;
		const double LengthSquared = Dot(Step, Step);
		double Fraction = 0.0;
		if (LengthSquared > 1e-12)
		{
			Fraction = std::clamp(Dot(Point - Start, Step) / LengthSquared, 0.0, 1.0);
		}
		const FStreetPoint Foot = Start + Step * Fraction;
		const double Distance = Norm(Point - Foot);
		if (Distance >= Nearest.Distance)
		{
			continue;
		}
		const FCrossFall& First = Chain.Falls[Index];
		const FCrossFall& Second = Chain.Falls[Index + 1];
		const double Side = Dot(Point - Foot, Polyline::RightOf(Polyline::Unit(Step))) >= 0.0 ? 1.0 : -1.0;
		Nearest.Chain = Run.Chain;
		Nearest.Distance = Distance;
		Nearest.Along = Lerp(Chain.Along[Index], Chain.Along[Index + 1], Fraction);
		Nearest.Offset = Side * Distance;
		Nearest.Centre = Lerp(Chain.Heights[Index], Chain.Heights[Index + 1], Fraction);
		Nearest.HalfWidth = Lerp(Chain.HalfWidths[Index], Chain.HalfWidths[Index + 1], Fraction);
		Nearest.Fall = {Lerp(First.Left, Second.Left, Fraction), Lerp(First.Right, Second.Right, Fraction)};
	}
	return Nearest;
}

std::vector<FRoadHeights::FNearest> FRoadHeights::NearestByChain(double X, double Y) const
{
	std::unordered_map<int, FNearest> ByChain;
	for (const int RunIndex : Index.Query({X, Y, X, Y}))
	{
		const FNearest Nearest = NearestOnRun(Runs[RunIndex], X, Y);
		FNearest& Best = ByChain[Nearest.Chain];
		if (Nearest.Distance < Best.Distance)
		{
			Best = Nearest;
		}
	}
	std::vector<FNearest> Found;
	for (const auto& [Chain, Nearest] : ByChain)
	{
		Found.push_back(Nearest);
	}
	// The map's order isn't fixed; sorting keeps the blend's sums, and so the heights, identical from run to run.
	std::sort(Found.begin(), Found.end(), [](const FNearest& A, const FNearest& B) { return A.Chain < B.Chain; });
	return Found;
}

FRoadHeights::FNearest FRoadHeights::NearestOnChain(int Chain, double X, double Y) const
{
	FNearest Best;
	for (const int RunIndex : Index.Query({X, Y, X, Y}))
	{
		if (Runs[RunIndex].Chain != Chain)
		{
			continue;
		}
		const FNearest Nearest = NearestOnRun(Runs[RunIndex], X, Y);
		if (Nearest.Distance < Best.Distance)
		{
			Best = Nearest;
		}
	}
	return Best;
}

double FRoadHeights::SurfaceAt(const FNearest& Nearest, double X, double Y) const
{
	// Beyond the kerb the road keeps its edge height, which the pavement and the kerb stand on.
	const double Offset = std::clamp(Nearest.Offset, -Nearest.HalfWidth, Nearest.HalfWidth);
	double Height = Nearest.Centre - CrossFallDrop(Nearest.Fall, Offset);
	const FRoadChainModel& Chain = Model->Chains[Nearest.Chain];
	for (const FJunctionYield& Yield : Chain.Yields)
	{
		const FNearest Major = NearestOnChain(Yield.Major, X, Y);
		if (Major.Chain < 0)
		{
			continue;
		}
		const double FromNode = std::abs(Nearest.Along - Chain.Along[Yield.NodeSample]);
		if (FromNode > Major.HalfWidth + AdaptLength + AdaptSlack)
		{
			continue;
		}
		const double MajorEdge = Major.Centre
			- CrossFallDrop(Major.Fall, std::clamp(Major.Offset, -Major.HalfWidth, Major.HalfWidth));
		Height = Lerp(MajorEdge, Height, SmoothStep(Outside(Major.Distance, Major.HalfWidth) / AdaptLength));
		break;
	}
	return Height;
}

bool FRoadHeights::YieldsTo(int Minor, int Major) const
{
	for (const FJunctionYield& Yield : Model->Chains[Minor].Yields)
	{
		if (Yield.Major == Major)
		{
			return true;
		}
	}
	return false;
}

std::optional<double> FRoadHeights::At(double X, double Y) const
{
	const std::vector<FNearest> Around = NearestByChain(X, Y);

	// Inside a carriageway: the road that yields to none of the others whose carriageway the point is also in.
	std::vector<const FNearest*> Inside;
	for (const FNearest& Nearest : Around)
	{
		if (Nearest.Distance <= Nearest.HalfWidth)
		{
			Inside.push_back(&Nearest);
		}
	}
	for (const FNearest* Candidate : Inside)
	{
		const bool bYields = std::any_of(Inside.begin(), Inside.end(), [&](const FNearest* Other) {
			return Other != Candidate && YieldsTo(Candidate->Chain, Other->Chain);
		});
		if (!bYields)
		{
			return SurfaceAt(*Candidate, X, Y);
		}
	}

	// Outside every carriageway: the kerb heights of the roads around, blended by distance.
	double WeightSum = 0.0;
	double HeightSum = 0.0;
	for (const FNearest& Nearest : Around)
	{
		const double Beyond = Outside(Nearest.Distance, Nearest.HalfWidth);
		if (Beyond > Reach)
		{
			continue;
		}
		const double Weight = 1.0 / ((Beyond + BlendSoftening) * (Beyond + BlendSoftening));
		WeightSum += Weight;
		HeightSum += Weight * SurfaceAt(Nearest, X, Y);
	}
	if (WeightSum <= 0.0)
	{
		return std::nullopt;
	}
	return HeightSum / WeightSum;
}
}
