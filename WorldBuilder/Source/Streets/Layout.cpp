#include "Layout.h"

#include <algorithm>
#include <cmath>

#include "Assumptions.h"
#include "OsmTags.h"

namespace WorldBuilder
{
double FMouths::OfEnd(const FSegmentEnd& End) const
{
	const auto Found = ByEnd.find(End);
	if (Found == ByEnd.end())
	{
		return 0.0;
	}
	return Found->second;
}

double FMouths::OfLine(const FSegmentEnd& End, const FLineKey& Key) const
{
	const auto Found = ByLine.find({End, Key});
	if (Found == ByLine.end())
	{
		return OfEnd(End);
	}
	return Found->second;
}

namespace
{
/**
 * How much of the node's offset is left at a fraction of the taper from the node: 1 at the node, 0 at the end of the
 * taper, with no kink at either end (an S-curve, as RAA's two parabolas).
 */
double TaperBlend(double Fraction)
{
	const double Remaining = 1.0 - std::min(std::max(Fraction, 0.0), 1.0);
	return Remaining * Remaining * (3.0 - 2.0 * Remaining);
}
}

std::vector<double> FSegmentLayout::Offsets(const FLineKey& Key, const std::vector<double>& Along) const
{
	const double Own = *Lines.Find(Key);
	std::vector<double> Result(Along.size(), Own);
	for (bool bAtStart : {true, false})
	{
		const FEndShape& Shape = EndShapeOf(*this, bAtStart);
		const double* NodeOffset = Shape.NodeOffsets.Find(Key);
		if (NodeOffset == nullptr || Shape.Taper <= 0)
		{
			continue;
		}
		for (size_t Index = 0; Index < Along.size(); ++Index)
		{
			const double DistanceFromNode = bAtStart ? Along[Index] : Segment->Length - Along[Index];
			Result[Index] += (*NodeOffset - Own) * TaperBlend(DistanceFromNode / Shape.Taper);
		}
	}
	return Result;
}

std::pair<double, double> FSegmentLayout::LineRange(const FLineKey& Key) const
{
	return {Start.Cuts.ValueOr(Key, 0.0), Segment->Length - End.Cuts.ValueOr(Key, 0.0)};
}

std::optional<FStreetPolyline> FSegmentLayout::LineXy(const FLineKey& Key) const
{
	const auto [StartS, EndS] = LineRange(Key);
	return LineBetween(Key, StartS, EndS);
}

std::optional<FStreetPolyline> FSegmentLayout::LineBetween(const FLineKey& Key, double StartS, double EndS,
														   double Shift) const
{
	if (EndS - StartS < 0.5)
	{
		return std::nullopt;
	}
	const FStreetPolyline Centre = Polyline::Resample(Polyline::CutPolyline(Segment->Xy, StartS, EndS),
													 Assumptions::LinePointSpacing);
	if (Centre.size() < 2)
	{
		return std::nullopt;
	}
	std::vector<double> Along = Polyline::Arclength(Centre);
	for (double& Distance : Along)
	{
		Distance += StartS;
	}
	std::vector<double> Distances = Offsets(Key, Along);
	for (double& Distance : Distances)
	{
		Distance += Shift;
	}
	return Polyline::OffsetPolyline(Centre, Distances);
}

double FSegmentLayout::WidthAtEnd(bool bAtStart) const
{
	const FEndShape& Shape = EndShapeOf(*this, bAtStart);
	const FLineKey LeftKerb = {ELineKind::Kerb, ESide::Left};
	const FLineKey RightKerb = {ELineKind::Kerb, ESide::Right};
	const double Left = Shape.NodeOffsets.ValueOr(LeftKerb, *Lines.Find(LeftKerb));
	const double Right = Shape.NodeOffsets.ValueOr(RightKerb, *Lines.Find(RightKerb));
	return Right - Left;
}

FEndShape& EndShapeOf(FSegmentLayout& Layout, bool bAtStart)
{
	if (bAtStart)
	{
		return Layout.Start;
	}
	return Layout.End;
}

const FEndShape& EndShapeOf(const FSegmentLayout& Layout, bool bAtStart)
{
	if (bAtStart)
	{
		return Layout.Start;
	}
	return Layout.End;
}

FLineOffsets ArmLines(const FLineOffsets& Lines, const FSegmentEnd& End)
{
	if (End.bAtStart)
	{
		return Lines;
	}
	return MirroredLines(Lines);
}

FPaintedLines ArmKeys(const FPaintedLines& Values, const FSegmentEnd& End)
{
	if (End.bAtStart)
	{
		return Values;
	}
	FPaintedLines Result;
	for (const auto& [Key, Value] : Values)
	{
		Result.Set(Mirrored(Key), Value);
	}
	return Result;
}

std::pair<FLineKey, double> ToSegmentFrame(const FLineKey& Key, double Offset, const FSegmentEnd& End)
{
	if (End.bAtStart)
	{
		return {Key, Offset};
	}
	return {Mirrored(Key), -Offset};
}

namespace
{
/**
 * Where a carriageway's line (facing out of the node) starts on the two-way road at the node, or nothing when it has
 * no counterpart there: the inner edge and kerb on the centre line, the others on their mirrored line.
 */
std::optional<double> SplitMeetingOffset(const FLineKey& Key, ESide InnerSide, const FLineOffsets& TwoWayLines)
{
	if ((Key.Kind == ELineKind::Edge || Key.Kind == ELineKind::Kerb) && Key.Side == InnerSide)
	{
		return -TwoWayLines.ValueOr({ELineKind::Centre}, 0.0);
	}
	const double* Counterpart = TwoWayLines.Find(Mirrored(Key));
	if (Counterpart == nullptr)
	{
		return std::nullopt;
	}
	return -*Counterpart;
}

/**
 * Each carriageway of a dual carriageway split starts on its half of the two-way road and moves out to its own line
 * over the gore: its lanes and outer edge from the two-way road's lanes and edge on its side, its inner edge and kerb
 * from the two-way road's centre line.
 */
void ShapeSplit(std::vector<FSegmentLayout>& Layouts, const FSplit& Split)
{
	const FLineOffsets TwoWayLines = ArmLines(Layouts[Split.TwoWay.Segment].Lines, Split.TwoWay);
	// Facing out of the node toward the carriageways, traffic coming in keeps to the left, so the incoming
	// carriageway's inner side is its right and the outgoing one's its left.
	const std::pair<FSegmentEnd, ESide> Carriageways[] = {{Split.Incoming, ESide::Right}, {Split.Outgoing, ESide::Left}};
	for (const auto& [OneWay, InnerSide] : Carriageways)
	{
		FSegmentLayout& Layout = Layouts[OneWay.Segment];
		FEndShape& Shape = EndShapeOf(Layout, OneWay.bAtStart);
		Shape.Taper = std::min(Split.GoreLength, Layout.Segment->Length * Assumptions::TaperMaxSegmentShare);
		for (const auto& [Key, Offset] : ArmLines(Layout.Lines, OneWay))
		{
			const std::optional<double> Meeting = SplitMeetingOffset(Key, InnerSide, TwoWayLines);
			const FLineKey SegmentKey = ToSegmentFrame(Key, 0.0, OneWay).first;
			if (!Meeting.has_value())
			{
				Shape.Cuts.Set(SegmentKey, Shape.Taper);
				continue;
			}
			Shape.NodeOffsets.Set(SegmentKey, ToSegmentFrame(Key, *Meeting, OneWay).second);
		}
	}
}

/**
 * Painted lines stop at the junction's mouth (Mouths: the arm's distance under the end, and some lines' own distance
 * under the end and the key facing out of the node).
 */
void CutAtMouth(FSegmentLayout& Layout, const FSegmentEnd& End, const FMouths& Mouths)
{
	FEndShape& Shape = EndShapeOf(Layout, End.bAtStart);
	const double Limit = Layout.Segment->Length * Assumptions::TaperMaxSegmentShare;
	for (const auto& [Key, Kind] : Layout.Painted)
	{
		const FLineKey ArmKey = ToSegmentFrame(Key, 0.0, End).first;  // mirroring is its own inverse
		Shape.Cuts.Set(Key, std::min(Mouths.OfLine(End, ArmKey), Limit));
	}
}

/** The width (rounded to centimetres) and lane count of a segment, which decide who owns a taper. */
std::pair<double, int> TaperOwnerSize(const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& End)
{
	const FCrossSection& Section = Layouts[End.Segment].Segment->Section;
	return {std::nearbyint(Section.Width() * 100.0) / 100.0, Section.LaneCount()};
}

/** (owner, other): the wider segment moves its lines; on equal widths the one with more lanes, then the first. */
std::pair<FSegmentEnd, FSegmentEnd> TaperOwner(const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& First,
											   const FSegmentEnd& Second)
{
	if (TaperOwnerSize(Layouts, Second) > TaperOwnerSize(Layouts, First))
	{
		return {Second, First};
	}
	return {First, Second};
}

/** The taper length on a segment for a sideways shift, at most TaperMaxSegmentShare of the segment. */
double TaperFor(const FSegmentLayout& Layout, double Shift, bool bUrban)
{
	std::optional<double> Speed = OsmTags::SpeedLimit(Layout.Segment->Way->Tags);
	if (!Speed.has_value())
	{
		Speed = bUrban ? Assumptions::DefaultSpeedUrban : Assumptions::DefaultSpeedRural;
	}
	const double Length = TaperLength(Shift, *Speed, bUrban);
	return std::min(Length, Layout.Segment->Length * Assumptions::TaperMaxSegmentShare);
}

/** The taper between two segments meeting at a continuation node, on the wider one. */
void ShapeContinuation(std::vector<FSegmentLayout>& Layouts, const std::vector<bool>& Urban, const FSegmentEnd& First,
					   const FSegmentEnd& Second)
{
	const auto [Owner, Other] = TaperOwner(Layouts, First, Second);
	FSegmentLayout& OwnerLayout = Layouts[Owner.Segment];
	FSegmentLayout& OtherLayout = Layouts[Other.Segment];
	const FLineOffsets OwnerLines = ArmLines(OwnerLayout.Lines, Owner);
	const FLineOffsets OtherLines = ArmLines(OtherLayout.Lines, Other);
	FLineOffsets Meeting;
	for (const auto& [Key, Offset] : OwnerLines)
	{
		const double* Counterpart = OtherLines.Find(Mirrored(Key));
		if (Counterpart != nullptr)
		{
			Meeting.Set(Key, -*Counterpart);
		}
	}
	std::vector<FLineKey> UnmatchedOwner;
	for (const auto& [Key, Offset] : OwnerLines)
	{
		if (!Meeting.Contains(Key))
		{
			UnmatchedOwner.push_back(Key);
		}
	}
	std::vector<FLineKey> UnmatchedOther;
	for (const auto& [Key, Offset] : OtherLines)
	{
		if (!OwnerLines.Contains(Mirrored(Key)))
		{
			UnmatchedOther.push_back(Key);
		}
	}
	double Shift = 0.0;
	for (const auto& [Key, Offset] : Meeting)
	{
		Shift = std::max(Shift, std::abs(*OwnerLines.Find(Key) - Offset));
	}
	if (Shift < 0.05 && UnmatchedOwner.empty() && UnmatchedOther.empty())
	{
		return;
	}
	const double Taper = TaperFor(OwnerLayout, Shift, Urban[Owner.Segment]);
	FEndShape& OwnerShape = EndShapeOf(OwnerLayout, Owner.bAtStart);
	OwnerShape.Taper = Taper;
	for (const auto& [Key, Offset] : Meeting)
	{
		const auto [SegmentKey, SegmentOffset] = ToSegmentFrame(Key, Offset, Owner);
		OwnerShape.NodeOffsets.Set(SegmentKey, SegmentOffset);
	}
	for (const FLineKey& Key : UnmatchedOwner)
	{
		OwnerShape.Cuts.Set(ToSegmentFrame(Key, 0.0, Owner).first, Taper);
	}
	FEndShape& OtherShape = EndShapeOf(OtherLayout, Other.bAtStart);
	const double OtherLimit = OtherLayout.Segment->Length * Assumptions::TaperMaxSegmentShare;
	for (const FLineKey& Key : UnmatchedOther)
	{
		OtherShape.Cuts.Set(ToSegmentFrame(Key, 0.0, Other).first, std::min(Taper, OtherLimit));
	}
}
}

std::vector<FSegmentLayout> BuildLayouts(const FSegmentNetwork& Network, const std::vector<FPaintedLines>& Painted,
										 const std::vector<bool>& Urban, const FMouths& Mouths)
{
	std::vector<FSegmentLayout> Layouts;
	Layouts.reserve(Network.Segments.size());
	for (const FSegment& Segment : Network.Segments)
	{
		FSegmentLayout Layout;
		Layout.Segment = &Segment;
		Layout.Lines = SectionLines(Segment.Section);
		Layout.Painted = Painted[Segment.Id];
		Layouts.push_back(std::move(Layout));
	}
	for (int64_t Node : Network.NodeOrder)
	{
		const ENodeKind Kind = Network.NodeKind(Node);
		const std::vector<FSegmentEnd>& Ends = Network.EndsAt.at(Node);
		if (Kind == ENodeKind::Continuation)
		{
			ShapeContinuation(Layouts, Urban, Ends[0], Ends[1]);
		}
		if (Kind != ENodeKind::Junction)
		{
			continue;
		}
		const std::optional<FSplit> Split = FindSplit(Network, Node);
		if (Split.has_value())
		{
			ShapeSplit(Layouts, *Split);
			continue;
		}
		for (const FSegmentEnd& End : Ends)
		{
			CutAtMouth(Layouts[End.Segment], End, Mouths);
		}
	}
	return Layouts;
}
}
