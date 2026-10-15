#include "RoadModel.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Centreline.h"
#include "OsmTags.h"
#include "VerticalProfile.h"

namespace WorldBuilder
{
namespace
{
/** Distance between the samples of a centreline, metres. */
constexpr double SampleSpacing = 1.0;
/**
 * Trust in the ground under a bridge, against 1 elsewhere: next to nothing, so the profile bridges the gap smoothly,
 * but not zero, so a road that lies entirely under a bridge still has heights.
 */
constexpr double UnderBridgeTrust = 1e-4;

/** A piece's points in the chain's direction. */
FStreetPolyline PieceLine(const FSegmentNetwork& Network, const FChainPiece& Piece)
{
	const FStreetPolyline& Line = Network.Segments[Piece.Segment].Xy;
	if (Piece.bReversed)
	{
		return Polyline::Reversed(Line);
	}
	return Line;
}

/**
 * The cross fall traits of a piece's road: its straight fall from the one-way tags, and whether it banks (a road class
 * built for speed, outside town).
 */
FCrossFallTraits PieceTraits(const FSegmentNetwork& Network, const FChainPiece& Piece,
							 const std::unordered_map<int64_t, bool>& Urban)
{
	const FStreetWay& Way = *Network.Segments[Piece.Segment].Way;
	FCrossFallTraits Traits;
	const auto Found = Urban.find(Way.Id);
	const bool bInTown = Found != Urban.end() && Found->second;
	Traits.bBanksInCurves = !bInTown && RoadDesignFor(OsmTags::ValueOrEmpty(Way.Tags, "highway")).bBanksInCurves;
	if (OsmTags::IsOneway(Way.Tags))
	{
		// Traffic runs along the chain unless the piece or the one-way runs against it, but not both.
		const bool bAlongChain = Piece.bReversed == OsmTags::IsReversedOneway(Way.Tags);
		Traits.StraightBank = bAlongChain ? CrownFall : -CrownFall;
	}
	return Traits;
}

/** The segment of the chain that is longest, whose road class the chain takes. */
const FSegment& LongestSegment(const FSegmentNetwork& Network, const FRoadChain& Chain)
{
	const FSegment* Longest = &Network.Segments[Chain.Pieces.front().Segment];
	for (const FChainPiece& Piece : Chain.Pieces)
	{
		const FSegment& Segment = Network.Segments[Piece.Segment];
		if (Segment.Length > Longest->Length)
		{
			Longest = &Segment;
		}
	}
	return *Longest;
}

/** Appends Line to Chain without repeating the point they share. */
void AppendLine(FStreetPolyline& Chain, const FStreetPolyline& Line)
{
	const size_t Skip = Chain.empty() ? 0 : 1;
	Chain.insert(Chain.end(), Line.begin() + static_cast<std::ptrdiff_t>(std::min(Skip, Line.size())), Line.end());
}

/**
 * The chain's OSM line, and its samples about a metre apart, sampled piece by piece so that every node is a sample
 * (their indices go to NodeSamples), with the half width and cross fall traits of the piece each sample lies on.
 */
void SampleChain(const FSegmentNetwork& Network, const std::unordered_map<int64_t, bool>& Urban, FRoadChainModel& Model,
				 FStreetPolyline& Samples, std::vector<FCrossFallTraits>& Traits)
{
	Model.NodeSamples.push_back(0);
	for (const FChainPiece& Piece : Model.Chain.Pieces)
	{
		const FStreetPolyline Line = PieceLine(Network, Piece);
		AppendLine(Model.Original, Line);
		AppendLine(Samples, Polyline::Resample(Line, SampleSpacing));
		Model.HalfWidths.resize(Samples.size(), 0.5 * Network.Segments[Piece.Segment].Section.Width());
		Traits.resize(Samples.size(), PieceTraits(Network, Piece, Urban));
		Model.NodeSamples.push_back(static_cast<int>(Samples.size()) - 1);
	}
}

/** The samples that must not move: the chain's ends and every junction it runs through. */
std::vector<int> PinnedSamples(const FSegmentNetwork& Network, const FRoadChainModel& Model)
{
	std::vector<int> Pinned;
	const size_t Last = Model.Chain.Nodes.size() - 1;
	for (size_t Index = 0; Index <= Last; ++Index)
	{
		const bool bEnd = Index == 0 || Index == Last;
		if (bEnd || Network.NodeKind(Model.Chain.Nodes[Index]) == ENodeKind::Junction)
		{
			Pinned.push_back(Model.NodeSamples[Index]);
		}
	}
	return Pinned;
}

/** One chain's smooth centreline and cross fall, the ground under it and where bridges cross it. */
FRoadChainModel BuildChainPlan(const FSegmentNetwork& Network, const FRoadChain& Chain, const FGroundSampler& Ground,
							   const FBridgeCover& Bridges, const std::unordered_map<int64_t, bool>& Urban)
{
	FRoadChainModel Model;
	Model.Chain = Chain;
	const FSegment& Longest = LongestSegment(Network, Chain);
	Model.RoadClass = OsmTags::ValueOrEmpty(Longest.Way->Tags, "highway");
	Model.Name = OsmTags::ValueOrEmpty(Longest.Way->Tags, "name");
	Model.Design = RoadDesignFor(Model.RoadClass);
	FStreetPolyline Samples;
	std::vector<FCrossFallTraits> Traits;
	SampleChain(Network, Urban, Model, Samples, Traits);
	const FCentrelineFit Fit = FitCentreline(Samples, Model.Original, PinnedSamples(Network, Model),
											 Model.Design.PlanSmoothing, Model.Design.MaxDeviation);
	Model.Points = Fit.Points;
	Model.MaxDeviation = Fit.MaxDeviation;
	Model.Along = Polyline::Arclength(Model.Points);
	Model.Falls = CrossFalls(Model.Points, Traits);
	for (const FStreetPoint& Point : Model.Points)
	{
		Model.Ground.push_back(Ground(Point.X, Point.Y));
		Model.UnderBridge.push_back(Bridges.Covers(Point, Chain.Nodes.front(), Chain.Nodes.back()));
	}
	return Model;
}

/**
 * The height unknowns of every chain's samples. Nodes where chains meet or end get one unknown shared by every chain
 * there; all other samples get their own.
 */
std::vector<FProfileChain> ProfileChains(const FSegmentNetwork& Network, const std::vector<FRoadChainModel>& Models,
										 int& UnknownCount)
{
	std::unordered_map<int64_t, int> NodeUnknowns;
	std::vector<FProfileChain> Profiles;
	for (const FRoadChainModel& Model : Models)
	{
		FProfileChain Profile;
		Profile.Unknowns.assign(Model.Points.size(), -1);
		const size_t Last = Model.Chain.Nodes.size() - 1;
		for (size_t Index = 0; Index <= Last; ++Index)
		{
			const int64_t Node = Model.Chain.Nodes[Index];
			if (Index == 0 || Index == Last || Network.NodeKind(Node) == ENodeKind::Junction)
			{
				const auto [Found, bNew] = NodeUnknowns.try_emplace(Node, UnknownCount);
				UnknownCount += bNew ? 1 : 0;
				Profile.Unknowns[Model.NodeSamples[Index]] = Found->second;
			}
		}
		for (int& Unknown : Profile.Unknowns)
		{
			if (Unknown < 0)
			{
				Unknown = UnknownCount++;
			}
		}
		Profile.Ground = Model.Ground;
		for (const bool bUnderBridge : Model.UnderBridge)
		{
			Profile.GroundTrust.push_back(bUnderBridge ? UnderBridgeTrust : 1.0);
		}
		for (size_t Index = 0; Index + 1 < Model.Along.size(); ++Index)
		{
			Profile.Spacing.push_back(Model.Along[Index + 1] - Model.Along[Index]);
		}
		Profile.Smoothing = Model.Design.ProfileSmoothing;
		Profile.MaxGrade = Model.Design.MaxGrade;
		Profiles.push_back(std::move(Profile));
	}
	return Profiles;
}

/** Bending stiffness right at a junction where a road yields, against 1 elsewhere. */
constexpr double JunctionStiffness = 0.02;
/** Beyond the major road's kerb, the stiffness stays low this far, then returns to 1 over JunctionStiffnessRamp. */
constexpr double JunctionStiffnessReach = 6.0;
constexpr double JunctionStiffnessRamp = 10.0;

/** The bending stiffness factor of every sample of a chain: low near the junctions where it yields. */
std::vector<double> StiffnessNearJunctions(const FRoadChainModel& Chain, const std::vector<FRoadChainModel>& Chains)
{
	std::vector<double> Stiffness(Chain.Points.size(), 1.0);
	for (const FJunctionYield& Yield : Chain.Yields)
	{
		const double Calm = Chains[Yield.Major].HalfWidths[Yield.MajorNodeSample] + JunctionStiffnessReach;
		for (size_t Index = 0; Index < Stiffness.size(); ++Index)
		{
			const double FromNode = std::abs(Chain.Along[Index] - Chain.Along[Yield.NodeSample]);
			const double Ramp = std::clamp((FromNode - Calm) / JunctionStiffnessRamp, 0.0, 1.0);
			Stiffness[Index] = std::min(Stiffness[Index], JunctionStiffness + (1.0 - JunctionStiffness) * Ramp);
		}
	}
	return Stiffness;
}

/** Where a point meets a chain's centreline near one of its samples: the step it falls on and its offset. */
struct FFoot
{
	int Sample = 0;
	double Fraction = 0.0;
	double Offset = 0.0;
	double Distance = 1e300;
};

/** The foot of a point on the chain's centreline among the steps within Reach samples of Around. */
FFoot FootNear(const FRoadChainModel& Chain, int Around, int Reach, const FStreetPoint& Point)
{
	FFoot Best;
	const int First = std::max(Around - Reach, 0);
	const int Last = std::min(Around + Reach, static_cast<int>(Chain.Points.size()) - 1);
	for (int Index = First; Index < Last; ++Index)
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
		if (Distance >= Best.Distance)
		{
			continue;
		}
		const double Side = Dot(Point - Foot, Polyline::RightOf(Polyline::Unit(Step))) >= 0.0 ? 1.0 : -1.0;
		Best = {Index, Fraction, Side * Distance, Distance};
	}
	return Best;
}

/**
 * Ties that hold every yielding chain's samples inside the major road's carriageway to the major road's surface from
 * a first fit (Heights, by unknown), so a side street's profile starts where it really meets the main road: at its
 * surface, not at its centreline. Holding them to fixed targets leaves the main road's own fit alone.
 */
std::vector<FProfileTie> JunctionTies(const std::vector<FRoadChainModel>& Chains, const std::vector<FProfileChain>& Profiles,
									  const std::vector<double>& Heights)
{
	std::vector<FProfileTie> Ties;
	for (size_t ChainIndex = 0; ChainIndex < Chains.size(); ++ChainIndex)
	{
		const FRoadChainModel& Minor = Chains[ChainIndex];
		for (const FJunctionYield& Yield : Minor.Yields)
		{
			const FRoadChainModel& Major = Chains[Yield.Major];
			const double HalfWidth = Major.HalfWidths[Yield.MajorNodeSample];
			const int Reach = static_cast<int>(std::ceil(HalfWidth / SampleSpacing)) + 10;
			for (int Index = 0; Index < static_cast<int>(Minor.Points.size()); ++Index)
			{
				if (Index == Yield.NodeSample
					|| std::abs(Minor.Along[Index] - Minor.Along[Yield.NodeSample]) > HalfWidth)
				{
					continue;
				}
				const FFoot Foot = FootNear(Major, Yield.MajorNodeSample, Reach, Minor.Points[Index]);
				if (Foot.Distance > Major.HalfWidths[Foot.Sample])
				{
					continue;
				}
				const FCrossFall& FallBefore = Major.Falls[Foot.Sample];
				const FCrossFall& FallAfter = Major.Falls[Foot.Sample + 1];
				const FCrossFall Fall{FallBefore.Left + (FallAfter.Left - FallBefore.Left) * Foot.Fraction,
									  FallBefore.Right + (FallAfter.Right - FallBefore.Right) * Foot.Fraction};
				const double Before = Heights[Profiles[Yield.Major].Unknowns[Foot.Sample]];
				const double After = Heights[Profiles[Yield.Major].Unknowns[Foot.Sample + 1]];
				const double Centre = Before + (After - Before) * Foot.Fraction;
				Ties.push_back({Profiles[ChainIndex].Unknowns[Index], Centre - CrossFallDrop(Fall, Foot.Offset)});
			}
		}
	}
	return Ties;
}
}

