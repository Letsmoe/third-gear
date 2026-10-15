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
 * Softening of the blend weights 1 / (distance outside the kerb + this)², metres: a point inside two roads at once
 * (a junction) takes both about equally, a point beside one road mostly that road.
 */
constexpr double BlendSoftening = 0.25;

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
		double Along = 0.0;
		if (LengthSquared > 1e-12)
		{
			Along = std::clamp(Dot(Point - Start, Step) / LengthSquared, 0.0, 1.0);
		}
		const double Distance = Norm(Point - (Start + Step * Along));
		if (Distance >= Nearest.Distance)
		{
			continue;
		}
		Nearest.Distance = Distance;
		Nearest.Height = Chain.Heights[Index] * (1.0 - Along) + Chain.Heights[Index + 1] * Along;
		Nearest.HalfWidth = Chain.HalfWidths[Index] * (1.0 - Along) + Chain.HalfWidths[Index + 1] * Along;
	}
	return Nearest;
}

std::optional<double> FRoadHeights::At(double X, double Y) const
{
	// The nearest point of every chain around, over all of its runs.
	std::unordered_map<int, FNearest> ByChain;
	for (const int RunIndex : Index.Query({X, Y, X, Y}))
	{
		const FRun& Run = Runs[RunIndex];
		const FNearest Nearest = NearestOnRun(Run, X, Y);
		FNearest& Best = ByChain[Run.Chain];
		if (Nearest.Distance < Best.Distance)
		{
			Best = Nearest;
		}
	}
	double WeightSum = 0.0;
	double HeightSum = 0.0;
	for (const auto& [ChainIndex, Nearest] : ByChain)
	{
		const double Outside = std::max(Nearest.Distance - Nearest.HalfWidth, 0.0);
		if (Outside > Reach)
		{
			continue;
		}
		const double Weight = 1.0 / ((Outside + BlendSoftening) * (Outside + BlendSoftening));
		WeightSum += Weight;
		HeightSum += Weight * Nearest.Height;
	}
	if (WeightSum <= 0.0)
	{
		return std::nullopt;
	}
	return HeightSum / WeightSum;
}
}
