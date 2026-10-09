#include "RoadGraph.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "OsmTags.h"

namespace WorldBuilder
{
namespace
{
constexpr double SegmentCellSize = 50.0;
constexpr double DirectionReach = 3.0;

int64_t CellKey(int64_t Column, int64_t Row)
{
	return Row * 1000003 + Column;
}

int64_t CellIndex(double Coordinate)
{
	return static_cast<int64_t>(std::floor(Coordinate / SegmentCellSize));
}

bool IsRoundabout(const FTags& Tags)
{
	return OsmTags::TagEquals(Tags, "junction", "roundabout") || OsmTags::TagEquals(Tags, "junction", "circular");
}

/** Python's str.strip().lower() of a tag value. */
std::string StrippedLower(const std::string& Text)
{
	size_t Begin = 0;
	size_t End = Text.size();
	while (Begin < End && std::isspace(static_cast<unsigned char>(Text[Begin])))
	{
		++Begin;
	}
	while (End > Begin && std::isspace(static_cast<unsigned char>(Text[End - 1])))
	{
		--End;
	}
	std::string Result = Text.substr(Begin, End - Begin);
	for (char& Character : Result)
	{
		Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
	}
	return Result;
}

/** The number written after Prefix at Position, or nothing when digits don't follow. */
std::optional<double> DigitsAfter(const std::string& Text, size_t Position, const char* Prefix)
{
	const size_t PrefixLength = std::char_traits<char>::length(Prefix);
	if (Text.compare(Position, PrefixLength, Prefix) != 0)
	{
		return std::nullopt;
	}
	size_t End = Position + PrefixLength;
	while (End < Text.size() && std::isdigit(static_cast<unsigned char>(Text[End])))
	{
		++End;
	}
	if (End == Position + PrefixLength)
	{
		return std::nullopt;
	}
	return std::stod(Text.substr(Position + PrefixLength, End - Position - PrefixLength));
}

/** The speed a zone tag names ("zone30", "DE:30"), found like re.search(r"zone(\d+)|DE:(\d+)"). */
std::optional<double> ZoneSpeed(const std::string& Zone)
{
	for (size_t Position = 0; Position < Zone.size(); ++Position)
	{
		const std::optional<double> Lowercase = DigitsAfter(Zone, Position, "zone");
		if (Lowercase.has_value())
		{
			return Lowercase;
		}
		const std::optional<double> Country = DigitsAfter(Zone, Position, "DE:");
		if (Country.has_value())
		{
			return Country;
		}
	}
	return std::nullopt;
}

/** The distance along a line to the point nearest to Point, as GEOS's LengthIndexedLine.project finds it. */
double ProjectOnLine(const FStreetPolyline& Line, const std::vector<double>& Along, const FStreetPoint& Point)
{
	double BestDistance = std::numeric_limits<double>::max();
	double BestAlong = 0.0;
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		const double Distance = Polyline::DistanceToSegment(Line[Index], Line[Index + 1], Point);
		if (Distance >= BestDistance)
		{
			continue;
		}
		BestDistance = Distance;
		const FStreetPoint Edge = Line[Index + 1] - Line[Index];
		const double Squared = Dot(Edge, Edge);
		double Fraction = 0.0;
		if (Squared > 0.0)
		{
			Fraction = std::min(std::max(Dot(Point - Line[Index], Edge) / Squared, 0.0), 1.0);
		}
		BestAlong = Along[Index] + Fraction * (Along[Index + 1] - Along[Index]);
	}
	return BestAlong;
}
}

