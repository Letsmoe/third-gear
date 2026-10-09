#include "Furniture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <cstdio>
#include <numbers>
#include <set>

#include "Assumptions.h"
#include "OsmTags.h"
#include "PointGrid.h"

namespace WorldBuilder
{
namespace
{
constexpr double JunctionMergeDistance = 30.0;
constexpr double ApproachSearchDistance = 90.0;
constexpr double StopLineMinSetback = 5.0;
// Two stop lines on one carriageway closer than this along the travel direction are one signal group (OSM maps a
// signal per carriageway, per direction or per pole, often a few metres apart, plus the arm's own stop line).
constexpr double SameGroupAlong = 25.0;
constexpr double SameGroupAcross = 4.0;
// Closer than this, stop lines on one carriageway are one group even when the way is split or the junctions differ.
constexpr double SameGroupAnyWayAlong = 12.0;
constexpr double SameGroupDot = 0.9;
constexpr double KerbClearance = 0.55;
constexpr double LampSpacing = 36.0;
constexpr double LampOsmMinGap = 14.0;
// A generated lamp keeps this far from every other lamp, so parallel carriageways and slip lanes share one row.
const double LampMinGap = 0.6 * LampSpacing;
// A pedestrian crossing light this close to a vehicle signal belongs to that junction's signals.
constexpr double CrossingSignalAbsorbDistance = 35.0;
// Half width of the cycleway strip poles keep clear of, around the OSM centre line.
constexpr double CyclewayClearHalfWidth = 1.2;
// How far a pole may be moved off the road surface and cycleways before it is dropped.
constexpr double PolePushLimit = 6.0;
// Signal poles may also stand across a slip lane and a few metres before the stop line, because the road surface has
// no traffic islands yet where the real ones stand.
constexpr double SignalPolePushLimit = 12.0;
constexpr double SignDistanceFromJunction = 9.0;
constexpr double ParallelDot = 0.82;
constexpr int BufferSegmentsPerQuarter = 16;

// Speed values we have a graphic for (Zeichen_274-<n>).
const std::vector<int> SpeedSigns = {10, 20, 30, 40, 50, 60, 70, 80, 100, 120};
// Every street in a German town is lit, but OSM tags lit=yes on only a few; these get lamps unless tagged lit=no or
// signed faster than a town street.
const std::set<std::string> TownStreets = {"primary",      "secondary",      "tertiary",      "unclassified",
										   "residential",  "living_street",  "primary_link",  "secondary_link",
										   "tertiary_link"};
// Graphics we can show for traffic_sign=DE:<code> (code without the bracketed value).
const std::set<std::string> KnownSigns = {
	"101",   "102",   "103-10", "103-20", "108",   "110",   "120",   "123",   "131",   "133",
	"136",   "138",   "205",    "206",    "208",   "209",   "211",   "214",   "220",   "222",
	"237",   "239",   "240",    "241",    "250",   "267",   "274",   "274.1", "274.2", "276",
	"278",   "282",   "283",    "286",    "301",   "306",   "307",   "310",   "311",   "325.1",
	"325.2", "350",   "357",    "1010-51", "1000-10", "1000-20", "1001-30", "1020-30", "1022-10", "1040-30",
	"1052-30"};

/** Python's round(Value, Digits): the correctly rounded decimal. */
double RoundDecimals(double Value, int Digits)
{
	char Text[64];
	std::snprintf(Text, sizeof(Text), "%.*f", Digits, Value);
	return std::strtod(Text, nullptr);
}

double DegreesOf(const FStreetPoint& Direction)
{
	return std::atan2(Direction.Y, Direction.X) * 180.0 / std::numbers::pi;
}

FStreetPoint RightOf(const FStreetPoint& Direction)
{
	return {-Direction.Y, Direction.X};
}

/** Which junction an approach belongs to: a junction node, or a standalone crossing signal. */
struct FJunctionKey
{
	bool bCrossing = false;
	int64_t Node = 0;
};

/** One direction of travel into a signalised junction. */
struct FApproach
{
	int Way = 0;
	/** +1 when vehicles move along increasing node index. */
	int Travel = 0;
	/** Distance along the way's line where the stop line is. */
	double StopS = 0.0;
	/** A traffic_signals node sits on the arm. */
	bool bExplicit = false;
	FJunctionKey Key;
	double Width = 6.0;
	int Lanes = 1;
	bool bOneway = false;
	double SpeedKmh = 50.0;
	int Rank = 0;
	FStreetPoint StopPoint;
	FStreetPoint Direction = {1.0, 0.0};
	int Phase = 0;
};

struct FJunction
{
	double X = 0.0;
	double Y = 0.0;
	bool bCrossingOnly = false;
	/** Indices into the builder's approach pool. */
	std::vector<int> Approaches;
};

/** Union-find over junction nodes, with path halving, in the order Python's dictionary version unites them. */
class FNodeUnion
{
public:
	int64_t Find(int64_t Node)
	{
		Parent.try_emplace(Node, Node);
		while (Parent[Node] != Node)
		{
			Parent[Node] = Parent[Parent[Node]];
			Node = Parent[Node];
		}
		return Node;
	}

	void Unite(int64_t First, int64_t Second)
	{
		Parent[Find(First)] = Find(Second);
	}

private:
	std::unordered_map<int64_t, int64_t> Parent;
};

/** The text Python's str() gives a cluster key, which orders the clusters. */
std::string ClusterKeyText(const FJunctionKey& Key)
{
	if (Key.bCrossing)
	{
		return "('crossing', " + std::to_string(Key.Node) + ")";
	}
	return std::to_string(Key.Node);
}

class FFurnitureBuilder
{
public:
	FFurnitureBuilder(const FOsmData& InData, const FRoadGraph& InGraph, const FPolygonSet& InGround,
					  const FPolygonSet& InBuildings)
		: Data(InData), Graph(InGraph), Ground(InGround), Buildings(InBuildings), LampGrid(LampMinGap),
		  OsmLampGrid(LampOsmMinGap)
	{
		BuildCyclewayStrips();
	}

	FFurniture Build();

private:
	const FOsmData& Data;
	const FRoadGraph& Graph;
	const FPolygonSet& Ground;
	const FPolygonSet& Buildings;
	FPolygonSet CyclewayStrips;
	std::vector<FApproach> Approaches;
	std::vector<FJunction> Junctions;
	FFurniture Result;
	FPointGrid LampGrid;
	FPointGrid OsmLampGrid;

	// ---- geometry helpers

	/** The strips of the separately mapped cycleways (and cycle-designated paths) that poles keep clear of. */
	void BuildCyclewayStrips();

	bool IsBlocked(double X, double Y, double Clearance) const;

