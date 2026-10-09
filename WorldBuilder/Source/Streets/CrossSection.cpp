#include "CrossSection.h"

#include <algorithm>

#include "Assumptions.h"
#include "OsmTags.h"

namespace WorldBuilder
{
double FCrossSection::Width() const
{
	double Total = 0.0;
	for (const FStrip& Strip : Strips)
	{
		Total += Strip.Width;
	}
	return Total;
}

std::vector<FStrip> FCrossSection::TravelLanes() const
{
	std::vector<FStrip> Lanes;
	for (const FStrip& Strip : Strips)
	{
		if (Strip.Kind == EStripKind::TravelLane)
		{
			Lanes.push_back(Strip);
		}
	}
	return Lanes;
}

int FCrossSection::LaneCount() const
{
	int Count = 0;
	for (const FStrip& Strip : Strips)
	{
		if (Strip.Kind == EStripKind::TravelLane)
		{
			++Count;
		}
	}
	return Count;
}

FCrossSection FCrossSection::Mirrored() const
{
	FCrossSection Result;
	Result.bMarked = bMarked;
	Result.WidthSource = WidthSource;
	for (auto Strip = Strips.rbegin(); Strip != Strips.rend(); ++Strip)
	{
		FStrip Swapped = *Strip;
		if (Strip->Travel == ETravel::Forward)
		{
			Swapped.Travel = ETravel::Backward;
		}
		else if (Strip->Travel == ETravel::Backward)
		{
			Swapped.Travel = ETravel::Forward;
		}
		Result.Strips.push_back(Swapped);
	}
	return Result;
}

std::vector<FStripEdges> FCrossSection::StripEdges() const
{
	std::vector<FStripEdges> Edges;
	Edges.reserve(Strips.size());
	double Left = -Width() / 2;
	for (const FStrip& Strip : Strips)
	{
		Edges.push_back({&Strip, Left, Left + Strip.Width});
		Left += Strip.Width;
	}
	return Edges;
}

std::vector<double> FCrossSection::LaneDividers() const
{
	std::vector<double> Dividers;
	const std::vector<FStripEdges> Edges = StripEdges();
	for (size_t Index = 0; Index + 1 < Edges.size(); ++Index)
	{
		const bool bBothLanes = Edges[Index].Strip->Kind == EStripKind::TravelLane
			&& Edges[Index + 1].Strip->Kind == EStripKind::TravelLane;
		if (bBothLanes)
		{
			Dividers.push_back(Edges[Index].Right);
		}
	}
	return Dividers;
}

std::optional<double> FCrossSection::SideStripCentre(EStripKind Kind, bool bRightSide) const
{
	for (const FStripEdges& Edge : StripEdges())
	{
		const double Centre = (Edge.Left + Edge.Right) / 2;
		if (Edge.Strip->Kind != Kind)
		{
			continue;
		}
		if (bRightSide && Centre > 0)
		{
			return Centre;
		}
		if (!bRightSide && Centre < 0)
		{
			return -Centre;
		}
	}
	return std::nullopt;
}

double FCrossSection::KerbLaneCentre(ETravel Travel) const
{
	std::vector<double> Centres;
	for (const FStripEdges& Edge : StripEdges())
	{
		if (Edge.Strip->Kind == EStripKind::TravelLane && Edge.Strip->Travel == Travel)
		{
			Centres.push_back((Edge.Left + Edge.Right) / 2);
		}
	}
	if (Centres.empty())
	{
		return 0.0;
	}
	if (Travel == ETravel::Forward)
	{
		return *std::max_element(Centres.begin(), Centres.end());
	}
	return -*std::min_element(Centres.begin(), Centres.end());
}

namespace
{
/** The urban or the rural value of an (urban, rural) pair. */
double ByContext(const Assumptions::FUrbanRural& UrbanAndRural, const FRoadContext& Context)
{
	if (Context.bUrban)
	{
		return UrbanAndRural.Urban;
	}
	return UrbanAndRural.Rural;
}

/** True when lane lines are painted: lane_markings says so, two or more lanes are tagged, or the class is. */
bool IsMarked(const FTags& Tags)
{
	const std::optional<bool> Painted = OsmTags::LaneMarkings(Tags);
	if (Painted.has_value())
	{
		return *Painted;
	}
	const std::optional<int> Tagged = OsmTags::TaggedLaneCount(Tags);
	if (Tagged.has_value() && *Tagged >= 2)
	{
		return true;
	}
	return Assumptions::MarkedByDefault.count(OsmTags::BaseClass(Tags)) > 0;
}

/** The travel direction of each of Count lanes, from left to right. */
std::vector<ETravel> LaneDirections(const FTags& Tags, int Count)
{
	if (OsmTags::IsReversedOneway(Tags))
	{
		return std::vector<ETravel>(Count, ETravel::Backward);
	}
	if (OsmTags::IsOneway(Tags))
	{
		return std::vector<ETravel>(Count, ETravel::Forward);
	}
	if (Count == 1)
	{
		return {ETravel::Both};
	}
	int Backward = Count / 2;
	const std::optional<std::pair<int, int>> ByDirection = OsmTags::TaggedLanesByDirection(Tags);
	if (ByDirection.has_value() && ByDirection->first + ByDirection->second == Count)
	{
		Backward = ByDirection->second;
	}
	std::vector<ETravel> Directions(Backward, ETravel::Backward);
	Directions.insert(Directions.end(), Count - Backward, ETravel::Forward);
	return Directions;
}

/** The width of the travel lanes together on a carriageway without lane lines. */
double UnmarkedWidth(const FTags& Tags, const FRoadContext& Context)
{
	const std::string RoadClass = OsmTags::BaseClass(Tags);
	const bool bOneway = OsmTags::IsOneway(Tags);
	if (OsmTags::IsBusRoad(Tags))
	{
		return Assumptions::BusRoadLaneWidth * LaneCount(Tags);
	}
	const auto Service = Assumptions::ServiceWidth.find(OsmTags::ValueOrEmpty(Tags, "service"));
	if (RoadClass == "service" && Service != Assumptions::ServiceWidth.end())
	{
		return bOneway ? Service->second.OneWay : Service->second.TwoWay;
	}
	if (bOneway)
	{
		const auto Found = Assumptions::OneWayWidth.find(RoadClass);
		return Found != Assumptions::OneWayWidth.end() ? Found->second : Assumptions::OneWayWidthOther;
	}
	const std::optional<double> Speed = OsmTags::SpeedLimit(Tags);
	if (RoadClass == "residential" && Speed.has_value() && *Speed >= Assumptions::CollectorStreetSpeed)
	{
		return Assumptions::CollectorStreetWidth;
	}
	const auto Found = Assumptions::UnmarkedTwoWayWidth.find(RoadClass);
	if (Found != Assumptions::UnmarkedTwoWayWidth.end())
	{
		return ByContext(Found->second, Context);
	}
	return ByContext(Assumptions::UnmarkedTwoWayWidthOther, Context);
}

/** The travel lane strips from left to right: marked lanes of the class's lane width, or an unmarked carriageway of
 * the class's width split between the directions. */
std::vector<FStrip> TravelLaneStrips(const FTags& Tags, const FRoadContext& Context, bool bMarked)
{
	int Count = LaneCount(Tags);
	if (OsmTags::IsLink(Tags) && OsmTags::IsOneway(Tags) && Count == 1)
	{
		return {{EStripKind::TravelLane, Assumptions::SingleLaneLinkWidth, LaneDirections(Tags, 1)[0]}};
	}
	std::vector<FStrip> Lanes;
	if (bMarked)
	{
		const auto Found = Assumptions::MarkedLaneWidth.find(OsmTags::BaseClass(Tags));
		const Assumptions::FUrbanRural Widths = Found != Assumptions::MarkedLaneWidth.end()
			? Found->second
			: Assumptions::MarkedLaneWidthOther;
		const double LaneWidth = ByContext(Widths, Context);
		for (ETravel Travel : LaneDirections(Tags, Count))
		{
			Lanes.push_back({EStripKind::TravelLane, LaneWidth, Travel});
		}
		return Lanes;
	}
	const double Width = UnmarkedWidth(Tags, Context);
	if (!OsmTags::IsOneway(Tags) && Width < Assumptions::SharedLaneMaxWidth)
	{
		Count = 1;
	}
	const std::vector<ETravel> Directions = LaneDirections(Tags, Count);
	for (ETravel Travel : Directions)
	{
		Lanes.push_back({EStripKind::TravelLane, Width / static_cast<double>(Directions.size()), Travel});
	}
	return Lanes;
}

/**
 * The bus and cycle lanes at one side, from the travel lanes outward: the cycle lane runs at the kerb. The lane at
 * the left carries traffic against the node order, the one at the right along it (right-hand traffic); on a one-way
 * street one of them is a contraflow lane (cycleway=opposite_lane).
 */
std::vector<FStrip> SideStrips(const FTags& Tags, bool bRightSide)
{
	std::vector<FStrip> Strips;
	const ETravel Travel = bRightSide ? ETravel::Forward : ETravel::Backward;
	if (OsmTags::HasBusLane(Tags, bRightSide))
	{
		Strips.push_back({EStripKind::BusLane, Assumptions::BusLaneWidth, Travel});
	}
	const OsmTags::FSideValues Cycleways = OsmTags::CyclewaySides(Tags);
	const std::optional<std::string>& Cycleway = Cycleways.OfSide(bRightSide);
	if (Cycleway.has_value() && OsmTags::IsCycleLaneValue(*Cycleway))
	{
		double Width = Assumptions::CycleLaneWidth;
		if (OsmTags::CycleLaneIsAdvisory(Tags, bRightSide))
		{
			Width = Assumptions::AdvisoryCycleLaneWidth;
		}
		Strips.push_back({EStripKind::CycleLane, Width, Travel});
	}
	return Strips;
}

/** The width= tag when it is a plausible carriageway width, else nothing. */
std::optional<double> TaggedWidth(const FTags& Tags)
{
	const std::optional<double> Width = OsmTags::Number(FindTag(Tags, "width"));
	if (!Width.has_value() || !(Assumptions::WidthTagRangeLow <= *Width && *Width <= Assumptions::WidthTagRangeHigh))
	{
		return std::nullopt;
	}
	return Width;
}

/**
 * The cross-section with its travel lanes widened or narrowed to match a trusted width= tag. On classes whose width
 * tags measure less than the kerbs (Assumptions::WidthTagTrustedClasses) the tag can only widen the road, and a tag
 * far below the assumption is taken to measure one lane and ignored.
 */
FCrossSection FittedToWidthTag(const FCrossSection& Section, const FTags& Tags)
{
	const std::optional<double> Target = TaggedWidth(Tags);
	if (!Target.has_value())
	{
		return Section;
	}
	const double Difference = *Target - Section.Width();
	const bool bTrusted = Assumptions::WidthTagTrustedClasses.count(OsmTags::Highway(Tags)) > 0;
	if (Difference < 0 && !bTrusted)
	{
		return Section;
	}
	if (Difference < -Assumptions::WidthTagLaneOnlyGap)
	{
		return Section;
	}
	const double ChangePerLane = Difference / Section.LaneCount();
	FCrossSection Fitted;
	Fitted.bMarked = Section.bMarked;
	Fitted.WidthSource = EWidthSource::WidthTag;
	for (FStrip Strip : Section.Strips)
	{
		if (Strip.Kind == EStripKind::TravelLane)
		{
			Strip.Width = std::max(Strip.Width + ChangePerLane, Assumptions::MinTravelLaneWidth);
		}
		Fitted.Strips.push_back(Strip);
	}
	return Fitted;
}
}

int LaneCount(const FTags& Tags)
{
	const bool bOneway = OsmTags::IsOneway(Tags);
	const std::optional<int> Tagged = OsmTags::TaggedLaneCount(Tags);
	if (!Tagged.has_value())
	{
		return bOneway ? Assumptions::DefaultLanesOneWay : Assumptions::DefaultLanesTwoWay;
	}
	const int MotorLanes = std::max(*Tagged - OsmTags::NonMotorLanesCounted(Tags), 1);
	const auto Found = Assumptions::MaxLanesPerDirection.find(OsmTags::BaseClass(Tags));
	const int PerDirection = Found != Assumptions::MaxLanesPerDirection.end()
		? Found->second
		: Assumptions::MaxLanesPerDirectionOther;
	if (bOneway)
	{
		return std::min(MotorLanes, PerDirection);
	}
	return std::min(MotorLanes, 2 * PerDirection);
}

FCrossSection MakeCrossSection(const FTags& Tags, const FRoadContext& Context)
{
	FCrossSection Section;
	Section.bMarked = IsMarked(Tags);
	const FStrip Margin = {EStripKind::Margin, ByContext(Assumptions::MarginWidth, Context), ETravel::None};
	const std::vector<FStrip> LeftSide = SideStrips(Tags, false);
	const std::vector<FStrip> RightSide = SideStrips(Tags, true);
	Section.Strips.push_back(Margin);
	Section.Strips.insert(Section.Strips.end(), LeftSide.rbegin(), LeftSide.rend());
	for (const FStrip& Lane : TravelLaneStrips(Tags, Context, Section.bMarked))
	{
		Section.Strips.push_back(Lane);
	}
	Section.Strips.insert(Section.Strips.end(), RightSide.begin(), RightSide.end());
	Section.Strips.push_back(Margin);
	return FittedToWidthTag(Section, Tags);
}
}
