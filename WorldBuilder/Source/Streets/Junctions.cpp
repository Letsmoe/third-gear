#include "Junctions.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

#include "Assumptions.h"
#include "Corrections.h"
#include "OsmTags.h"
#include "Splits.h"

namespace WorldBuilder
{
namespace
{
constexpr double SignalCellSize = Assumptions::SignalJunctionRadius;

/** Lines the through road carries across a junction (edge lines, broken across side roads). */
bool IsContinuedKind(ELineKind Kind)
{
	return Kind == ELineKind::Edge || Kind == ELineKind::Cycle;
}

/** The lines whose crossings with the other road's get a cross instead. */
bool IsCrossedKind(ELineKind Kind)
{
	return Kind == ELineKind::Centre || Kind == ELineKind::Lane;
}

int64_t CellKey(int64_t CellX, int64_t CellY)
{
	return CellX * 1000003 + CellY;
}

int64_t CellIndex(double Coordinate)
{
	return static_cast<int64_t>(std::floor(Coordinate / SignalCellSize));
}

/**
 * Degrees a path turns to the left from the arriving direction into the leaving one (negative to the right). The
 * world frame is mirrored (y south), so a left turn has a negative cross product.
 */
double TurnAngle(const FStreetPoint& Arriving, const FStreetPoint& Leaving)
{
	const double Cross = Arriving.X * Leaving.Y - Arriving.Y * Leaving.X;
	const double DotProduct = Dot(Arriving, Leaving);
	return std::atan2(-Cross, DotProduct) * 180.0 / std::numbers::pi;
}

/** How many lanes go one way, from the lane lines between them (facing out of the node: Backward comes in). */
int LanesOneWay(const FLineOffsets& Lines, ETravel Travel)
{
	int Dividers = 0;
	for (const auto& [Key, Offset] : Lines)
	{
		if (Key.Kind == ELineKind::Lane && Key.Travel == Travel)
		{
			++Dividers;
		}
	}
	return Dividers + 1;
}

/**
 * The line on the driver's right of the LaneFromCentre-th lane counted from the centre, among LaneCount lanes going
 * one way (lane lines are counted from the kerb, so it is the (LaneCount - lane)-th; the kerb-side lane has the edge).
 */
FLineKey LineRightOfLane(ETravel Travel, int LaneCount, int LaneFromCentre)
{
	const int Index = LaneCount - LaneFromCentre;
	if (Index <= 0)
	{
		if (Travel == ETravel::Forward)
		{
			return {ELineKind::Edge, ESide::Right};
		}
		return {ELineKind::Edge, ESide::Left};
	}
	return {ELineKind::Lane, ESide::None, Travel, Index};
}

/** How many lanes coming into the node at this end only turn left, counted from the driver's left. */
int ExclusiveLeftLanes(const FTags& Tags, const FSegmentEnd& End)
{
	ETravel TowardNode = ETravel::Forward;
	if (End.bAtStart)
	{
		TowardNode = ETravel::Backward;  // the segment leaves the node along the way, so traffic comes in against it
	}
	int Count = 0;
	for (const std::string& Value : OsmTags::TurnLanes(Tags, TowardNode))
	{
		bool bOnlyLeft = true;
		for (const std::string& Direction : OsmTags::SplitText(Value, ';'))
		{
			bOnlyLeft = bOnlyLeft && (Direction == "left" || Direction == "slight_left" || Direction == "sharp_left");
		}
		if (!bOnlyLeft)
		{
			break;
		}
		++Count;
	}
	return Count;
}

/** The point where two straight segments cross, or nothing. */
std::optional<FStreetPoint> SegmentIntersection(const FStreetPoint& FirstStart, const FStreetPoint& FirstEnd,
												const FStreetPoint& SecondStart, const FStreetPoint& SecondEnd)
{
	const FStreetPoint First = FirstEnd - FirstStart;
	const FStreetPoint Second = SecondEnd - SecondStart;
	const double Denominator = First.X * Second.Y - First.Y * Second.X;
	if (std::abs(Denominator) < 1e-9)
	{
		return std::nullopt;
	}
	const FStreetPoint Gap = SecondStart - FirstStart;
	const double AlongFirst = (Gap.X * Second.Y - Gap.Y * Second.X) / Denominator;
	const double AlongSecond = (Gap.X * First.Y - Gap.Y * First.X) / Denominator;
	if (!(0.0 <= AlongFirst && AlongFirst <= 1.0 && 0.0 <= AlongSecond && AlongSecond <= 1.0))
	{
		return std::nullopt;
	}
	return FirstStart + First * AlongFirst;
}

/** The angle between two lines in degrees, 0 to 90. */
double CrossingAngle(const FStreetPoint& First, const FStreetPoint& Second)
{
	const double Cosine = std::abs(Dot(Polyline::Unit(First), Polyline::Unit(Second)));
	return std::acos(std::min(Cosine, 1.0)) * 180.0 / std::numbers::pi;
}

/** The two bars of a cross, each along one of the crossing lines. */
std::vector<FMarking> CrossBars(const FStreetPoint& Centre, const FStreetPoint& FirstDirection,
								const FStreetPoint& SecondDirection)
{
	const double Half = Assumptions::JunctionCrossLength / 2;
	std::vector<FMarking> Bars;
	for (const FStreetPoint& Direction : {FirstDirection, SecondDirection})
	{
		const FStreetPoint Along = Polyline::Unit(Direction) * Half;
		Bars.push_back({"solid", {Centre - Along, Centre + Along}});
	}
	return Bars;
}

/** A straight line across a junction between two points of arms, and the road it belongs to. */
struct FChord
{
	int Road = 0;
	FStreetPoint Start;
	FStreetPoint End;
};
}

FJunctionLines::FJunctionLines(const FSegmentNetwork& InNetwork, const std::vector<FStreetPoint>& SignalPoints)
	: Network(InNetwork), Signals(SignalPoints)
{
	for (int64_t Node : Network.NodeOrder)
	{
		if (Network.NodeKind(Node) == ENodeKind::Junction)
		{
			Junctions.push_back(Node);
		}
	}
	for (const FStreetPoint& Signal : Signals)
	{
		SignalCells[CellKey(CellIndex(Signal.X), CellIndex(Signal.Y))].push_back(Signal);
	}
}

const FSegment& FJunctionLines::SegmentOf(const FSegmentEnd& End) const
{
	return Network.Segments[End.Segment];
}

int FJunctionLines::Rank(const FSegmentEnd& End) const
{
	return Assumptions::RoadClassRank(OsmTags::Highway(SegmentOf(End).Way->Tags));
}

bool FJunctionLines::IsMinor(const FSegmentEnd& End) const
{
	return WorldBuilder::IsMinor(Network, End);
}

bool FJunctionLines::IsSignalised(int64_t Node) const
{
	const FStreetPoint Position = Network.NodePoints.at(Node);
	for (int64_t CellX = CellIndex(Position.X) - 1; CellX <= CellIndex(Position.X) + 1; ++CellX)
	{
		for (int64_t CellY = CellIndex(Position.Y) - 1; CellY <= CellIndex(Position.Y) + 1; ++CellY)
		{
			const auto Cell = SignalCells.find(CellKey(CellX, CellY));
			if (Cell == SignalCells.end())
			{
				continue;
			}
			for (const FStreetPoint& Signal : Cell->second)
			{
				if (Norm(Signal - Position) <= Assumptions::SignalJunctionRadius)
				{
					return true;
				}
			}
		}
	}
	return false;
}

FStreetPoint FJunctionLines::MouthPoint(const FSegmentEnd& End, double Offset, double Distance) const
{
	const FStreetPolyline Line = Network.ArmLine(End);
	const std::vector<double> Along = Polyline::Arclength(Line);
	Distance = std::min(Distance, Along.back());
	return Polyline::PointAt(Line, Along, Distance)
		+ Polyline::RightOf(Polyline::DirectionAt(Line, Along, Distance)) * Offset;
}

FStreetPoint FJunctionLines::DirectionAtMouth(const FSegmentEnd& End, double Distance) const
{
	const FStreetPolyline Line = Network.ArmLine(End);
	const std::vector<double> Along = Polyline::Arclength(Line);
	return Polyline::DirectionAt(Line, Along, std::min(Distance, Along.back()));
}

std::vector<FSegmentEnd> FJunctionLines::ArmsThatInterrupt(const FSegmentEnd& End,
														   const std::vector<FSegmentEnd>& Ends) const
{
	std::vector<FSegmentEnd> Others;
	for (const FSegmentEnd& Other : Ends)
	{
		if (Other != End)
		{
			Others.push_back(Other);
		}
	}
	if (IsMinor(End))
	{
		return Others;
	}
	std::vector<FSegmentEnd> Major;
	for (const FSegmentEnd& Other : Others)
	{
		if (!IsMinor(Other))
		{
			Major.push_back(Other);
		}
	}
	if (Major.size() == 1 && Deflection(Network, End, Major[0]) <= Assumptions::ThroughMaxDeflectionDegrees)
	{
		return {};
	}
	return Major;
}

void FJunctionLines::AddMergeMouths(const FSegmentEnd& End, const std::vector<FSegmentEnd>& Ends,
									const std::pair<FSegmentEnd, FSegmentEnd>& ThroughPair, double Mouth,
									FMouths& Result) const
{
	const FStreetPoint Direction = Network.ArmDirection(End);
	for (const FSegmentEnd& Other : Ends)
	{
		if (Other == ThroughPair.first || Other == ThroughPair.second || IsMinor(Other))
		{
			continue;
		}
		if (Deflection(Network, End, Other) < 180.0 - Assumptions::MergeMaxAngle)
		{
			continue;
		}
		ESide Side = ESide::Left;
		if (Dot(Network.ArmDirection(Other), Polyline::RightOf(Direction)) > 0)
		{
			Side = ESide::Right;
		}
		const double Distance = std::max(Network.ClearDistance(End, {Other}, Assumptions::MergeGap), Mouth);
		for (ELineKind Kind : {ELineKind::Edge, ELineKind::Cycle, ELineKind::CycleOuter})
		{
			double& Stored = Result.ByLine[{End, FLineKey{Kind, Side}}];
			Stored = std::max(Distance, Stored);
		}
	}
}

FMouths FJunctionLines::Mouths() const
{
	FMouths Result;
	for (int64_t Node : Junctions)
	{
		const std::vector<FSegmentEnd>& Ends = Network.EndsAt.at(Node);
		if (FindSplit(Network, Node).has_value())
		{
			continue;  // the carriageways continue the two-way road's lines (layout.py)
		}
		for (const FSegmentEnd& End : Ends)
		{
			const std::vector<FSegmentEnd> Others = ArmsThatInterrupt(End, Ends);
			if (Others.empty())
			{
				Result.ByEnd[End] = 0.0;
				continue;
			}
			Result.ByEnd[End] = Network.ClearDistance(End, Others) + Assumptions::CornerRadius;
		}
		for (const auto& ThroughPair : ThroughPairs(Ends))
		{
			for (const FSegmentEnd& End : {ThroughPair.first, ThroughPair.second})
			{
				AddMergeMouths(End, Ends, ThroughPair, Result.ByEnd.at(End), Result);
			}
		}
	}
	return Result;
}

std::vector<std::pair<FSegmentEnd, FSegmentEnd>> FJunctionLines::ThroughPairs(const std::vector<FSegmentEnd>& Ends) const
{
	struct FCandidate
	{
		int NegativeRank;
		double Deflection;
		FSegmentEnd First;
		FSegmentEnd Second;
	};
	std::vector<FCandidate> Candidates;
	for (size_t Index = 0; Index < Ends.size(); ++Index)
	{
		for (size_t Next = Index + 1; Next < Ends.size(); ++Next)
		{
			const FSegmentEnd& First = Ends[Index];
			const FSegmentEnd& Second = Ends[Next];
			if (IsMinor(First) || IsMinor(Second))
			{
				continue;
			}
			const double Turn = Deflection(Network, First, Second);
			if (Turn <= Assumptions::ThroughMaxDeflectionDegrees)
			{
				Candidates.push_back({-std::min(Rank(First), Rank(Second)), Turn, First, Second});
			}
		}
	}
	std::stable_sort(Candidates.begin(), Candidates.end(), [](const FCandidate& Left, const FCandidate& Right) {
		if (Left.NegativeRank != Right.NegativeRank)
		{
			return Left.NegativeRank < Right.NegativeRank;
		}
		return Left.Deflection < Right.Deflection;
	});
	std::vector<std::pair<FSegmentEnd, FSegmentEnd>> Pairs;
	std::set<FSegmentEnd> Used;
	for (const FCandidate& Candidate : Candidates)
	{
		if (Used.count(Candidate.First) > 0 || Used.count(Candidate.Second) > 0)
		{
			continue;
		}
		Pairs.emplace_back(Candidate.First, Candidate.Second);
		Used.insert(Candidate.First);
		Used.insert(Candidate.Second);
	}
	return Pairs;
}

std::vector<FMarking> FJunctionLines::GuideLines(const std::vector<FSegmentLayout>& Layouts, const FMouths& Mouths) const
{
	std::vector<FMarking> Result;
	const auto Append = [&Result](std::vector<FMarking> More) {
		Result.insert(Result.end(), std::make_move_iterator(More.begin()), std::make_move_iterator(More.end()));
	};
	for (int64_t Node : Junctions)
	{
		const std::vector<FSegmentEnd>& Ends = Network.EndsAt.at(Node);
		if (FindSplit(Network, Node).has_value())
		{
			continue;
		}
		const auto Pairs = ThroughPairs(Ends);
		for (const auto& [Entering, Leaving] : Pairs)
		{
			Append(ThroughGuides(Layouts, Entering, Leaving, Ends, Mouths));
		}
		Append(Crosses(Layouts, Ends, Pairs, Mouths));
		if (!IsSignalised(Node))
		{
			continue;
		}
		for (const FSegmentEnd& Entering : Ends)
		{
			Append(LeftTurnGuides(Layouts, Entering, Ends, Mouths));
		}
	}
	return Result;
}

std::string FJunctionLines::GuideKind(const FLineKey& Key, const std::string& Kind, const FSegmentEnd& Entering,
									  const FSegmentEnd& Leaving, const std::vector<FSegmentEnd>& SideArms) const
{
	if (SideArms.empty())
	{
		return Kind;
	}
	// Facing out of the entering arm is facing against the through traffic, so its left edge is on the right of the
	// traffic going from entering to leaving.
	const FStreetPoint Through = Polyline::Unit(Network.ArmDirection(Leaving) - Network.ArmDirection(Entering));
	const FStreetPoint Right = Polyline::RightOf(Through);
	const bool bOnRight = Key.Side == ESide::Left;
	for (const FSegmentEnd& Arm : SideArms)
	{
		const bool bArmOnRight = Dot(Network.ArmDirection(Arm), Right) > 0;
		if (bArmOnRight != bOnRight)
		{
			continue;
		}
		if (Key.Kind == ELineKind::Cycle)
		{
			return "cycle_furt";
		}
		return "edge_guide";
	}
	return Kind;
}

std::vector<FMarking> FJunctionLines::ThroughGuides(const std::vector<FSegmentLayout>& Layouts,
													const FSegmentEnd& Entering, const FSegmentEnd& Leaving,
													const std::vector<FSegmentEnd>& Ends, const FMouths& Mouths) const
{
	const FLineOffsets EnteringLines = ArmLines(Layouts[Entering.Segment].Lines, Entering);
	const FPaintedLines EnteringPainted = ArmKeys(Layouts[Entering.Segment].Painted, Entering);
	const FLineOffsets LeavingLines = ArmLines(Layouts[Leaving.Segment].Lines, Leaving);
	const FPaintedLines LeavingPainted = ArmKeys(Layouts[Leaving.Segment].Painted, Leaving);
	std::vector<FSegmentEnd> SideArms;
	for (const FSegmentEnd& End : Ends)
	{
		if (End != Entering && End != Leaving && !IsMinor(End))
		{
			SideArms.push_back(End);
		}
	}
	std::vector<FMarking> Result;
	for (const auto& [Key, Kind] : EnteringPainted)
	{
		if (!LeavingPainted.Contains(Mirrored(Key)) || !IsContinuedKind(Key.Kind))
		{
			continue;
		}
		const std::string GuideKindName = GuideKind(Key, Kind, Entering, Leaving, SideArms);
		std::vector<FLineKey> Continued = {Key};
		if (GuideKindName == "cycle_furt")
		{
			Continued.push_back({ELineKind::CycleOuter, Key.Side});
		}
		for (const FLineKey& LineKey : Continued)
		{
			const double EnteringMouth = Mouths.OfLine(Entering, LineKey);
			const double LeavingMouth = Mouths.OfLine(Leaving, Mirrored(LineKey));
			const FStreetPoint Start = MouthPoint(Entering, *EnteringLines.Find(LineKey), EnteringMouth);
			const FStreetPoint End = MouthPoint(Leaving, *LeavingLines.Find(Mirrored(LineKey)), LeavingMouth);
			const FStreetPolyline Curve = Polyline::SmoothCurve(
				Start, -DirectionAtMouth(Entering, EnteringMouth), End, DirectionAtMouth(Leaving, LeavingMouth),
				Assumptions::LinePointSpacing);
			Result.push_back({GuideKindName, Curve});
		}
	}
	return Result;
}

std::vector<std::pair<FStreetPoint, FStreetPoint>> FJunctionLines::ThroughChords(
	const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& Entering, const FSegmentEnd& Leaving,
	const FMouths& Mouths) const
{
	const FLineOffsets EnteringLines = ArmLines(Layouts[Entering.Segment].Lines, Entering);
	const FLineOffsets LeavingLines = ArmLines(Layouts[Leaving.Segment].Lines, Leaving);
	const FPaintedLines LeavingPainted = ArmKeys(Layouts[Leaving.Segment].Painted, Leaving);
	std::vector<std::pair<FStreetPoint, FStreetPoint>> Chords;
	for (const auto& [Key, Kind] : ArmKeys(Layouts[Entering.Segment].Painted, Entering))
	{
		if (!IsCrossedKind(Key.Kind) || !LeavingPainted.Contains(Mirrored(Key)))
		{
			continue;
		}
		Chords.emplace_back(MouthPoint(Entering, *EnteringLines.Find(Key), Mouths.OfEnd(Entering)),
							MouthPoint(Leaving, *LeavingLines.Find(Mirrored(Key)), Mouths.OfEnd(Leaving)));
	}
	return Chords;
}

std::vector<std::pair<FStreetPoint, FStreetPoint>> FJunctionLines::EntryChords(
	const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& End, const FMouths& Mouths) const
{
	const FLineOffsets Lines = ArmLines(Layouts[End.Segment].Lines, End);
	const FStreetPoint Inward = -DirectionAtMouth(End, Mouths.OfEnd(End));
	const double Reach = 2.0 * Mouths.OfEnd(End) + SegmentOf(End).Section.Width();
	std::vector<std::pair<FStreetPoint, FStreetPoint>> Chords;
	for (const auto& [Key, Kind] : ArmKeys(Layouts[End.Segment].Painted, End))
	{
		if (!IsCrossedKind(Key.Kind))
		{
			continue;
		}
		const FStreetPoint Start = MouthPoint(End, *Lines.Find(Key), Mouths.OfEnd(End));
		Chords.emplace_back(Start, Start + Inward * Reach);
	}
	return Chords;
}

std::vector<FMarking> FJunctionLines::Crosses(const std::vector<FSegmentLayout>& Layouts,
											  const std::vector<FSegmentEnd>& Ends,
											  const std::vector<std::pair<FSegmentEnd, FSegmentEnd>>& Pairs,
											  const FMouths& Mouths) const
{
	std::vector<FChord> Chords;
	std::set<FSegmentEnd> Paired;
	for (size_t Road = 0; Road < Pairs.size(); ++Road)
	{
		const auto& [Entering, Leaving] = Pairs[Road];
		Paired.insert(Entering);
		Paired.insert(Leaving);
		for (const auto& [Start, End] : ThroughChords(Layouts, Entering, Leaving, Mouths))
		{
			Chords.push_back({static_cast<int>(Road), Start, End});
		}
	}
	for (size_t Index = 0; Index < Ends.size(); ++Index)
	{
		const FSegmentEnd& End = Ends[Index];
		if (Paired.count(End) > 0 || IsMinor(End))
		{
			continue;
		}
		for (const auto& [Start, Stop] : EntryChords(Layouts, End, Mouths))
		{
			Chords.push_back({static_cast<int>(Pairs.size() + Index), Start, Stop});
		}
	}
	std::vector<FMarking> Result;
	for (size_t Index = 0; Index < Chords.size(); ++Index)
	{
		for (size_t Next = Index + 1; Next < Chords.size(); ++Next)
		{
			const FChord& First = Chords[Index];
			const FChord& Second = Chords[Next];
			if (Second.Road == First.Road)
			{
				continue;
			}
			const std::optional<FStreetPoint> Crossing = SegmentIntersection(First.Start, First.End, Second.Start, Second.End);
			if (!Crossing.has_value()
				|| CrossingAngle(First.End - First.Start, Second.End - Second.Start) < CrossMinAngle)
			{
				continue;
			}
			for (FMarking& Bar : CrossBars(*Crossing, First.End - First.Start, Second.End - Second.Start))
			{
				Result.push_back(std::move(Bar));
			}
		}
	}
	return Result;
}

std::optional<FSegmentEnd> FJunctionLines::LeftTurnTarget(const FSegmentEnd& Entering,
														  const std::vector<FSegmentEnd>& Ends) const
{
	const FStreetPoint Arriving = -Network.ArmDirection(Entering);
	std::optional<FSegmentEnd> Best;
	double BestDistance = 0.0;
	for (const FSegmentEnd& End : Ends)
	{
		if (End == Entering || IsMinor(End))
		{
			continue;
		}
		const double Angle = TurnAngle(Arriving, Network.ArmDirection(End));
		if (!(LeftTurnAngleLow <= Angle && Angle <= LeftTurnAngleHigh))
		{
			continue;
		}
		if (!Best.has_value() || std::abs(Angle - 90.0) < BestDistance)
		{
			Best = End;
			BestDistance = std::abs(Angle - 90.0);
		}
	}
	return Best;
}

std::vector<FMarking> FJunctionLines::LeftTurnGuides(const std::vector<FSegmentLayout>& Layouts,
													 const FSegmentEnd& Entering, const std::vector<FSegmentEnd>& Ends,
													 const FMouths& Mouths) const
{
	const int LeftLanes = ExclusiveLeftLanes(SegmentOf(Entering).Way->Tags, Entering);
	if (LeftLanes == 0)
	{
		return {};
	}
	const std::optional<FSegmentEnd> Target = LeftTurnTarget(Entering, Ends);
	if (!Target.has_value())
	{
		return {};
	}
	const FLineOffsets EnteringLines = ArmLines(Layouts[Entering.Segment].Lines, Entering);
	const FLineOffsets TargetLines = ArmLines(Layouts[Target->Segment].Lines, *Target);
	const int Incoming = LanesOneWay(EnteringLines, ETravel::Backward);
	const int Outgoing = LanesOneWay(TargetLines, ETravel::Forward);
	std::vector<FMarking> Result;
	for (int Lane = 1; Lane <= LeftLanes; ++Lane)
	{
		const FLineKey StartKey = LineRightOfLane(ETravel::Backward, Incoming, Lane);
		const FLineKey EndKey = LineRightOfLane(ETravel::Forward, Outgoing, std::min(Lane, Outgoing));
		if (!EnteringLines.Contains(StartKey) || !TargetLines.Contains(EndKey))
		{
			continue;
		}
		const double EnteringMouth = Mouths.OfLine(Entering, StartKey);
		const double TargetMouth = Mouths.OfLine(*Target, EndKey);
		const FStreetPoint Start = MouthPoint(Entering, *EnteringLines.Find(StartKey), EnteringMouth);
		const FStreetPoint End = MouthPoint(*Target, *TargetLines.Find(EndKey), TargetMouth);
		Result.push_back({"guide", Polyline::SmoothCurve(Start, -DirectionAtMouth(Entering, EnteringMouth), End,
														 DirectionAtMouth(*Target, TargetMouth),
														 Assumptions::LinePointSpacing)});
	}
	return Result;
}
}