FRoadModel BuildRoadModel(const FSegmentNetwork& Network, const FGroundSampler& Ground, const FBridgeCover& Bridges,
						  const std::unordered_map<int64_t, bool>& Urban)
{
	FRoadModel Model;
	for (const FRoadChain& Chain : BuildRoadChains(Network))
	{
		Model.Chains.push_back(BuildChainPlan(Network, Chain, Ground, Bridges, Urban));
	}
	AssignJunctionPriorities(Network, Model.Chains);
	int UnknownCount = 0;
	const std::vector<FProfileChain> Profiles = ProfileChains(Network, Model.Chains, UnknownCount);
	// First every road on its own ground, then again with the side streets held to the main roads' surfaces where they
	// cross them, and free to bend more on their way out of the junction.
	const FProfileResult Untied = SolveProfiles(UnknownCount, Profiles, {});
	std::vector<FProfileChain> Yielding = Profiles;
	for (size_t Index = 0; Index < Model.Chains.size(); ++Index)
	{
		Yielding[Index].Stiffness = StiffnessNearJunctions(Model.Chains[Index], Model.Chains);
	}
	const FProfileResult Result = SolveProfiles(UnknownCount, Yielding,
												JunctionTies(Model.Chains, Profiles, Untied.Heights));
	Model.GradeLimited = Result.GradeLimited;
	for (size_t Index = 0; Index < Model.Chains.size(); ++Index)
	{
		for (const int Unknown : Profiles[Index].Unknowns)
		{
			Model.Chains[Index].Heights.push_back(Result.Heights[Unknown]);
		}
	}
	return Model;
}
}