	/**
	 * Moves a point along DirectionRight until it is clear of the road surface and the cycleways, plus Clearance.
	 * Nothing when there is no free spot within Limit (inside a big junction, say).
	 */
	std::optional<FStreetPoint> PushOffRoad(double X, double Y, const FStreetPoint& DirectionRight,
											double Clearance = KerbClearance, double Limit = PolePushLimit) const;

	/**
	 * The place itself when it is clear of the road and the cycleways, else the nearest free spot beside the nearest
	 * road, away from its centre line; nothing when there is none within reach.
	 */
	std::optional<FStreetPoint> OffRoadSpot(double X, double Y) const;

	bool InsideBuilding(double X, double Y) const;

	// ---- signals

	/** The next junction node in the travel direction within the search distance, if any. */
	std::optional<int64_t> DownstreamJunction(int Way, int Index, int Travel) const;
	int MakeApproach(int Way, int Travel, double StopS, bool bExplicit, const FJunctionKey& Key);
	void ExplicitApproaches(const std::vector<const FOsmPoint*>& SignalNodes, std::vector<int>& Explicit,
							std::vector<int64_t>& JunctionSignals);
	std::optional<int> ArmApproach(int64_t Node, int Way, int Index, int Step);
	void AssignPhases(const FJunction& Junction);
	std::vector<const FOsmPoint*> SignalNodes() const;
	void BuildJunctions();
	static bool FollowsOnCarriageway(const FApproach& First, const FApproach& Second, double MaxAlong);
	void MergeStackedApproaches(std::vector<FJunction>& Pending);
	std::vector<FPhaseRecord> SignalTiming(const FJunction& Junction) const;
	std::vector<FSignalHead> HeadPoles(const FApproach& Approach, int JunctionId, int ApproachId) const;
	std::optional<FStreetPoint> SignalPoleSpot(const FApproach& Approach, const FStreetPoint& Outward,
											   const std::vector<double>& Setbacks, double Limit) const;
	static std::array<double, 4> StopLine(const FApproach& Approach);

	// ---- signs

	void AddSign(double X, double Y, double YawDegrees, const std::vector<std::string>& Names);
	void SignBesideRoad(int Way, double S, int Travel, const std::vector<std::string>& Names);
	static std::string SpeedSignName(double Speed);
	static std::string LimitSignName(double Limit, bool bZone);
	void BuildSpeedSigns();
	void BuildPointSigns();
	void AddZebra(const FOsmPoint& Point);
	static std::vector<std::string> NamesFromTags(const FTags& Tags);
	void PlacePointSign(const FOsmPoint& Point, const std::vector<std::string>& Names);
	void PlaceJunctionSigns(const FOsmPoint& Point, const std::vector<std::string>& Names);

	// ---- lamps

	void BuildLamps();
	static bool IsLit(const FStreetWay& Way);
	void AddLitWayLamps(int Way);

