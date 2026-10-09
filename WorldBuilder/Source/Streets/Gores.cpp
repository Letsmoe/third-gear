#include "Gores.h"

namespace WorldBuilder
{
namespace
{
/** A line of the arm (key facing out of the node) over its first metres, as points from the node outward. */
std::optional<FStreetPolyline> FromNode(const FSegmentLayout& Layout, const FSegmentEnd& End, const FLineKey& ArmKey,
										double Length)
{
	const FLineKey Key = ToSegmentFrame(ArmKey, 0.0, End).first;
	const double Total = Layout.Segment->Length;
	if (End.bAtStart)
	{
		return Layout.LineBetween(Key, 0.0, Length);
	}
	std::optional<FStreetPolyline> Line = Layout.LineBetween(Key, Total - Length, Total);
	if (!Line.has_value())
	{
		return std::nullopt;
	}
	return Polyline::Reversed(*Line);
}

/** The polygon along the first line, then back along the second. */
FStreetPolyline Between(const FStreetPolyline& First, const FStreetPolyline& Second)
{
	FStreetPolyline Polygon = First;
	Polygon.insert(Polygon.end(), Second.rbegin(), Second.rend());
	return Polygon;
}

/**
 * The gore between the incoming carriageway's right side and the outgoing one's left (facing out of the node), as
 * long as the shorter of their two openings.
 */
std::optional<FGore> MakeGore(const std::vector<FSegmentLayout>& Layouts, const FSegmentEnd& Incoming,
							  const FSegmentEnd& Outgoing)
{
	const double Length = std::min(EndShapeOf(Layouts[Incoming.Segment], Incoming.bAtStart).Taper,
								   EndShapeOf(Layouts[Outgoing.Segment], Outgoing.bAtStart).Taper);
	if (Length < 1.0)
	{
		return std::nullopt;
	}
	const std::pair<FSegmentEnd, ESide> Sides[] = {{Incoming, ESide::Right}, {Outgoing, ESide::Left}};
	std::vector<FStreetPolyline> Kerbs;
	std::vector<FStreetPolyline> Edges;
	for (const auto& [End, Side] : Sides)
	{
		const std::optional<FStreetPolyline> Kerb =
			FromNode(Layouts[End.Segment], End, {ELineKind::Kerb, Side}, Length);
		const std::optional<FStreetPolyline> Edge =
			FromNode(Layouts[End.Segment], End, {ELineKind::Edge, Side}, Length);
		if (!Kerb.has_value() || !Edge.has_value())
		{
			return std::nullopt;
		}
		Kerbs.push_back(*Kerb);
		Edges.push_back(*Edge);
	}
	FGore Gore;
	for (size_t Index = 0; Index < 2; ++Index)
	{
		const auto& [End, Side] = Sides[Index];
		const FLineKey Key = ToSegmentFrame({ELineKind::Edge, Side}, 0.0, End).first;
		if (!Layouts[End.Segment].Painted.Contains(Key))
		{
			Gore.Outline.push_back(Edges[Index]);
		}
	}
	Gore.Paved = Between(Kerbs[0], Kerbs[1]);
	Gore.Hatched = Between(Edges[0], Edges[1]);
	return Gore;
}
}

std::vector<FGore> SplitGores(const FSegmentNetwork& Network, const std::vector<FSegmentLayout>& Layouts)
{
	std::vector<FGore> Gores;
	for (int64_t Node : Network.NodeOrder)
	{
		if (Network.NodeKind(Node) != ENodeKind::Junction)
		{
			continue;
		}
		const std::optional<FSplit> Split = FindSplit(Network, Node);
		if (!Split.has_value())
		{
			continue;
		}
		std::optional<FGore> Gore = MakeGore(Layouts, Split->Incoming, Split->Outgoing);
		if (Gore.has_value())
		{
			Gores.push_back(std::move(*Gore));
		}
	}
	return Gores;
}
}
