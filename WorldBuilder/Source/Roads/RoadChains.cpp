#include "RoadChains.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>

#include "OsmTags.h"

namespace WorldBuilder
{
namespace
{
/** Most a road may turn at a junction and still count as going straight through, degrees. */
constexpr double ThroughAngleDegrees = 30.0;

using FPartners = std::map<FSegmentEnd, FSegmentEnd>;

/** Two arms at a junction that could be one road going straight through, and how far from straight they are. */
struct FThroughCandidate
{
	FSegmentEnd First;
	FSegmentEnd Second;
	double Deviation = 0.0;
};

/** True when two arms belong to the same road: the same way, the same name, or both unnamed and of one class. */
bool IsSameRoad(const FSegment& First, const FSegment& Second)
{
	if (First.Way == Second.Way)
	{
		return true;
	}
	const std::string FirstName = OsmTags::ValueOrEmpty(First.Way->Tags, "name");
	const std::string SecondName = OsmTags::ValueOrEmpty(Second.Way->Tags, "name");
	if (!FirstName.empty() || !SecondName.empty())
	{
		return FirstName == SecondName;
	}
	return OsmTags::ValueOrEmpty(First.Way->Tags, "highway") == OsmTags::ValueOrEmpty(Second.Way->Tags, "highway");
}

/** Angle in radians between leaving along one arm and arriving along the other, 0 for a straight line. */
double TurnAngle(const FSegmentNetwork& Network, const FSegmentEnd& First, const FSegmentEnd& Second)
{
	const FStreetPoint Out = Network.ArmDirection(First);
	const FStreetPoint In = -Network.ArmDirection(Second);
	return std::acos(std::clamp(Dot(Out, In), -1.0, 1.0));
}

/** Pairs the arms of one junction that go straight through, the straightest pairs first. */
void PairThroughArms(const FSegmentNetwork& Network, const std::vector<FSegmentEnd>& Ends, FPartners& Partners)
{
	const double Limit = ThroughAngleDegrees * 3.14159265358979323846 / 180.0;
	std::vector<FThroughCandidate> Candidates;
	for (size_t First = 0; First < Ends.size(); ++First)
	{
		for (size_t Second = First + 1; Second < Ends.size(); ++Second)
		{
			if (Ends[First].Segment == Ends[Second].Segment)
			{
				continue;
			}
			const FSegment& FirstSegment = Network.Segments[Ends[First].Segment];
			const FSegment& SecondSegment = Network.Segments[Ends[Second].Segment];
			if (!IsSameRoad(FirstSegment, SecondSegment))
			{
				continue;
			}
			const double Deviation = TurnAngle(Network, Ends[First], Ends[Second]);
			if (Deviation < Limit)
			{
				Candidates.push_back({Ends[First], Ends[Second], Deviation});
			}
		}
	}
	std::sort(Candidates.begin(), Candidates.end(),
			  [](const FThroughCandidate& A, const FThroughCandidate& B) { return A.Deviation < B.Deviation; });
	for (const FThroughCandidate& Candidate : Candidates)
	{
		if (Partners.count(Candidate.First) || Partners.count(Candidate.Second))
		{
			continue;
		}
		Partners[Candidate.First] = Candidate.Second;
		Partners[Candidate.Second] = Candidate.First;
	}
}

/** For every segment end, the end the road carries on into: at continuations always, at junctions when straight. */
FPartners FindPartners(const FSegmentNetwork& Network)
{
	FPartners Partners;
	for (const int64_t Node : Network.NodeOrder)
	{
		const std::vector<FSegmentEnd>& Ends = Network.EndsAt.at(Node);
		const ENodeKind Kind = Network.NodeKind(Node);
		if (Kind == ENodeKind::Continuation && Ends.size() == 2)
		{
			Partners[Ends[0]] = Ends[1];
			Partners[Ends[1]] = Ends[0];
		}
		else if (Kind == ENodeKind::Junction)
		{
			PairThroughArms(Network, Ends, Partners);
		}
	}
	return Partners;
}

/** The end of a piece at its far side (along the chain), or at its near side. */
FSegmentEnd FarEnd(const FChainPiece& Piece)
{
	return {Piece.Segment, Piece.bReversed};
}

FSegmentEnd NearEnd(const FChainPiece& Piece)
{
	return {Piece.Segment, !Piece.bReversed};
}

/** The piece that enters a segment at the given end and runs away from it. */
FChainPiece PieceEnteringAt(const FSegmentEnd& End)
{
	return {End.Segment, !End.bAtStart};
}

/** Follows the road from the last piece onwards until it ends, meets a used segment or comes back round. */
void ExtendForward(const FPartners& Partners, std::vector<bool>& Used, std::deque<FChainPiece>& Pieces, bool& bClosed)
{
	while (true)
	{
		const auto Partner = Partners.find(FarEnd(Pieces.back()));
		if (Partner == Partners.end())
		{
			return;
		}
		const FChainPiece Next = PieceEnteringAt(Partner->second);
		if (Used[Next.Segment])
		{
			bClosed = Next.Segment == Pieces.front().Segment && Next.bReversed == Pieces.front().bReversed;
			return;
		}
		Used[Next.Segment] = true;
		Pieces.push_back(Next);
	}
}

/** Follows the road from the first piece backwards until it ends or meets a used segment. */
void ExtendBackward(const FPartners& Partners, std::vector<bool>& Used, std::deque<FChainPiece>& Pieces)
{
	while (true)
	{
		const auto Partner = Partners.find(NearEnd(Pieces.front()));
		if (Partner == Partners.end())
		{
			return;
		}
		// The piece before runs towards the partner end, so it leaves from the segment's other end.
		const FChainPiece Previous{Partner->second.Segment, Partner->second.bAtStart};
		if (Used[Previous.Segment])
		{
			return;
		}
		Used[Previous.Segment] = true;
		Pieces.push_front(Previous);
	}
}

/** The node at the start of each piece and at the end of the last. */
std::vector<int64_t> ChainNodes(const FSegmentNetwork& Network, const std::vector<FChainPiece>& Pieces)
{
	std::vector<int64_t> Nodes;
	for (const FChainPiece& Piece : Pieces)
	{
		Nodes.push_back(Network.NodeOf(NearEnd(Piece)));
	}
	Nodes.push_back(Network.NodeOf(FarEnd(Pieces.back())));
	return Nodes;
}
}

std::vector<FRoadChain> BuildRoadChains(const FSegmentNetwork& Network)
{
	const FPartners Partners = FindPartners(Network);
	std::vector<bool> Used(Network.Segments.size(), false);
	std::vector<FRoadChain> Chains;
	for (const FSegment& Segment : Network.Segments)
	{
		if (Used[Segment.Id])
		{
			continue;
		}
		Used[Segment.Id] = true;
		std::deque<FChainPiece> Pieces{{Segment.Id, false}};
		FRoadChain Chain;
		ExtendForward(Partners, Used, Pieces, Chain.bClosed);
		if (!Chain.bClosed)
		{
			ExtendBackward(Partners, Used, Pieces);
		}
		Chain.Pieces.assign(Pieces.begin(), Pieces.end());
		Chain.Nodes = ChainNodes(Network, Chain.Pieces);
		Chains.push_back(std::move(Chain));
	}
	return Chains;
}
}