double ParseSpeed(const FTags& Tags, bool bUrban)
{
	const std::string Raw = StrippedLower(OsmTags::ValueOrEmpty(Tags, "maxspeed"));
	static const std::vector<std::pair<std::string, double>> Named = {
		{"de:urban", 50}, {"de:rural", 100}, {"de:living_street", 7}, {"walk", 7}, {"de:walk", 7}, {"none", 0},
		{"de:motorway", 0}};
	for (const auto& [Name, Value] : Named)
	{
		if (Raw == Name)
		{
			return Value;
		}
	}
	if (!Raw.empty())
	{
		std::string First = OsmTags::SplitText(Raw, ';')[0];
		for (size_t Position = First.find("mph"); Position != std::string::npos; Position = First.find("mph"))
		{
			First.erase(Position, 3);
		}
		const std::optional<double> Value = OsmTags::Number(&First);
		if (Value.has_value() && 1 <= *Value && *Value <= 130)
		{
			return *Value;
		}
	}
	const std::string Zone = OsmTags::ValueOrEmpty(Tags, "maxspeed:type") + OsmTags::ValueOrEmpty(Tags, "zone:maxspeed");
	const std::optional<double> ZoneValue = ZoneSpeed(Zone);
	if (ZoneValue.has_value())
	{
		return *ZoneValue;
	}
	const std::string Highway = OsmTags::ValueOrEmpty(Tags, "highway");
	if (Highway == "living_street")
	{
		return 7.0;
	}
	if (Highway == "motorway" || Highway == "motorway_link")
	{
		return 0.0;
	}
	if (bUrban)
	{
		return Highway == "service" ? 30.0 : 50.0;
	}
	if (Highway == "residential" || Highway == "service" || Highway == "living_street")
	{
		return Highway == "residential" ? 50.0 : 30.0;
	}
	return 100.0;
}

FRoadGraph::FRoadGraph(const std::vector<FStreetWay>& InWays, const std::unordered_map<int64_t, double>& Widths,
					   const FBuildingIndex& Buildings)
	: Ways(InWays), Degree(NodeDegrees(InWays))
{
	for (size_t WayIndex = 0; WayIndex < Ways.size(); ++WayIndex)
	{
		const FStreetWay& Way = Ways[WayIndex];
		WayWidth.push_back(Widths.at(Way.Id));
		WayAlong.push_back(Polyline::Arclength(Way.Points));
		const bool bUrban = IsUrban(Way, Buildings);
		WayUrban.push_back(bUrban);
		WaySpeed.push_back(ParseSpeed(Way.Tags, bUrban));
		for (size_t Index = 0; Index < Way.NodeIds.size(); ++Index)
		{
			AtNode[Way.NodeIds[Index]].push_back({static_cast<int>(WayIndex), static_cast<int>(Index)});
			NodePoint[Way.NodeIds[Index]] = Way.Points[Index];
		}
		NodeSCache.emplace_back(Way.NodeIds.size(), std::numeric_limits<double>::quiet_NaN());
	}
	BuildSegmentCells();
}

void FRoadGraph::BuildSegmentCells()
{
	for (size_t WayIndex = 0; WayIndex < Ways.size(); ++WayIndex)
	{
		const FStreetPolyline& Line = Ways[WayIndex].Points;
		for (size_t Segment = 0; Segment + 1 < Line.size(); ++Segment)
		{
			const int64_t FirstColumn = CellIndex(std::min(Line[Segment].X, Line[Segment + 1].X));
			const int64_t LastColumn = CellIndex(std::max(Line[Segment].X, Line[Segment + 1].X));
			const int64_t FirstRow = CellIndex(std::min(Line[Segment].Y, Line[Segment + 1].Y));
			const int64_t LastRow = CellIndex(std::max(Line[Segment].Y, Line[Segment + 1].Y));
			for (int64_t Row = FirstRow; Row <= LastRow; ++Row)
			{
				for (int64_t Column = FirstColumn; Column <= LastColumn; ++Column)
				{
					SegmentCells[CellKey(Column, Row)].emplace_back(static_cast<int>(WayIndex), static_cast<int>(Segment));
				}
			}
		}
	}
}

const std::vector<FNodeEntry>* FRoadGraph::EntriesAt(int64_t Node) const
{
	const auto Found = AtNode.find(Node);
	if (Found == AtNode.end())
	{
		return nullptr;
	}
	return &Found->second;
}

bool FRoadGraph::IsJunction(int64_t Node) const
{
	const auto Found = Degree.find(Node);
	if (Found == Degree.end() || Found->second < 3)
	{
		return false;
	}
	for (const FNodeEntry& Entry : AtNode.at(Node))
	{
		if (IsRoundabout(Ways[Entry.Way].Tags))
		{
			return false;
		}
	}
	return true;
}