	FTrafficNetwork BuildNetwork();
};

void FFurnitureBuilder::BuildCyclewayStrips()
{
	for (const FOsmWay& Way : Data.Footways)
	{
		if (!OsmTags::TagEquals(Way.Tags, "highway", "cycleway") && !OsmTags::TagEquals(Way.Tags, "bicycle", "designated"))
		{
			continue;
		}
		CyclewayStrips.Add(BufferPolyline(Way.Points, CyclewayClearHalfWidth, ECapStyle::Flat, BufferSegmentsPerQuarter));
	}
}

bool FFurnitureBuilder::IsBlocked(double X, double Y, double Clearance) const
{
	if (Ground.Contains(X, Y) || CyclewayStrips.Contains(X, Y))
	{
		return true;
	}
	return Ground.Distance(X, Y, Clearance) < Clearance || CyclewayStrips.Distance(X, Y, Clearance) < Clearance;
}

std::optional<FStreetPoint> FFurnitureBuilder::PushOffRoad(double X, double Y, const FStreetPoint& DirectionRight,
														   double Clearance, double Limit) const
{
	double Moved = 0.0;
	while (IsBlocked(X + DirectionRight.X * Moved, Y + DirectionRight.Y * Moved, Clearance))
	{
		Moved += 0.25;
		if (Moved > Limit)
		{
			return std::nullopt;
		}
	}
	return FStreetPoint{X + DirectionRight.X * Moved, Y + DirectionRight.Y * Moved};
}

std::optional<FStreetPoint> FFurnitureBuilder::OffRoadSpot(double X, double Y) const
{
	if (!Ground.Contains(X, Y) && !CyclewayStrips.Contains(X, Y))
	{
		return FStreetPoint{X, Y};
	}
	const std::optional<FNearestWay> Found = Graph.NearestWay(X, Y, 30.0);
	if (!Found.has_value())
	{
		return std::nullopt;
	}
	const FPositionAndDirection Here = Graph.PointAndDirection(Found->Way, Found->Along, +1);
	FStreetPoint Outward = RightOf(Here.Direction);
	if (Found->Offset < 0)
	{
		Outward = -Outward;
	}
	return PushOffRoad(X, Y, Outward, KerbClearance, Graph.WayWidth[Found->Way] / 2 + PolePushLimit);
}

bool FFurnitureBuilder::InsideBuilding(double X, double Y) const
{
	return Buildings.Contains(X, Y);
}

// ------------------------------------------------------------------ signals

std::optional<int64_t> FFurnitureBuilder::DownstreamJunction(int Way, int Index, int Travel) const
{
	const FStreetWay& WayData = Graph.Ways[Way];
	const double StartS = Graph.NodeS(Way, Index);
	for (int Node = Index + Travel; 0 <= Node && Node < static_cast<int>(WayData.NodeIds.size()); Node += Travel)
	{
		if (std::abs(Graph.NodeS(Way, Node) - StartS) > ApproachSearchDistance)
		{
			return std::nullopt;
		}
		if (Graph.IsJunction(WayData.NodeIds[Node]))
		{
			return WayData.NodeIds[Node];
		}
	}
	return std::nullopt;
}

int FFurnitureBuilder::MakeApproach(int Way, int Travel, double StopS, bool bExplicit, const FJunctionKey& Key)
{
	FApproach Approach;
	Approach.Way = Way;
	Approach.Travel = Travel;
	Approach.StopS = StopS;
	Approach.bExplicit = bExplicit;
	Approach.Key = Key;
	Approach.Width = Graph.WayWidth[Way];
	Approach.Lanes = Graph.LanesPerDirection(Way);
	Approach.bOneway = OsmTags::IsOneway(Graph.Ways[Way].Tags);
	Approach.SpeedKmh = Graph.WaySpeed[Way];
	Approach.Rank = Assumptions::RoadClassRank(OsmTags::ValueOrEmpty(Graph.Ways[Way].Tags, "highway"));
	const FPositionAndDirection Here = Graph.PointAndDirection(Way, StopS, Travel);
	Approach.StopPoint = Here.Position;
	Approach.Direction = Here.Direction;
	Approaches.push_back(Approach);
	return static_cast<int>(Approaches.size()) - 1;
}

void FFurnitureBuilder::ExplicitApproaches(const std::vector<const FOsmPoint*>& SignalNodes, std::vector<int>& Explicit,
										   std::vector<int64_t>& JunctionSignals)
{
	for (const FOsmPoint* Point : SignalNodes)
	{
		const std::vector<FNodeEntry>* Entries = Graph.EntriesAt(Point->Id);
		if (Entries == nullptr)
		{
			continue;
		}
		if (Graph.IsJunction(Point->Id))
		{
			JunctionSignals.push_back(Point->Id);
			continue;
		}
		const FNodeEntry Entry = (*Entries)[0];
		std::string DirectionTag = OsmTags::ValueOrEmpty(Point->Tags, "traffic_signals:direction");
		if (DirectionTag.empty())
		{
			DirectionTag = OsmTags::ValueOrEmpty(Point->Tags, "direction");
		}
		std::vector<int> Travels = {+1, -1};
		if (DirectionTag == "forward")
		{
			Travels = {+1};
		}
		else if (DirectionTag == "backward")
		{
			Travels = {-1};
		}
		for (int Travel : Travels)
		{
			if (!Graph.CanTravel(Entry.Way, Travel))
			{
				continue;
			}
			const double StopS = Graph.NodeS(Entry.Way, Entry.Index);
			const std::optional<int64_t> Junction = DownstreamJunction(Entry.Way, Entry.Index, Travel);
			FJunctionKey Key;
			if (Junction.has_value())
			{
				Key.Node = *Junction;
			}
			else
			{
				Key.bCrossing = true;
				Key.Node = Point->Id;
			}
			Explicit.push_back(MakeApproach(Entry.Way, Travel, StopS, true, Key));
		}
	}
}

std::optional<int> FFurnitureBuilder::ArmApproach(int64_t Node, int Way, int Index, int Step)
{
	const int Travel = -Step;
	if (!Graph.CanTravel(Way, Travel))
	{
		return std::nullopt;
	}
	const FTags& Tags = Graph.Ways[Way].Tags;
	const std::string Highway = OsmTags::ValueOrEmpty(Tags, "highway");
	if (Highway == "service" || Highway == "track")
	{
		return std::nullopt;
	}
	if (Highway.size() >= 5 && Highway.compare(Highway.size() - 5, 5, "_link") == 0)
	{
		return std::nullopt;  // slip lanes mostly turn free and give way; OSM maps a signal on them when there is one
	}
	double CrossingWidth = 6.0;
	bool bFirst = true;
	for (const FNodeEntry& Entry : *Graph.EntriesAt(Node))
	{
		if (Graph.Ways[Entry.Way].Id == Graph.Ways[Way].Id)
		{
			continue;
		}
		CrossingWidth = bFirst ? Graph.WayWidth[Entry.Way] : std::max(CrossingWidth, Graph.WayWidth[Entry.Way]);
		bFirst = false;
	}
	const double Setback = std::max(StopLineMinSetback, CrossingWidth / 2 + 2.0);
	const double StopS = Graph.NodeS(Way, Index) - Travel * Setback;
	if (StopS < 0.5 || StopS > Graph.LineLength(Way) - 0.5)
	{
		return std::nullopt;
	}
	return MakeApproach(Way, Travel, StopS, false, FJunctionKey{false, Node});
}

void FFurnitureBuilder::AssignPhases(const FJunction& Junction)
{
	std::vector<int> Ordered = Junction.Approaches;
	std::stable_sort(Ordered.begin(), Ordered.end(),
					 [this](int First, int Second) { return -Approaches[First].Rank < -Approaches[Second].Rank; });
	std::vector<std::vector<int>> Phases;
	for (int Index : Ordered)
	{
		FApproach& Approach = Approaches[Index];
		bool bPlaced = false;
		for (size_t PhaseIndex = 0; PhaseIndex < Phases.size() && !bPlaced; ++PhaseIndex)
		{
			bool bAllParallel = true;
			for (int Member : Phases[PhaseIndex])
			{
				bAllParallel = bAllParallel && std::abs(Dot(Approach.Direction, Approaches[Member].Direction)) >= ParallelDot;
			}
			if (bAllParallel)
			{
				Phases[PhaseIndex].push_back(Index);
				Approach.Phase = static_cast<int>(PhaseIndex);
				bPlaced = true;
			}
		}
		if (!bPlaced)
		{
			Approach.Phase = static_cast<int>(Phases.size());
			Phases.push_back({Index});
		}
	}
}

std::vector<const FOsmPoint*> FFurnitureBuilder::SignalNodes() const
{
	std::vector<const FOsmPoint*> Vehicle;
	std::vector<const FOsmPoint*> Crossings;
	for (const FOsmPoint& Point : Data.Points)
	{
		if (OsmTags::TagEquals(Point.Tags, "highway", "traffic_signals"))
		{
			Vehicle.push_back(&Point);
		}
		if (OsmTags::TagEquals(Point.Tags, "highway", "crossing")
			&& OsmTags::TagEquals(Point.Tags, "crossing", "traffic_signals"))
		{
			Crossings.push_back(&Point);
		}
	}
	if (Vehicle.empty())
	{
		return Crossings;
	}
	FPointGrid VehicleGrid(CrossingSignalAbsorbDistance);
	for (size_t Index = 0; Index < Vehicle.size(); ++Index)
	{
		VehicleGrid.Add({Vehicle[Index]->Position.X, Vehicle[Index]->Position.Y}, static_cast<int>(Index));
	}
	for (const FOsmPoint* Crossing : Crossings)
	{
		const FStreetPoint Place = {Crossing->Position.X, Crossing->Position.Y};
		if (VehicleGrid.Within(Place, CrossingSignalAbsorbDistance).empty())
		{
			Vehicle.push_back(Crossing);
		}
	}
	return Vehicle;
}

bool FFurnitureBuilder::FollowsOnCarriageway(const FApproach& First, const FApproach& Second, double MaxAlong)
{
	if (Dot(First.Direction, Second.Direction) < SameGroupDot)
	{
		return false;
	}
	const FStreetPoint Offset = Second.StopPoint - First.StopPoint;
	const double Along = Offset.X * First.Direction.X + Offset.Y * First.Direction.Y;
	const double Across = Offset.X * First.Direction.Y - Offset.Y * First.Direction.X;
	return 0.0 <= Along && Along <= MaxAlong && std::abs(Across) <= SameGroupAcross;
}

void FFurnitureBuilder::MergeStackedApproaches(std::vector<FJunction>& Pending)
{
	struct FOwner
	{
		int Junction;
		int Approach;
	};
	std::vector<FOwner> Owners;
	FPointGrid StopGrid(SameGroupAlong + 1.0);
	for (size_t JunctionIndex = 0; JunctionIndex < Pending.size(); ++JunctionIndex)
	{
		for (int Approach : Pending[JunctionIndex].Approaches)
		{
			StopGrid.Add(Approaches[Approach].StopPoint, static_cast<int>(Owners.size()));
			Owners.push_back({static_cast<int>(JunctionIndex), Approach});
		}
	}
	std::vector<bool> Dropped(Approaches.size(), false);
	for (size_t First = 0; First < Owners.size(); ++First)
	{
		std::vector<int> Candidates = StopGrid.Within(Approaches[Owners[First].Approach].StopPoint, SameGroupAlong + 1.0);
		std::sort(Candidates.begin(), Candidates.end());
		for (int Second : Candidates)
		{
			if (Second <= static_cast<int>(First))
			{
				continue;
			}
			const FOwner& A = Owners[First];
			const FOwner& B = Owners[Second];
			if (Dropped[A.Approach] || Dropped[B.Approach])
			{
				continue;
			}
			double MaxAlong = SameGroupAlong;
			if (Approaches[A.Approach].Way != Approaches[B.Approach].Way && A.Junction != B.Junction)
			{
				MaxAlong = SameGroupAnyWayAlong;
			}
			int Leading = 0;
			int Following = 0;
			if (FollowsOnCarriageway(Approaches[A.Approach], Approaches[B.Approach], MaxAlong))
			{
				Leading = A.Approach;
				Following = B.Approach;
			}
			else if (FollowsOnCarriageway(Approaches[B.Approach], Approaches[A.Approach], MaxAlong))
			{
				Leading = B.Approach;
				Following = A.Approach;
			}
			else
			{
				continue;
			}
			// Keep the stop line OSM maps explicitly, else the one nearer the junction (the follower).
			int Kept = Following;
			if (Approaches[Leading].bExplicit != Approaches[Following].bExplicit && Approaches[Leading].bExplicit)
			{
				Kept = Leading;
			}
			Dropped[Kept == Leading ? Following : Leading] = true;
		}
	}
	for (FJunction& Junction : Pending)
	{
		std::vector<int> Remaining;
		for (int Approach : Junction.Approaches)
		{
			if (!Dropped[Approach])
			{
				Remaining.push_back(Approach);
			}
		}
		Junction.Approaches = Remaining;
	}
}

void FFurnitureBuilder::BuildJunctions()
{
	std::vector<int> Explicit;
	std::vector<int64_t> JunctionSignals;
	ExplicitApproaches(SignalNodes(), Explicit, JunctionSignals);

	// Merge junction nodes of one big junction (dual carriageways, slip roads).
	FNodeUnion Union;
	std::set<int64_t> KeySet(JunctionSignals.begin(), JunctionSignals.end());
	for (int Index : Explicit)
	{
		if (!Approaches[Index].Key.bCrossing)
		{
			KeySet.insert(Approaches[Index].Key.Node);
		}
	}
	const std::vector<int64_t> Keys(KeySet.begin(), KeySet.end());
	FPointGrid KeyGrid(JunctionMergeDistance);
	std::unordered_map<int64_t, int> KeyIndex;
	for (size_t Index = 0; Index < Keys.size(); ++Index)
	{
		Union.Find(Keys[Index]);
		KeyGrid.Add(Graph.NodePoint.at(Keys[Index]), static_cast<int>(Index));
		KeyIndex[Keys[Index]] = static_cast<int>(Index);
	}
	for (size_t First = 0; First < Keys.size(); ++First)
	{
		std::vector<int> Near = KeyGrid.Within(Graph.NodePoint.at(Keys[First]), JunctionMergeDistance);
		std::sort(Near.begin(), Near.end());
		for (int Second : Near)
		{
			if (Second <= static_cast<int>(First))
			{
				continue;
			}
			if (Norm(Graph.NodePoint.at(Keys[First]) - Graph.NodePoint.at(Keys[Second])) < JunctionMergeDistance)
			{
				Union.Unite(Keys[First], Keys[Second]);
			}
		}
	}

	struct FCluster
	{
		FJunctionKey Key;
		std::vector<int> Approaches;
	};
	std::vector<FCluster> Clusters;
	std::map<std::pair<bool, int64_t>, int> ClusterIndex;
	const auto ClusterFor = [&](const FJunctionKey& Key) -> FCluster& {
		const auto Found = ClusterIndex.find({Key.bCrossing, Key.Node});
		if (Found != ClusterIndex.end())
		{
			return Clusters[Found->second];
		}
		ClusterIndex[{Key.bCrossing, Key.Node}] = static_cast<int>(Clusters.size());
		Clusters.push_back({Key, {}});
		return Clusters.back();
	};
	for (int Index : Explicit)
	{
		const FJunctionKey& Key = Approaches[Index].Key;
		FJunctionKey ClusterKey = Key;
		if (!Key.bCrossing)
		{
			ClusterKey.Node = Union.Find(Key.Node);
		}
		ClusterFor(ClusterKey).Approaches.push_back(Index);
	}
	for (int64_t Node : JunctionSignals)
	{
		ClusterFor(FJunctionKey{false, Union.Find(Node)});
	}
	std::vector<std::pair<std::string, int>> Order;
	for (size_t Index = 0; Index < Clusters.size(); ++Index)
	{
		Order.emplace_back(ClusterKeyText(Clusters[Index].Key), static_cast<int>(Index));
	}
	std::stable_sort(Order.begin(), Order.end());

	std::vector<FJunction> Pending;
	for (const auto& [KeyText, ClusterId] : Order)
	{
		FCluster& Cluster = Clusters[ClusterId];
		FJunction Junction;
		Junction.bCrossingOnly = Cluster.Key.bCrossing;
		std::vector<int> Members = Cluster.Approaches;
		if (!Cluster.Key.bCrossing)
		{
			std::vector<int64_t> MemberNodes;
			for (int64_t Node : Keys)
			{
				if (Union.Find(Node) == Cluster.Key.Node)
				{
					MemberNodes.push_back(Node);
				}
			}
			const std::set<int64_t> MemberSet(MemberNodes.begin(), MemberNodes.end());
			std::set<std::pair<int64_t, int>> Covered;
			for (int Index : Members)
			{
				Covered.insert({Graph.Ways[Approaches[Index].Way].Id, Approaches[Index].Travel});
			}
			for (int64_t Node : MemberNodes)
			{
				for (const FNodeEntry& Entry : *Graph.EntriesAt(Node))
				{
					const FStreetWay& Way = Graph.Ways[Entry.Way];
					for (int Step : {-1, +1})
					{
						const int Neighbour = Entry.Index + Step;
						if (Neighbour < 0 || Neighbour >= static_cast<int>(Way.NodeIds.size()))
						{
							continue;
						}
						const bool bLinkWithinJunction =
							Graph.IsJunction(Way.NodeIds[Neighbour]) && MemberSet.count(Way.NodeIds[Neighbour]) > 0;
						if (bLinkWithinJunction || Covered.count({Way.Id, -Step}) > 0)
						{
							continue;
						}
						const std::optional<int> Arm = ArmApproach(Node, Entry.Way, Entry.Index, Step);
						if (Arm.has_value())
						{
							Members.push_back(*Arm);
							Covered.insert({Way.Id, Approaches[*Arm].Travel});
						}
					}
				}
			}
			double SumX = 0.0;
			double SumY = 0.0;
			for (int64_t Node : MemberNodes)
			{
				SumX += Graph.NodePoint.at(Node).X;
				SumY += Graph.NodePoint.at(Node).Y;
			}
			Junction.X = SumX / MemberNodes.size();
			Junction.Y = SumY / MemberNodes.size();
		}
		else
		{
			double SumX = 0.0;
			double SumY = 0.0;
			for (int Index : Members)
			{
				SumX += Approaches[Index].StopPoint.X;
				SumY += Approaches[Index].StopPoint.Y;
			}
			Junction.X = SumX / Members.size();
			Junction.Y = SumY / Members.size();
		}
		// An explicit approach on a way that also appears twice (the same travel) keeps the one nearest the junction.
		std::set<std::pair<int, int>> Seen;
		for (int Index : Members)
		{
			if (Seen.insert({Approaches[Index].Way, Approaches[Index].Travel}).second)
			{
				Junction.Approaches.push_back(Index);
			}
		}
		Pending.push_back(std::move(Junction));
	}

	MergeStackedApproaches(Pending);
	for (FJunction& Junction : Pending)
	{
		if (Junction.Approaches.empty())
		{
			continue;
		}
		AssignPhases(Junction);
		Junctions.push_back(std::move(Junction));
	}
}

std::vector<FPhaseRecord> FFurnitureBuilder::SignalTiming(const FJunction& Junction) const
{
	int PhaseCount = 0;
	for (int Index : Junction.Approaches)
	{
		PhaseCount = std::max(PhaseCount, Approaches[Index].Phase + 1);
	}
	const int TertiaryRank = Assumptions::RoadClassRank("tertiary");
	std::vector<FPhaseRecord> Phases;
	for (int PhaseIndex = 0; PhaseIndex < PhaseCount; ++PhaseIndex)
	{
		int MaxRank = 0;
		double Speed = 0.0;
		double MaxWidth = 0.0;
		bool bAny = false;
		for (int Index : Junction.Approaches)
		{
			const FApproach& Approach = Approaches[Index];
			if (Approach.Phase != PhaseIndex)
			{
				continue;
			}
			MaxRank = bAny ? std::max(MaxRank, Approach.Rank) : Approach.Rank;
			Speed = bAny ? std::max(Speed, Approach.SpeedKmh) : Approach.SpeedKmh;
			MaxWidth = bAny ? std::max(MaxWidth, Approach.Width) : Approach.Width;
			bAny = true;
		}
		if (!bAny)
		{
			MaxRank = 0;
			Speed = 50.0;
			MaxWidth = 6.0;
		}
		const bool bMajor = MaxRank >= TertiaryRank;
		const double Amber = Speed <= 50 ? 3.0 : (Speed <= 60 ? 4.0 : 5.0);
		const double Green = bMajor ? 28.0 : 18.0;
		const double Extent = MaxWidth + 8.0;
		const double Clearance = std::min(std::max(Extent / std::max(Speed / 3.6, 8.0), 2.0), 4.5);
		Phases.push_back({Green, Amber, RoundDecimals(Clearance, 1), false});
	}
	if (Junction.bCrossingOnly)
	{
		Phases[0].Green = 35.0;
	}
	if (Phases.size() == 1)
	{
		// Nothing crosses the only phase in the data (side streets or pedestrians are not mapped as approaches): give
		// them a phase of their own so the junction is not green forever.
		Phases.push_back({10.0, 0.0, 2.0, true});
	}
	return Phases;
}

std::optional<FStreetPoint> FFurnitureBuilder::SignalPoleSpot(const FApproach& Approach, const FStreetPoint& Outward,
															  const std::vector<double>& Setbacks, double Limit) const
{
	const double HalfWidth = Approach.Width / 2;
	for (double Setback : Setbacks)
	{
		const double X = Approach.StopPoint.X - Approach.Direction.X * Setback + Outward.X * HalfWidth;
		const double Y = Approach.StopPoint.Y - Approach.Direction.Y * Setback + Outward.Y * HalfWidth;
		const std::optional<FStreetPoint> Placed = PushOffRoad(X, Y, Outward, KerbClearance, Limit);
		if (Placed.has_value())
		{
			return Placed;
		}
	}
	return std::nullopt;
}

std::vector<FSignalHead> FFurnitureBuilder::HeadPoles(const FApproach& Approach, int JunctionId, int ApproachId) const
{
	std::vector<FSignalHead> Poles;
	const FStreetPoint Right = RightOf(Approach.Direction);
	const double Yaw = DegreesOf(Approach.Direction);
	const std::optional<FStreetPoint> RightSpot =
		SignalPoleSpot(Approach, Right, {0.0, 3.0, 6.0, 9.0, 12.0}, SignalPolePushLimit);
	if (RightSpot.has_value())
	{
		Poles.push_back({RightSpot->X, RightSpot->Y, Yaw, ApproachId, JunctionId, false});
	}
	if (Approach.bOneway && Approach.Lanes >= 2)
	{
		const std::optional<FStreetPoint> LeftSpot = SignalPoleSpot(Approach, -Right, {0.0}, PolePushLimit);
		if (LeftSpot.has_value())
		{
			Poles.push_back({LeftSpot->X, LeftSpot->Y, Yaw, ApproachId, JunctionId, true});
		}
	}
	std::vector<FSignalHead> Outside;
	for (const FSignalHead& Pole : Poles)
	{
		if (!InsideBuilding(Pole.X, Pole.Y))
		{
			Outside.push_back(Pole);
		}
	}
	return Outside;
}

std::array<double, 4> FFurnitureBuilder::StopLine(const FApproach& Approach)
{
	const FStreetPoint Right = RightOf(Approach.Direction);
	const double Half = Approach.Width / 2;
	const double LeftExtent = Approach.bOneway ? Half : 0.0;
	return {Approach.StopPoint.X - Right.X * LeftExtent, Approach.StopPoint.Y - Right.Y * LeftExtent,
			Approach.StopPoint.X + Right.X * Half, Approach.StopPoint.Y + Right.Y * Half};
}

// ------------------------------------------------------------------ signs

void FFurnitureBuilder::AddSign(double X, double Y, double YawDegrees, const std::vector<std::string>& Names)
{
	const std::optional<FStreetPoint> Spot = OffRoadSpot(X, Y);
	if (!Spot.has_value() || InsideBuilding(Spot->X, Spot->Y))
	{
		return;
	}
	Result.Signs.push_back({Spot->X, Spot->Y, YawDegrees, Names});
}

void FFurnitureBuilder::SignBesideRoad(int Way, double S, int Travel, const std::vector<std::string>& Names)
{
	const FPositionAndDirection Here = Graph.PointAndDirection(Way, S, Travel);
	const FStreetPoint Right = RightOf(Here.Direction);
	const double Half = Graph.WayWidth[Way] / 2;
	const std::optional<FStreetPoint> Placed =
		PushOffRoad(Here.Position.X + Right.X * Half, Here.Position.Y + Right.Y * Half, Right);
	if (!Placed.has_value())
	{
		return;
	}
	AddSign(Placed->X, Placed->Y, DegreesOf(Here.Direction), Names);
}

std::string FFurnitureBuilder::SpeedSignName(double Speed)
{
	int Nearest = SpeedSigns[0];
	for (int Value : SpeedSigns)
	{
		if (std::abs(Value - Speed) < std::abs(Nearest - Speed))
		{
			Nearest = Value;
		}
	}
	return "Zeichen_274-" + std::to_string(Nearest);
}

std::string FFurnitureBuilder::LimitSignName(double Limit, bool bZone)
{
	if (bZone && Limit == 30)
	{
		return "Zeichen_274.1";
	}
	if (bZone && Limit == 20)
	{
		return "Zeichen_274.1-20";
	}
	return SpeedSignName(Limit);
}

void FFurnitureBuilder::BuildSpeedSigns()
{
	for (size_t WayIndex = 0; WayIndex < Graph.Ways.size(); ++WayIndex)
	{
		const int Way = static_cast<int>(WayIndex);
		const FStreetWay& WayData = Graph.Ways[Way];
		const std::string Highway = OsmTags::ValueOrEmpty(WayData.Tags, "highway");
		if ((Highway == "service" || Highway == "track") && !OsmTags::HasTag(WayData.Tags, "maxspeed"))
		{
			continue;
		}
		const double Length = Graph.LineLength(Way);
		if (Length < 25.0)
		{
			continue;
		}
		const double Limit = Graph.WaySpeed[Way];
		const bool bZone = OsmTags::ValueOrEmpty(WayData.Tags, "maxspeed:type").find("zone") != std::string::npos
			|| OsmTags::ValueOrEmpty(WayData.Tags, "zone:maxspeed").find("zone") != std::string::npos;
		const int LastIndex = static_cast<int>(WayData.NodeIds.size()) - 1;
		for (const auto& [EndIndex, Travel] : {std::pair<int, int>{0, +1}, std::pair<int, int>{LastIndex, -1}})
		{
			if (!Graph.CanTravel(Way, Travel))
			{
				continue;
			}
			std::vector<FNodeEntry> Others;
			for (const FNodeEntry& Entry : *Graph.EntriesAt(WayData.NodeIds[EndIndex]))
			{
				if (Graph.Ways[Entry.Way].Id != WayData.Id)
				{
					Others.push_back(Entry);
				}
			}
			if (Others.empty())
			{
				continue;
			}
			const double StartS = Travel == +1 ? 0.0 : Length;
			const FStreetPoint Direction = Graph.PointAndDirection(Way, StartS, Travel).Direction;
			double BestStraightness = 0.0;
			int BestWay = -1;
			for (const FNodeEntry& Other : Others)
			{
				const int Outward = Other.Index == static_cast<int>(Graph.Ways[Other.Way].NodeIds.size()) - 1 ? -1 : +1;
				const FStreetPoint Inward =
					Graph.PointAndDirection(Other.Way, Graph.NodeS(Other.Way, Other.Index), -Outward).Direction;
				const double Straightness = Dot(Inward, Direction);
				if (BestWay < 0 || Straightness > BestStraightness)
				{
					BestStraightness = Straightness;
					BestWay = Other.Way;
				}
			}
			if (std::abs(Graph.WaySpeed[BestWay] - Limit) < 0.5 || Limit <= 0)
			{
				continue;
			}
			SignBesideRoad(Way, StartS + Travel * SignDistanceFromJunction, Travel, {LimitSignName(Limit, bZone)});
		}
	}
}

std::vector<std::string> FFurnitureBuilder::NamesFromTags(const FTags& Tags)
{
	std::vector<std::string> Names;
	const std::string Value = OsmTags::ValueOrEmpty(Tags, "traffic_sign");
	if (Value.empty())
	{
		return Names;
	}
	for (std::string Part : OsmTags::SplitText(OsmTags::SplitText(Value, ';')[0], ','))
	{
		for (size_t Position = Part.find("DE:"); Position != std::string::npos; Position = Part.find("DE:"))
		{
			Part.erase(Position, 3);
		}
		size_t Begin = Part.find_first_not_of(" \t\n\r\f\v");
		if (Begin == std::string::npos)
		{
			continue;
		}
		Part = Part.substr(Begin, Part.find_last_not_of(" \t\n\r\f\v") - Begin + 1);
		// re.match(r"([0-9.]+(?:-[0-9]+)?)(?:\[(\d+)\])?", part)
		size_t End = 0;
		while (End < Part.size() && (std::isdigit(static_cast<unsigned char>(Part[End])) || Part[End] == '.'))
		{
			++End;
		}
		if (End == 0)
		{
			continue;
		}
		if (End + 1 < Part.size() && Part[End] == '-' && std::isdigit(static_cast<unsigned char>(Part[End + 1])))
		{
			++End;
			while (End < Part.size() && std::isdigit(static_cast<unsigned char>(Part[End])))
			{
				++End;
			}
		}
		const std::string Code = Part.substr(0, End);
		std::string Number;
		if (End < Part.size() && Part[End] == '[')
		{
			size_t Close = End + 1;
			while (Close < Part.size() && std::isdigit(static_cast<unsigned char>(Part[Close])))
			{
				++Close;
			}
			if (Close > End + 1 && Close < Part.size() && Part[Close] == ']')
			{
				Number = Part.substr(End + 1, Close - End - 1);
			}
		}
		if ((Code == "274" || Code == "278") && !Number.empty())
		{
			int Nearest = SpeedSigns[0];
			const int Wanted = std::stoi(Number);
			for (int Speed : SpeedSigns)
			{
				if (std::abs(Speed - Wanted) < std::abs(Nearest - Wanted))
				{
					Nearest = Speed;
				}
			}
			Names.push_back("Zeichen_" + Code + "-" + std::to_string(Nearest));
		}
		else if (KnownSigns.count(Code) > 0)
		{
			const bool bAdditional = Code.rfind("10", 0) == 0;
			Names.push_back((bAdditional ? "Zusatzzeichen_" : "Zeichen_") + Code);
		}
	}
	return Names;
}

void FFurnitureBuilder::AddZebra(const FOsmPoint& Point)
{
	const std::vector<FNodeEntry>* Entries = Graph.EntriesAt(Point.Id);
	int Way = 0;
	double S = 0.0;
	if (Entries != nullptr)
	{
		if (Graph.IsJunction(Point.Id))
		{
			return;
		}
		Way = (*Entries)[0].Way;
		S = Graph.NodeS(Way, (*Entries)[0].Index);
	}
	else
	{
		const std::optional<FNearestWay> Found = Graph.NearestWay(Point.Position.X, Point.Position.Y, 6.0);
		if (!Found.has_value())
		{
			return;
		}
		Way = Found->Way;
		S = Found->Along;
	}
	const FPositionAndDirection Here = Graph.PointAndDirection(Way, S, +1);
	Result.Zebras.push_back({Here.Position.X, Here.Position.Y, Here.Direction, Graph.WayWidth[Way]});
}

void FFurnitureBuilder::BuildPointSigns()
{
	for (const FOsmPoint& Point : Data.Points)
	{
		const FTags& Tags = Point.Tags;
		std::vector<std::string> Names = NamesFromTags(Tags);
		if (OsmTags::TagEquals(Tags, "highway", "stop"))
		{
			if (Names.empty())
			{
				Names = {"Zeichen_206"};
			}
		}
		else if (OsmTags::TagEquals(Tags, "highway", "give_way"))
		{
			if (Names.empty())
			{
				Names = {"Zeichen_205"};
			}
		}
		else if ((OsmTags::TagEquals(Tags, "highway", "crossing")
				  && (OsmTags::TagEquals(Tags, "crossing", "zebra") || OsmTags::TagEquals(Tags, "crossing", "marked")))
				 || OsmTags::TagEquals(Tags, "crossing_ref", "zebra"))
		{
			Names = {"Zeichen_350"};
			AddZebra(Point);
		}
		if (Names.empty())
		{
			continue;
		}
		PlacePointSign(Point, Names);
	}
}

void FFurnitureBuilder::PlacePointSign(const FOsmPoint& Point, const std::vector<std::string>& Names)
{
	std::string DirectionTag = OsmTags::ValueOrEmpty(Point.Tags, "direction");
	if (DirectionTag.empty())
	{
		DirectionTag = OsmTags::ValueOrEmpty(Point.Tags, "traffic_sign:direction");
	}
	const std::vector<FNodeEntry>* Entries = Graph.EntriesAt(Point.Id);
	if (Entries != nullptr)
	{
		const FNodeEntry Entry = (*Entries)[0];
		if (Graph.IsJunction(Point.Id))
		{
			PlaceJunctionSigns(Point, Names);
			return;
		}
		std::vector<int> Travels = {+1, -1};
		if (DirectionTag == "forward")
		{
			Travels = {+1};
		}
		else if (DirectionTag == "backward")
		{
			Travels = {-1};
		}
		else if (OsmTags::TagEquals(Point.Tags, "highway", "crossing"))
		{
			Travels.clear();
			for (int Travel : {+1, -1})
			{
				if (Graph.CanTravel(Entry.Way, Travel))
				{
					Travels.push_back(Travel);
				}
			}
		}
		const double S = Graph.NodeS(Entry.Way, Entry.Index);
		for (int Travel : Travels)
		{
			if (Graph.CanTravel(Entry.Way, Travel))
			{
				SignBesideRoad(Entry.Way, S - Travel * 3.0, Travel, Names);
			}
		}
		return;
	}
	const std::optional<FNearestWay> Found = Graph.NearestWay(Point.Position.X, Point.Position.Y, 30.0);
	if (!Found.has_value())
	{
		return;
	}
	const int Travel = Found->Offset > 0 ? +1 : -1;  // the node is right of the line direction, so it serves that way
	if (!Graph.CanTravel(Found->Way, Travel))
	{
		return;
	}
	const FPositionAndDirection Here = Graph.PointAndDirection(Found->Way, Found->Along, Travel);
	AddSign(Point.Position.X, Point.Position.Y, DegreesOf(Here.Direction), Names);
}

void FFurnitureBuilder::PlaceJunctionSigns(const FOsmPoint& Point, const std::vector<std::string>& Names)
{
	struct FArm
	{
		int Way;
		int Index;
		int Step;
	};
	std::vector<FArm> Arms;
	for (const FNodeEntry& Entry : *Graph.EntriesAt(Point.Id))
	{
		for (int Step : {-1, +1})
		{
			const int Neighbour = Entry.Index + Step;
			if (0 <= Neighbour && Neighbour < static_cast<int>(Graph.Ways[Entry.Way].NodeIds.size())
				&& Graph.CanTravel(Entry.Way, -Step))
			{
				Arms.push_back({Entry.Way, Entry.Index, Step});
			}
		}
	}
	if (Arms.size() < 2)
	{
		return;
	}
	std::vector<FStreetPoint> Directions;
	for (const FArm& Arm : Arms)
	{
		Directions.push_back(Graph.PointAndDirection(Arm.Way, Graph.NodeS(Arm.Way, Arm.Index), -Arm.Step).Direction);
	}
	std::set<size_t> Through;
	if (Arms.size() >= 3)
	{
		bool bHaveBest = false;
		double BestStraightness = 0.0;
		size_t BestFirst = 0;
		size_t BestSecond = 0;
		for (size_t First = 0; First < Arms.size(); ++First)
		{
			for (size_t Second = First + 1; Second < Arms.size(); ++Second)
			{
				const double Straightness = -Dot(Directions[First], Directions[Second]);
				if (!bHaveBest || Straightness > BestStraightness)
				{
					bHaveBest = true;
					BestStraightness = Straightness;
					BestFirst = First;
					BestSecond = Second;
				}
			}
		}
		Through = {BestFirst, BestSecond};
	}
	for (size_t Index = 0; Index < Arms.size(); ++Index)
	{
		if (Through.count(Index) > 0)
		{
			continue;
		}
		const double Setback = SignDistanceFromJunction * 0.6;
		SignBesideRoad(Arms[Index].Way, Graph.NodeS(Arms[Index].Way, Arms[Index].Index) + Arms[Index].Step * Setback,
					   -Arms[Index].Step, Names);
	}
}

// ------------------------------------------------------------------ lamps

bool FFurnitureBuilder::IsLit(const FStreetWay& Way)
{
	const std::string Lit = OsmTags::ValueOrEmpty(Way.Tags, "lit");
	const std::string Highway = OsmTags::ValueOrEmpty(Way.Tags, "highway");
	if (Highway == "motorway" || Highway == "motorway_link" || Lit == "no")
	{
		return false;
	}
	if (Lit == "yes")
	{
		return true;
	}
	const double Speed = ParseSpeed(Way.Tags, true);
	return TownStreets.count(Highway) > 0 && 0 < Speed && Speed <= 50;
}

void FFurnitureBuilder::AddLitWayLamps(int Way)
{
	const double Length = Graph.LineLength(Way);
	if (Length < 12.0)
	{
		return;
	}
	const double Width = Graph.WayWidth[Way];
	const bool bBothSides = Width >= 7.5;
	const int Count = std::max(static_cast<int>(std::floor(Length / LampSpacing)), 1);
	for (int K = 0; K < Count; ++K)
	{
		const double S = (K + 0.5) * Length / Count;
		const FPositionAndDirection Here = Graph.PointAndDirection(Way, S, +1);
		const FStreetPoint Right = RightOf(Here.Direction);
		const int Side = (bBothSides && K % 2 == 1) ? -1 : 1;
		const FStreetPoint Normal = {Right.X * Side, Right.Y * Side};
		const std::optional<FStreetPoint> Placed =
			PushOffRoad(Here.Position.X + Normal.X * Width / 2, Here.Position.Y + Normal.Y * Width / 2, Normal, 0.6);
		if (!Placed.has_value() || InsideBuilding(Placed->X, Placed->Y))
		{
			continue;
		}
		if (OsmLampGrid.AnyCloserThan(*Placed, LampOsmMinGap) || LampGrid.AnyCloserThan(*Placed, LampMinGap))
		{
			continue;
		}
		LampGrid.Add(*Placed, static_cast<int>(Result.Lamps.size()));
		Result.Lamps.push_back({Placed->X, Placed->Y, DegreesOf(-Normal), false});
	}
}

void FFurnitureBuilder::BuildLamps()
{
	for (const FOsmPoint& Point : Data.Points)
	{
		if (!OsmTags::TagEquals(Point.Tags, "highway", "street_lamp"))
		{
			continue;
		}
		const std::optional<FNearestWay> Found = Graph.NearestWay(Point.Position.X, Point.Position.Y, 40.0);
		double Yaw = 0.0;
		if (Found.has_value())
		{
			const FPositionAndDirection Here = Graph.PointAndDirection(Found->Way, Found->Along, +1);
			const FStreetPoint Right = RightOf(Here.Direction);
			const FStreetPoint Toward = Found->Offset > 0 ? -Right : Right;
			Yaw = DegreesOf(Toward);
		}
		if (!InsideBuilding(Point.Position.X, Point.Position.Y))
		{
			const FStreetPoint Place = {Point.Position.X, Point.Position.Y};
			OsmLampGrid.Add(Place, static_cast<int>(Result.Lamps.size()));
			LampGrid.Add(Place, static_cast<int>(Result.Lamps.size()));
			Result.Lamps.push_back({Place.X, Place.Y, Yaw, true});
		}
	}
	for (size_t Way = 0; Way < Graph.Ways.size(); ++Way)
	{
		if (IsLit(Graph.Ways[Way]))
		{
			AddLitWayLamps(static_cast<int>(Way));
		}
	}
}

// ------------------------------------------------------------------ output

FTrafficNetwork FFurnitureBuilder::BuildNetwork()
{
	FTrafficNetwork Network;
	int ApproachId = 0;
	for (size_t JunctionIndex = 0; JunctionIndex < Junctions.size(); ++JunctionIndex)
	{
		const FJunction& Junction = Junctions[JunctionIndex];
		const int JunctionId = static_cast<int>(JunctionIndex);
		FJunctionRecord Record;
		Record.Id = JunctionId;
		Record.X = RoundDecimals(Junction.X, 1);
		Record.Y = RoundDecimals(Junction.Y, 1);
		Record.bCrossingOnly = Junction.bCrossingOnly;
		Record.Phases = SignalTiming(Junction);
		for (int Index : Junction.Approaches)
		{
			const FApproach& Approach = Approaches[Index];
			for (const FSignalHead& Pole : HeadPoles(Approach, JunctionId, ApproachId))
			{
				Result.Heads.push_back(Pole);
			}
			FApproachRecord Approach_;
			Approach_.Id = ApproachId;
			Approach_.Phase = Approach.Phase;
			Approach_.Way = Graph.Ways[Approach.Way].Id;
			const std::array<double, 4> Line = StopLine(Approach);
			for (int Value = 0; Value < 4; ++Value)
			{
				Approach_.StopLine[Value] = RoundDecimals(Line[Value], 2);
			}
			Approach_.Direction[0] = RoundDecimals(Approach.Direction.X, 4);
			Approach_.Direction[1] = RoundDecimals(Approach.Direction.Y, 4);
			Approach_.Lanes = Approach.Lanes;
			Approach_.SpeedKmh = Approach.SpeedKmh;
			Approach_.Travel = Approach.Travel;
			Approach_.StopPoint = Approach.StopPoint;
			Record.Approaches.push_back(Approach_);
			++ApproachId;
		}
		Network.Junctions.push_back(std::move(Record));
	}
	for (size_t Way = 0; Way < Graph.Ways.size(); ++Way)
	{
		const FStreetWay& WayData = Graph.Ways[Way];
		FSpeedWay Speed;
		Speed.Id = WayData.Id;
		Speed.LimitKmh = Graph.WaySpeed[Way];
		Speed.Width = RoundDecimals(Graph.WayWidth[Way], 1);
		Speed.bOneway = OsmTags::IsOneway(WayData.Tags);
		for (const FStreetPoint& Point : WayData.Points)
		{
			Speed.Points.push_back({std::nearbyint(Point.X * 100.0) / 100.0, std::nearbyint(Point.Y * 100.0) / 100.0});
		}
		Network.SpeedWays.push_back(std::move(Speed));
	}
	return Network;
}

FFurniture FFurnitureBuilder::Build()
{
	BuildJunctions();
	BuildSpeedSigns();
	BuildPointSigns();
	BuildLamps();
	Result.Network = BuildNetwork();
	return std::move(Result);
}
}

FFurniture BuildFurniture(const FOsmData& Data, const FRoadGraph& Graph, const FPolygonSet& RoadGround,
						  const FPolygonSet& Buildings)
{
	FFurnitureBuilder Builder(Data, Graph, RoadGround, Buildings);
	return Builder.Build();
}
}
