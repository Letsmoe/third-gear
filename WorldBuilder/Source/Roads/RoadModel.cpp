#include "RoadModel.h"

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
}

FRoadModel BuildRoadModel(const FSegmentNetwork& Network, const FGroundSampler& Ground, const FBridgeCover& Bridges,
						  const std::unordered_map<int64_t, bool>& Urban)
{
	FRoadModel Model;
	for (const FRoadChain& Chain : BuildRoadChains(Network))
	{
		Model.Chains.push_back(BuildChainPlan(Network, Chain, Ground, Bridges, Urban));
	}
	int UnknownCount = 0;
	const std::vector<FProfileChain> Profiles = ProfileChains(Network, Model.Chains, UnknownCount);
	const FProfileResult Result = SolveProfiles(UnknownCount, Profiles);
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