bool FRoadGraph::CanTravel(int Way, int Travel) const
{
	const FTags& Tags = Ways[Way].Tags;
	const std::string Oneway = OsmTags::ValueOrEmpty(Tags, "oneway");
	const std::string Highway = OsmTags::ValueOrEmpty(Tags, "highway");
	if (IsRoundabout(Tags) || Oneway == "yes" || Oneway == "1" || Oneway == "true" || Highway == "motorway"
		|| Highway == "motorway_link")
	{
		return Travel == +1;
	}
	if (Oneway == "-1")
	{
		return Travel == -1;
	}
	return true;
}

FPositionAndDirection FRoadGraph::PointAndDirection(int Way, double S, int Travel) const
{
	const FStreetPolyline& Line = Ways[Way].Points;
	const std::vector<double>& Along = WayAlong[Way];
	const double Length = Along.back();
	S = std::min(std::max(S, 0.0), Length);
	const FStreetPoint Here = Polyline::PointAt(Line, Along, S);
	const FStreetPoint Ahead =
		Polyline::PointAt(Line, Along, std::min(std::max(S + Travel * DirectionReach, 0.0), Length));
	const FStreetPoint Behind =
		Polyline::PointAt(Line, Along, std::min(std::max(S - Travel * DirectionReach, 0.0), Length));
	const FStreetPoint Delta = Ahead - Behind;
	const double Norm = std::hypot(Delta.X, Delta.Y);
	FStreetPoint Direction = {1.0, 0.0};
	if (Norm > 1e-6)
	{
		Direction = Delta / Norm;
	}
	return {Here, Direction};
}

double FRoadGraph::Project(int Way, const FStreetPoint& Point) const
{
	return ProjectOnLine(Ways[Way].Points, WayAlong[Way], Point);
}

double FRoadGraph::NodeS(int Way, int Index) const
{
	double& Cached = NodeSCache[Way][Index];
	if (std::isnan(Cached))
	{
		Cached = Project(Way, Ways[Way].Points[Index]);
	}
	return Cached;
}

int FRoadGraph::LanesPerDirection(int Way) const
{
	const FTags& Tags = Ways[Way].Tags;
	const int Total = MakeCrossSection(Tags, FRoadContext{true}).LaneCount();
	if (OsmTags::IsOneway(Tags))
	{
		return std::max(Total, 1);
	}
	return std::max(Total / 2, 1);
}

double FRoadGraph::LineLength(int Way) const
{
	return WayAlong[Way].back();
}

std::optional<FNearestWay> FRoadGraph::NearestWay(double X, double Y, double MaxDistance) const
{
	const FStreetPoint Point = {X, Y};
	double BestDistance = std::numeric_limits<double>::max();
	int BestWay = -1;
	for (int64_t Row = CellIndex(Y - MaxDistance); Row <= CellIndex(Y + MaxDistance); ++Row)
	{
		for (int64_t Column = CellIndex(X - MaxDistance); Column <= CellIndex(X + MaxDistance); ++Column)
		{
			const auto Cell = SegmentCells.find(CellKey(Column, Row));
			if (Cell == SegmentCells.end())
			{
				continue;
			}
			for (const auto& [Way, Segment] : Cell->second)
			{
				const FStreetPolyline& Line = Ways[Way].Points;
				const double Distance = Polyline::DistanceToSegment(Line[Segment], Line[Segment + 1], Point);
				if (Distance < BestDistance || (Distance == BestDistance && Way < BestWay))
				{
					BestDistance = Distance;
					BestWay = Way;
				}
			}
		}
	}
	if (BestWay < 0 || BestDistance > MaxDistance)
	{
		return std::nullopt;
	}
	const double Along = Project(BestWay, Point);
	const FPositionAndDirection Here = PointAndDirection(BestWay, Along, +1);
	const FStreetPoint Right = Polyline::RightOf(Here.Direction);
	return FNearestWay{BestWay, Along, (X - Here.Position.X) * Right.X + (Y - Here.Position.Y) * Right.Y};
}
}
