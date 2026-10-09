#include "Lanes.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <set>

#include "Assumptions.h"
#include "OsmTags.h"
#include "Parking.h"
#include "TrafficJson.h"

namespace WorldBuilder
{
namespace
{
constexpr double PiHalf = std::numbers::pi / 2;
constexpr double SignGridCell = 20.0;

// Importance of a road class for who has right of way and where cars like to go.
const std::map<std::string, int> ClassTier = {
	{"service", 0},       {"living_street", 0}, {"residential", 1},   {"unclassified", 1},  {"road", 1},
	{"tertiary_link", 2}, {"tertiary", 2},      {"secondary_link", 3}, {"secondary", 3},     {"primary_link", 4},
	{"primary", 4},       {"trunk_link", 4},    {"trunk", 4}};
const std::set<std::string> PrivateServices = {"driveway", "parking_aisle", "drive-through", "emergency_access",
											   "slipway"};

/** numpy.round(Value, Decimals): scale, round half to even, scale back. */
double NumpyRound(double Value, int Decimals)
{
	const double Scale = std::pow(10.0, Decimals);
	return std::nearbyint(Value * Scale) / Scale;
}

/** Python's round(Value, Decimals): the correctly rounded decimal. */
double PythonRound(double Value, int Decimals)
{
	char Text[64];
	std::snprintf(Text, sizeof(Text), "%.*f", Decimals, Value);
	return std::strtod(Text, nullptr);
}

/** Rounds corners: repeated 1-2-1 averaging with the end points pinned. */
FStreetPolyline Smooth(const FStreetPolyline& Line, int Passes = 3)
{
	FStreetPolyline Out = Line;
	if (Out.size() < 4)
	{
		return Out;
	}
	for (int Pass = 0; Pass < Passes; ++Pass)
	{
		const FStreetPolyline Before = Out;
		for (size_t Index = 1; Index + 1 < Out.size(); ++Index)
		{
			Out[Index] = Before[Index - 1] * 0.25 + Before[Index] * 0.5 + Before[Index + 1] * 0.25;
		}
	}
	return Out;
}

/** Per point speed (km/h) that keeps the lateral acceleration under LateralAcceleration, from the turning angle. */
std::vector<double> CurvatureSpeeds(const FStreetPolyline& Line, double LimitKmh, std::optional<double> CapKmh = std::nullopt)
{
	const int Count = static_cast<int>(Line.size());
	std::vector<double> Speeds(Count, LimitKmh);
	if (Count < 3)
	{
		return Speeds;  // too short to bend, and the cap is not applied either
	}
	const int Window = 2;
	for (int Index = 0; Index < Count; ++Index)
	{
		const FStreetPoint& A = Line[std::max(Index - Window, 0)];
		const FStreetPoint& B = Line[Index];
		const FStreetPoint& C = Line[std::min(Index + Window, Count - 1)];
		const FStreetPoint AB = B - A;
		const FStreetPoint BC = C - B;
		const double LengthAB = std::hypot(AB.X, AB.Y);
		const double LengthBC = std::hypot(BC.X, BC.Y);
		if (LengthAB < 1e-6 || LengthBC < 1e-6)
		{
			continue;
		}
		const double CosAngle = std::min(std::max(Dot(AB, BC) / (LengthAB * LengthBC), -1.0), 1.0);
		const double Angle = std::acos(CosAngle);
		const double Chord = std::hypot(C.X - A.X, C.Y - A.Y);
		if (Angle < 1e-3 || Chord < 1e-6)
		{
			continue;
		}
		double Radius = Chord / 2.0;
		if (Angle < PiHalf)
		{
			Radius = Chord / (2.0 * std::sin(std::min(Angle, PiHalf - 1e-3)) + 1e-9);
		}
		Speeds[Index] = std::min(Speeds[Index], std::sqrt(LateralAcceleration * std::max(Radius, 1.0)) * 3.6);
	}
	if (CapKmh.has_value())
	{
		for (double& Speed : Speeds)
		{
			Speed = std::min(Speed, *CapKmh);
		}
	}
	return Speeds;
}

/** A piece of a way between two graph nodes, possibly clipped to the area. */
struct FLaneSegment
{
	int Id = 0;
	const FStreetWay* Way = nullptr;
	FStreetPolyline Xy;
	int64_t StartNode = 0;
	int64_t EndNode = 0;
};

/** The segment, travel direction pair a lane belongs to, as a map key. */
int64_t LaneKey(int Segment, int Travel)
{
	return static_cast<int64_t>(Segment) * 2 + (Travel > 0 ? 0 : 1);
}

class FLaneBuilder
{
public:
	FLaneBuilder(const FStreetModel& InModel, const FRoadGraph& InGraph, const FFurniture& InFurniture, const FBox& InArea)
		: Model(InModel), Graph(InGraph), Furniture(InFurniture), Area(InArea)
	{
	}

	FLaneGraph Build();

private:
	const FStreetModel& Model;
	const FRoadGraph& Graph;
	const FFurniture& Furniture;
	FBox Area;
	std::vector<const FStreetWay*> Ways;
	std::unordered_map<int64_t, int> GraphWayOfId;
	std::vector<FLaneSegment> Segments;
	int SyntheticNodes = 0;
	std::vector<FLane> Lanes;
	std::unordered_map<int64_t, std::vector<int>> Arms;
	std::vector<int64_t> ArmNodes;
	std::unordered_map<int64_t, int> ArmCount;
	std::unordered_map<int64_t, double> NodeWidth;
	std::unordered_map<int64_t, FStreetPolyline> Raw;
	std::unordered_map<int64_t, std::vector<std::pair<double, int>>> SignalStops;
	std::vector<std::pair<int, int>> ApproachPhase;
	std::unordered_map<int64_t, int> LaneOf;
	std::map<int64_t, std::vector<int>> NodeConnections;
	std::vector<int64_t> ConnectionNodes;

	bool IsTrafficWay(const FStreetWay& Way) const;
	void BuildSegments();
	void AddClipped(const FStreetWay& Way, const FStreetPolyline& Xy, int64_t StartNode, int64_t EndNode,
					const FBox& Box);
	std::vector<int> Directions(const FStreetWay& Way) const;
	double BaseLaneOffset(const FStreetWay& Way, int Travel) const;
	double LaneOffset(const FStreetWay& Way, int Travel) const;
	static std::vector<double> TaperedOffsets(const FStreetPolyline& Line, double Base, double Shifted);
	FStreetPolyline RawLanePath(const FLaneSegment& Segment, int Travel) const;
	void BuildRoadLanes();
	void LinkSignalStops();
	std::pair<double, double> TrimFor(const FLaneSegment& Segment, int Travel, double RawLength) const;
	double LaneLimit(const FStreetWay& Way) const;
	void MakeRoadLane(const FLaneSegment& Segment, int Travel);
	static FStreetPoint EndDirection(const FLane& Lane, bool bAtEnd);
	void BuildConnections();
	bool Connect(int LaneIn, int LaneOut, int64_t Node, bool bMulti, bool bUTurn);
	std::unordered_map<int, std::string> SignControls() const;
	void AssignControls();
	std::optional<std::pair<bool, bool>> YieldsTo(const FLane& A, const FLane& B) const;
	void FindConflicts();
	int MarkGoodLanes();
};

bool FLaneBuilder::IsTrafficWay(const FStreetWay& Way) const
{
	const FTags& Tags = Way.Tags;
	const std::string Highway = OsmTags::ValueOrEmpty(Tags, "highway");
	if (ClassTier.count(Highway) == 0)
	{
		return false;
	}
	if (Highway == "service" && PrivateServices.count(OsmTags::ValueOrEmpty(Tags, "service")) > 0)
	{
		return false;
	}
	const std::string Access = OsmTags::ValueOrEmpty(Tags, "access");
	const std::string MotorVehicle = OsmTags::ValueOrEmpty(Tags, "motor_vehicle");
	if ((Access == "private" || Access == "no") && MotorVehicle != "yes" && MotorVehicle != "destination")
	{
		return false;
	}
	return MotorVehicle != "no" && OsmTags::ValueOrEmpty(Tags, "motorcar") != "no";
}

void FLaneBuilder::AddClipped(const FStreetWay& Way, const FStreetPolyline& Xy, int64_t StartNode, int64_t EndNode,
							  const FBox& Box)
{
	bool bInside = true;
	for (const FStreetPoint& Point : Xy)
	{
		bInside = bInside && Box.X0 <= Point.X && Point.X <= Box.X1 && Box.Y0 <= Point.Y && Point.Y <= Box.Y1;
	}
	if (bInside)
	{
		Segments.push_back({static_cast<int>(Segments.size()), &Way, Xy, StartNode, EndNode});
		return;
	}
	FPolyline Line;
	for (const FStreetPoint& Point : Xy)
	{
		Line.push_back({Point.X, Point.Y});
	}
	for (const FPolylinePiece& Piece : ClipPolylineToBox(Line, Box))
	{
		if (PolylineLength(Piece.Points) < 3.0)
		{
			continue;
		}
		FStreetPolyline Coords;
		for (const FWorldPoint& Point : Piece.Points)
		{
			Coords.push_back({Point.X, Point.Y});
		}
		const bool bKeepFirst = Norm(Coords.front() - Xy.front()) < 1e-6;
		const int64_t FirstNode = bKeepFirst ? StartNode : -(++SyntheticNodes);
		const bool bKeepLast = Norm(Coords.back() - Xy.back()) < 1e-6;
		const int64_t LastNode = bKeepLast ? EndNode : -(++SyntheticNodes);
		Segments.push_back({static_cast<int>(Segments.size()), &Way, Coords, FirstNode, LastNode});
	}
}

void FLaneBuilder::BuildSegments()
{
	std::vector<const FStreetWay*> TrafficWays;
	for (const FStreetWay* Way : Ways)
	{
		if (IsTrafficWay(*Way))
		{
			TrafficWays.push_back(Way);
		}
	}
	std::unordered_map<int64_t, int> Occurrences;
	std::set<int64_t> Ends;
	for (const FStreetWay* Way : TrafficWays)
	{
		Ends.insert(Way->NodeIds.front());
		Ends.insert(Way->NodeIds.back());
		for (int64_t Node : Way->NodeIds)
		{
			++Occurrences[Node];
		}
	}
	const FBox Box = {Area.X0 + EdgeInset, Area.Y0 + EdgeInset, Area.X1 - EdgeInset, Area.Y1 - EdgeInset};
	for (const FStreetWay* Way : TrafficWays)
	{
		std::vector<size_t> CutIndices;
		for (size_t Index = 0; Index < Way->NodeIds.size(); ++Index)
		{
			const int64_t Node = Way->NodeIds[Index];
			if (Ends.count(Node) > 0 || Occurrences[Node] >= 2)
			{
				CutIndices.push_back(Index);
			}
		}
		for (size_t Cut = 0; Cut + 1 < CutIndices.size(); ++Cut)
		{
			const size_t First = CutIndices[Cut];
			const size_t Last = CutIndices[Cut + 1];
			const FStreetPolyline Xy(Way->Points.begin() + First, Way->Points.begin() + Last + 1);
			if (Xy.size() < 2 || Polyline::Length(Xy) < 0.5)
			{
				continue;
			}
			AddClipped(*Way, Xy, Way->NodeIds[First], Way->NodeIds[Last], Box);
		}
	}
}

std::vector<int> FLaneBuilder::Directions(const FStreetWay& Way) const
{
	std::vector<int> Result;
	for (int Travel : {+1, -1})
	{
		if (CanTravelByTags(Way.Tags, Travel))
		{
			Result.push_back(Travel);
		}
	}
	return Result;
}

double FLaneBuilder::BaseLaneOffset(const FStreetWay& Way, int Travel) const
{
	const FCrossSection& Section = Model.Sections.at(Way.Id);
	const double Offset = Section.KerbLaneCentre(Travel > 0 ? ETravel::Forward : ETravel::Backward);
	if (OsmTags::IsOneway(Way.Tags))
	{
		return std::max(Offset, 0.0);
	}
	const double Width = Section.Width();
	return std::min(std::max(Offset, MinLaneOffset), std::max(Width / 2.0 - 1.0, MinLaneOffset));
}

double FLaneBuilder::LaneOffset(const FStreetWay& Way, int Travel) const
{
	const double Width = Model.Widths.at(Way.Id);
	return LaneOffsetWithParking(BaseLaneOffset(Way, Travel), Width, Travel, ParkingSides(Way.Tags, Way.Id, Width));
}

std::vector<double> FLaneBuilder::TaperedOffsets(const FStreetPolyline& Line, double Base, double Shifted)
{
	const std::vector<double> Along = Polyline::Arclength(Line);
	std::vector<double> Offsets;
	for (double Distance : Along)
	{
		const double FromEnd = std::min(Distance, Along.back() - Distance);
		const double Blend = std::min(std::max((FromEnd - ParkingTaperStart) / ParkingTaperLength, 0.0), 1.0);
		Offsets.push_back(Base + (Shifted - Base) * Blend);
	}
	return Offsets;
}

FStreetPolyline FLaneBuilder::RawLanePath(const FLaneSegment& Segment, int Travel) const
{
	const FStreetPolyline Xy = Travel > 0 ? Segment.Xy : Polyline::Reversed(Segment.Xy);
	const double Base = BaseLaneOffset(*Segment.Way, Travel);
	const double Shifted = LaneOffset(*Segment.Way, Travel);
	if (std::abs(Shifted - Base) < 1e-6)
	{
		return Polyline::OffsetPolyline(Xy, Base);
	}
	const FStreetPolyline Resampled = Polyline::Resample(Xy, 1.0);
	return Polyline::OffsetPolyline(Resampled, TaperedOffsets(Resampled, Base, Shifted));
}

void FLaneBuilder::LinkSignalStops()
{
	std::unordered_map<int64_t, std::vector<int>> SegmentsOfWay;
	for (const FLaneSegment& Segment : Segments)
	{
		SegmentsOfWay[Segment.Way->Id].push_back(Segment.Id);
	}
	for (const FJunctionRecord& Junction : Furniture.Network.Junctions)
	{
		for (const FApproachRecord& Approach : Junction.Approaches)
		{
			ApproachPhase.push_back({Junction.Id, Approach.Phase});
			const auto Candidates = SegmentsOfWay.find(Approach.Way);
			if (Candidates == SegmentsOfWay.end())
			{
				continue;
			}
			bool bFound = false;
			double BestMismatch = 0.0;
			int BestSegment = 0;
			double BestS = 0.0;
			for (int SegmentId : Candidates->second)
			{
				const auto RawPath = Raw.find(LaneKey(SegmentId, Approach.Travel));
				if (RawPath == Raw.end())
				{
					continue;
				}
				const auto [S, Distance] = Polyline::ProjectOnPolyline(RawPath->second, Approach.StopPoint);
				// The stop point is on the way's centre line and the lane is offset from it: compare against that offset.
				const double Mismatch = std::abs(Distance - BaseLaneOffset(*Segments[SegmentId].Way, Approach.Travel));
				if (Mismatch > 4.0)
				{
					continue;
				}
				if (!bFound || Mismatch < BestMismatch)
				{
					bFound = true;
					BestMismatch = Mismatch;
					BestSegment = SegmentId;
					BestS = S;
				}
			}
			if (bFound)
			{
				SignalStops[LaneKey(BestSegment, Approach.Travel)].push_back({BestS, Approach.Id});
			}
		}
	}
}

std::pair<double, double> FLaneBuilder::TrimFor(const FLaneSegment& Segment, int Travel, double RawLength) const
{
	const int64_t StartNode = Travel > 0 ? Segment.StartNode : Segment.EndNode;
	const int64_t EndNode = Travel > 0 ? Segment.EndNode : Segment.StartNode;
	const auto JunctionTrim = [this](int64_t Node) {
		if (ArmCount.at(Node) < 3)
		{
			return 0.0;
		}
		return NodeWidth.at(Node) * 0.5 + JunctionTrimBase;
	};
	double TrimStart = JunctionTrim(StartNode);
	double TrimEnd = JunctionTrim(EndNode);
	const auto Stops = SignalStops.find(LaneKey(Segment.Id, Travel));
	if (Stops != SignalStops.end())
	{
		for (const auto& [S, Approach] : Stops->second)
		{
			const double Behind = RawLength - S;
			if (0.0 <= Behind && Behind < 40.0 && ArmCount.at(EndNode) >= 3)
			{
				TrimEnd = std::max(TrimEnd, Behind + StopTrimExtra);
			}
		}
	}
	const double Limit = RawLength * 0.45;
	return {std::min(TrimStart, Limit), std::min(TrimEnd, Limit)};
}

double FLaneBuilder::LaneLimit(const FStreetWay& Way) const
{
	double Limit = 0.0;
	const auto Found = GraphWayOfId.find(Way.Id);
	if (Found != GraphWayOfId.end())
	{
		Limit = Graph.WaySpeed[Found->second];
	}
	else
	{
		Limit = ParseSpeed(Way.Tags, true);  // bridges are not in the furniture graph
	}
	return Limit > 0 ? Limit : 100.0;
}

void FLaneBuilder::MakeRoadLane(const FLaneSegment& Segment, int Travel)
{
	const FStreetPolyline& RawPath = Raw.at(LaneKey(Segment.Id, Travel));
	const double RawLength = Polyline::Length(RawPath);
	const auto [TrimStart, TrimEnd] = TrimFor(Segment, Travel, RawLength);
	const FStreetPolyline Cut = Polyline::CutPolyline(RawPath, TrimStart, RawLength - TrimEnd);
	if (Cut.size() < 2 || Polyline::Length(Cut) < 0.3)
	{
		return;
	}
	FLane Lane;
	Lane.Id = static_cast<int>(Lanes.size());
	Lane.Kind = ELaneKind::Road;
	Lane.Way = Segment.Way->Id;
	Lane.Points = Smooth(Polyline::Resample(Cut, LaneSpacing));
	Lane.LimitKmh = LaneLimit(*Segment.Way);
	const auto Tier = ClassTier.find(OsmTags::ValueOrEmpty(Segment.Way->Tags, "highway"));
	Lane.Tier = Tier != ClassTier.end() ? Tier->second : 1;
	Lane.bRoundabout = OsmTags::TagEquals(Segment.Way->Tags, "junction", "roundabout")
		|| OsmTags::TagEquals(Segment.Way->Tags, "junction", "circular");
	Lane.StartNode = Travel > 0 ? Segment.StartNode : Segment.EndNode;
	Lane.EndNode = Travel > 0 ? Segment.EndNode : Segment.StartNode;
	Lane.Segment = Segment.Id;
	Lane.Travel = Travel;
	const double Length = Lane.Length();
	const auto Stops = SignalStops.find(LaneKey(Segment.Id, Travel));
	if (Stops != SignalStops.end())
	{
		for (const auto& [S, Approach] : Stops->second)
		{
			// A stop line may lie before the lane's start (the junction trim took the lane's first metres): negative.
			Lane.Stops.push_back({std::min(std::max(S - TrimStart, -MaxStopBeforeLane), Length), Approach});
		}
	}
	Lane.CurveKmh = CurvatureSpeeds(Lane.Points, Lane.LimitKmh);
	LaneOf[LaneKey(Segment.Id, Travel)] = Lane.Id;
	for (int64_t Node : {Lane.StartNode, Lane.EndNode})
	{
		auto [Found, bInserted] = Arms.try_emplace(Node);
		if (bInserted)
		{
			ArmNodes.push_back(Node);
		}
		Found->second.push_back(Lane.Id);
	}
	Lanes.push_back(std::move(Lane));
}

void FLaneBuilder::BuildRoadLanes()
{
	for (const FLaneSegment& Segment : Segments)
	{
		for (int Travel : Directions(*Segment.Way))
		{
			Raw[LaneKey(Segment.Id, Travel)] = RawLanePath(Segment, Travel);
		}
	}
	for (const FLaneSegment& Segment : Segments)
	{
		if (Directions(*Segment.Way).empty())
		{
			continue;
		}
		++ArmCount[Segment.StartNode];
		++ArmCount[Segment.EndNode];
	}
	for (const FLaneSegment& Segment : Segments)
	{
		const double Width = Model.Widths.at(Segment.Way->Id);
		for (int64_t Node : {Segment.StartNode, Segment.EndNode})
		{
			double& Widest = NodeWidth[Node];
			Widest = std::max(Widest, Width);
			ArmCount.try_emplace(Node, 0);
		}
	}
	LinkSignalStops();
	for (const FLaneSegment& Segment : Segments)
	{
		for (int Travel : Directions(*Segment.Way))
		{
			MakeRoadLane(Segment, Travel);
		}
	}
}

FStreetPoint FLaneBuilder::EndDirection(const FLane& Lane, bool bAtEnd)
{
	const FStreetPolyline& Points = Lane.Points;
	if (bAtEnd)
	{
		return Polyline::Unit(Points[Points.size() - 1] - Points[Points.size() - 2]);
	}
	return Polyline::Unit(Points[1] - Points[0]);
}

bool FLaneBuilder::Connect(int LaneInId, int LaneOutId, int64_t Node, bool bMulti, bool bUTurn)
{
	const FLane& LaneIn = Lanes[LaneInId];
	const FLane& LaneOut = Lanes[LaneOutId];
	const FStreetPoint P0 = LaneIn.Points.back();
	const FStreetPoint P3 = LaneOut.Points.front();
	const FStreetPoint DirIn = EndDirection(LaneIn, true);
	const FStreetPoint DirOut = EndDirection(LaneOut, false);
	const double Cross = DirIn.X * DirOut.Y - DirIn.Y * DirOut.X;
	const double Angle = std::atan2(Cross, Dot(DirIn, DirOut)) * 180.0 / std::numbers::pi;  // positive = right turn
	if (!bUTurn && std::abs(Angle) > MaxTurnDegrees && bMulti)
	{
		return false;
	}
	const double Chord = Norm(P3 - P0);
	double Handle = std::max(Chord * 0.4, 0.5);
	if (bUTurn)
	{
		Handle = 6.0;
	}
	FLane Connection;
	Connection.Id = static_cast<int>(Lanes.size());
	Connection.Kind = bUTurn ? ELaneKind::UTurn : ELaneKind::Connection;
	Connection.Way = LaneIn.Way;
	Connection.Points = Polyline::Bezier(P0, P0 + DirIn * Handle, P3 - DirOut * Handle, P3, ConnectionSpacing);
	Connection.FromLane = LaneInId;
	Connection.ToLane = LaneOutId;
	Connection.Node = Node;
	Connection.LimitKmh = std::min(LaneIn.LimitKmh, LaneOut.LimitKmh);
	Connection.bRoundabout = LaneIn.bRoundabout && LaneOut.bRoundabout;
	Connection.Tier = LaneIn.Tier;
	if (std::abs(Angle) < StraightAngleDegrees)
	{
		Connection.Turn = 0;
	}
	else
	{
		Connection.Turn = Angle > 0 ? 1 : -1;
	}
	std::optional<double> TurnCap;
	if (bUTurn)
	{
		TurnCap = 15.0;
	}
	else if (Connection.Turn != 0 && bMulti)
	{
		TurnCap = 25.0;
	}
	Connection.CurveKmh = CurvatureSpeeds(Connection.Points, Connection.LimitKmh, TurnCap);
	if (Connection.Points.size() < 2)
	{
		return false;
	}
	Connection.Next = {LaneOutId};
	Lanes[LaneInId].Next.push_back(Connection.Id);
	NodeConnections[Node].push_back(Connection.Id);
	Lanes.push_back(std::move(Connection));
	return true;
}

void FLaneBuilder::BuildConnections()
{
	std::vector<std::pair<std::string, int64_t>> Sorted;
	for (int64_t Node : ArmNodes)
	{
		Sorted.emplace_back(std::to_string(Node), Node);
	}
	std::sort(Sorted.begin(), Sorted.end());
	for (const auto& [Text, Node] : Sorted)
	{
		std::vector<int> Incoming;
		std::vector<int> Outgoing;
		std::set<int> SegmentsHere;
		for (int LaneId : Arms[Node])
		{
			SegmentsHere.insert(Lanes[LaneId].Segment);
			if (Lanes[LaneId].EndNode == Node && std::find(Incoming.begin(), Incoming.end(), LaneId) == Incoming.end())
			{
				Incoming.push_back(LaneId);
			}
			if (Lanes[LaneId].StartNode == Node && std::find(Outgoing.begin(), Outgoing.end(), LaneId) == Outgoing.end())
			{
				Outgoing.push_back(LaneId);
			}
		}
		const bool bMulti = SegmentsHere.size() >= 3 || ArmCount.at(Node) >= 3;
		for (int LaneInId : Incoming)
		{
			bool bMade = false;
			for (int LaneOutId : Outgoing)
			{
				const FLane& LaneIn = Lanes[LaneInId];
				const FLane& LaneOut = Lanes[LaneOutId];
				if (LaneOut.Segment == LaneIn.Segment && LaneOut.Travel != LaneIn.Travel && LaneIn.Id != LaneOut.Id)
				{
					continue;  // turning back on the same road is only for dead ends
				}
				if (LaneOut.Id == LaneIn.Id)
				{
					continue;
				}
				const bool bConnected = Connect(LaneInId, LaneOutId, Node, bMulti, false);
				bMade = bMade || bConnected;
			}
			if (bMade)
			{
				continue;
			}
			// Dead end: turn around on the road when the opposite lane exists.
			for (int LaneOutId : Outgoing)
			{
				if (Lanes[LaneOutId].Segment == Lanes[LaneInId].Segment)
				{
					if (ArmCount.at(Node) <= 1)
					{
						Connect(LaneInId, LaneOutId, Node, bMulti, true);
					}
					break;
				}
			}
		}
	}
}

std::unordered_map<int, std::string> FLaneBuilder::SignControls() const
{
	// Road lane segments by cell, to find the lanes near a sign without scanning every lane.
	std::unordered_map<int64_t, std::vector<int>> Cells;
	const auto Key = [](int64_t Column, int64_t Row) { return Row * 1000003 + Column; };
	const auto Cell = [](double Coordinate) { return static_cast<int64_t>(std::floor(Coordinate / SignGridCell)); };
	for (const FLane& Lane : Lanes)
	{
		if (Lane.Kind != ELaneKind::Road)
		{
			continue;
		}
		std::set<int64_t> Seen;
		for (size_t Index = 0; Index + 1 < Lane.Points.size(); ++Index)
		{
			for (int64_t Row = Cell(std::min(Lane.Points[Index].Y, Lane.Points[Index + 1].Y));
				 Row <= Cell(std::max(Lane.Points[Index].Y, Lane.Points[Index + 1].Y)); ++Row)
			{
				for (int64_t Column = Cell(std::min(Lane.Points[Index].X, Lane.Points[Index + 1].X));
					 Column <= Cell(std::max(Lane.Points[Index].X, Lane.Points[Index + 1].X)); ++Column)
				{
					if (Seen.insert(Key(Column, Row)).second)
					{
						Cells[Key(Column, Row)].push_back(Lane.Id);
					}
				}
			}
		}
	}
	std::unordered_map<int, std::string> Controls;
	for (const FSign& Sign : Furniture.Signs)
	{
		std::string Kind;
		if (std::find(Sign.Names.begin(), Sign.Names.end(), "Zeichen_206") != Sign.Names.end())
		{
			Kind = "stop";
		}
		else if (std::find(Sign.Names.begin(), Sign.Names.end(), "Zeichen_205") != Sign.Names.end())
		{
			Kind = "yield";
		}
		else
		{
			continue;
		}
		const FStreetPoint Position = {Sign.X, Sign.Y};
		const double Yaw = Sign.YawDegrees * std::numbers::pi / 180.0;
		const FStreetPoint Direction = {std::cos(Yaw), std::sin(Yaw)};
		std::vector<int> Candidates;
		for (int64_t Row = Cell(Position.Y - SignSnapDistance); Row <= Cell(Position.Y + SignSnapDistance); ++Row)
		{
			for (int64_t Column = Cell(Position.X - SignSnapDistance); Column <= Cell(Position.X + SignSnapDistance); ++Column)
			{
				const auto Found = Cells.find(Key(Column, Row));
				if (Found != Cells.end())
				{
					Candidates.insert(Candidates.end(), Found->second.begin(), Found->second.end());
				}
			}
		}
		std::sort(Candidates.begin(), Candidates.end());
		Candidates.erase(std::unique(Candidates.begin(), Candidates.end()), Candidates.end());
		int BestLane = -1;
		double BestDistance = 0.0;
		for (int LaneId : Candidates)
		{
			const FLane& Lane = Lanes[LaneId];
			const auto [S, Distance] = Polyline::ProjectOnPolyline(Lane.Points, Position);
			if (Distance > SignSnapDistance || Lane.Length() - S > 40.0)
			{
				continue;
			}
			if (Dot(EndDirection(Lane, true), Direction) < 0.7)
			{
				continue;
			}
			if (BestLane < 0 || Distance < BestDistance)
			{
				BestLane = LaneId;
				BestDistance = Distance;
			}
		}
		if (BestLane >= 0)
		{
			Controls[BestLane] = Kind;
		}
	}
	return Controls;
}

void FLaneBuilder::AssignControls()
{
	const std::unordered_map<int, std::string> SignControlOfLane = SignControls();
	for (const auto& [Node, Connections] : NodeConnections)
	{
		if (ArmCount.at(Node) < 3)
		{
			continue;
		}
		std::vector<int> Incoming;
		for (int LaneId : Arms.at(Node))
		{
			if (Lanes[LaneId].EndNode == Node)
			{
				Incoming.push_back(LaneId);
			}
		}
		bool bNodeHasSignal = false;
		bool bNodeHasSign = false;
		bool bAnyRoundabout = false;
		bool bAnyOutside = false;
		int Top = Lanes[Incoming[0]].Tier;
		int Lowest = Top;
		for (int LaneId : Incoming)
		{
			const FLane& Lane = Lanes[LaneId];
			bNodeHasSignal = bNodeHasSignal || !Lane.Stops.empty();
			bNodeHasSign = bNodeHasSign || SignControlOfLane.count(LaneId) > 0;
			bAnyRoundabout = bAnyRoundabout || Lane.bRoundabout;
			bAnyOutside = bAnyOutside || !Lane.bRoundabout;
			Top = std::max(Top, Lane.Tier);
			Lowest = std::min(Lowest, Lane.Tier);
		}
		int TopCount = 0;
		for (int LaneId : Incoming)
		{
			TopCount += Lanes[LaneId].Tier == Top ? 1 : 0;
		}
		const bool bRingNode = bAnyRoundabout && bAnyOutside;
		for (int LaneId : Incoming)
		{
			FLane& Lane = Lanes[LaneId];
			int Level = LevelEqual;
			std::string Name = "equal";
			if (!Lane.Stops.empty())
			{
				Level = LevelSignal;
				Name = "signal";
			}
			else if (bNodeHasSignal)
			{
				Level = LevelYield;
				Name = "yield";
			}
			else if (bRingNode)
			{
				Level = Lane.bRoundabout ? LevelPriority : LevelYield;
				Name = Lane.bRoundabout ? "priority" : "yield";
			}
			else if (SignControlOfLane.count(LaneId) > 0)
			{
				Level = LevelYield;
				Name = SignControlOfLane.at(LaneId);
			}
			else if (bNodeHasSign)
			{
				Level = LevelPriority;
				Name = "priority";
			}
			else if (TopCount >= 2 && Lowest < Top)
			{
				Level = Lane.Tier == Top ? LevelPriority : LevelYield;
				Name = Lane.Tier == Top ? "priority" : "yield";
			}
			Lane.Control = Name;
			Lane.ControlLevel = Level;
		}
		for (int ConnectionId : Connections)
		{
			FLane& Connection = Lanes[ConnectionId];
			const FLane& Source = Lanes[Connection.FromLane];
			Connection.Level = Source.ControlLevel;
			if (!Source.Stops.empty())
			{
				Connection.Phase = ApproachPhase[Source.Stops[0].Approach].second;
			}
		}
	}
	// Lanes with a signal stop need the control even where fewer than three arms meet (pedestrian crossings).
	for (FLane& Lane : Lanes)
	{
		if (Lane.Kind == ELaneKind::Road && !Lane.Stops.empty() && Lane.Control == "none")
		{
			Lane.Control = "signal";
		}
	}
}

std::optional<std::pair<bool, bool>> FLaneBuilder::YieldsTo(const FLane& A, const FLane& B) const
{
	if (A.Phase >= 0 && B.Phase >= 0 && A.Level == LevelSignal && B.Level == LevelSignal && A.Phase != B.Phase)
	{
		return std::nullopt;  // never green together
	}
	if (A.Level != B.Level)
	{
		return std::make_pair(A.Level < B.Level, true);
	}
	const bool bLeftA = A.Turn < 0;
	const bool bLeftB = B.Turn < 0;
	if (bLeftA != bLeftB)
	{
		return std::make_pair(bLeftA, true);  // a left turn gives way to oncoming straight and right turns
	}
	const FStreetPoint DirectionA = EndDirection(Lanes[A.FromLane], true);
	const FStreetPoint DirectionB = EndDirection(Lanes[B.FromLane], true);
	const double Side = Dot(Polyline::RightOf(DirectionA), -DirectionB);
	if (A.Level == LevelPriority && !(A.bRoundabout && B.bRoundabout))
	{
		return std::nullopt;
	}
	if (std::abs(Side) > 0.25)
	{
		return std::make_pair(Side > 0.0, false);  // right before left: b comes from a's right
	}
	return std::make_pair(A.Id > B.Id, false);
}

void FLaneBuilder::FindConflicts()
{
	for (const auto& [Node, Connections] : NodeConnections)
	{
		if (Connections.size() < 2)
		{
			continue;
		}
		std::vector<std::vector<double>> Profiles;
		for (int ConnectionId : Connections)
		{
			Profiles.push_back(Polyline::Arclength(Lanes[ConnectionId].Points));
		}
		for (size_t First = 0; First < Connections.size(); ++First)
		{
			for (size_t Second = First + 1; Second < Connections.size(); ++Second)
			{
				FLane& A = Lanes[Connections[First]];
				FLane& B = Lanes[Connections[Second]];
				if (A.FromLane == B.FromLane)
				{
					continue;
				}
				int FirstRow = -1;
				int LastRow = -1;
				int FirstColumn = -1;
				int LastColumn = -1;
				for (size_t Row = 0; Row < A.Points.size(); ++Row)
				{
					for (size_t Column = 0; Column < B.Points.size(); ++Column)
					{
						const double Distance = std::hypot(A.Points[Row].X - B.Points[Column].X,
														   A.Points[Row].Y - B.Points[Column].Y);
						if (Distance >= ConflictDistance)
						{
							continue;
						}
						FirstRow = FirstRow < 0 ? static_cast<int>(Row) : FirstRow;
						LastRow = static_cast<int>(Row);
						FirstColumn = FirstColumn < 0 ? static_cast<int>(Column) : std::min(FirstColumn, static_cast<int>(Column));
						LastColumn = std::max(LastColumn, static_cast<int>(Column));
					}
				}
				if (FirstRow < 0)
				{
					continue;
				}
				const std::vector<double>& AlongA = Profiles[First];
				const std::vector<double>& AlongB = Profiles[Second];
				const double AFrom = std::max(AlongA[FirstRow] - ConflictMargin, 0.0);
				const double ATo = std::min(AlongA[LastRow] + ConflictMargin, AlongA.back());
				const double BFrom = std::max(AlongB[FirstColumn] - ConflictMargin, 0.0);
				const double BTo = std::min(AlongB[LastColumn] + ConflictMargin, AlongB.back());
				const auto Relation = YieldsTo(A, B);
				if (!Relation.has_value())
				{
					continue;
				}
				const int Kind = Relation->second ? 2 : 1;  // 0 would be: does not give way
				A.Conflicts.push_back({B.Id, AFrom, ATo, BFrom, BTo, Relation->first ? Kind : 0});
				B.Conflicts.push_back({A.Id, BFrom, BTo, AFrom, ATo, Relation->first ? 0 : Kind});
			}
		}
	}
}

int FLaneBuilder::MarkGoodLanes()
{
	// Iterative Tarjan: lanes in the largest strongly connected component stay on routes and spawns.
	const int Count = static_cast<int>(Lanes.size());
	std::vector<int> Index(Count, -1);
	std::vector<int> LowLink(Count, 0);
	std::vector<int> Component(Count, -1);
	std::vector<bool> OnStack(Count, false);
	std::vector<int> Stack;
	std::vector<int> ComponentSizes;
	int Counter = 0;
	for (int Root = 0; Root < Count; ++Root)
	{
		if (Index[Root] >= 0)
		{
			continue;
		}
		std::vector<std::pair<int, size_t>> Work = {{Root, 0}};
		Index[Root] = LowLink[Root] = Counter++;
		Stack.push_back(Root);
		OnStack[Root] = true;
		while (!Work.empty())
		{
			auto& [Node, NextIndex] = Work.back();
			if (NextIndex < Lanes[Node].Next.size())
			{
				const int Target = Lanes[Node].Next[NextIndex++];
				if (Index[Target] < 0)
				{
					Index[Target] = LowLink[Target] = Counter++;
					Stack.push_back(Target);
					OnStack[Target] = true;
					Work.push_back({Target, 0});
				}
				else if (OnStack[Target])
				{
					LowLink[Node] = std::min(LowLink[Node], Index[Target]);
				}
				continue;
			}
			const int Finished = Node;
			Work.pop_back();
			if (LowLink[Finished] == Index[Finished])
			{
				int Size = 0;
				int Member = -1;
				while (Member != Finished)
				{
					Member = Stack.back();
					Stack.pop_back();
					OnStack[Member] = false;
					Component[Member] = static_cast<int>(ComponentSizes.size());
					++Size;
				}
				ComponentSizes.push_back(Size);
			}
			if (!Work.empty())
			{
				LowLink[Work.back().first] = std::min(LowLink[Work.back().first], LowLink[Finished]);
			}
		}
	}
	if (ComponentSizes.empty())
	{
		return 0;
	}
	const int Main = static_cast<int>(std::max_element(ComponentSizes.begin(), ComponentSizes.end()) - ComponentSizes.begin());
	for (FLane& Lane : Lanes)
	{
		Lane.bGood = Component[Lane.Id] == Main;
	}
	return ComponentSizes[Main];
}

FLaneGraph FLaneBuilder::Build()
{
	for (const FStreetWay& Way : *Model.GroundWays)
	{
		Ways.push_back(&Way);
	}
	for (const FStreetWay& Way : Model.BridgeWays)
	{
		Ways.push_back(&Way);
	}
	for (size_t Index = 0; Index < Graph.Ways.size(); ++Index)
	{
		GraphWayOfId[Graph.Ways[Index].Id] = static_cast<int>(Index);
	}
	BuildSegments();
	BuildRoadLanes();
	BuildConnections();
	AssignControls();
	FindConflicts();
	FLaneGraph Result;
	Result.GoodCount = MarkGoodLanes();
	for (const FStreetWay& Way : Model.BridgeWays)
	{
		Result.Bridges.push_back({Way.Id, Way.Points, 0.0, 0.0});
	}
	Result.Lanes = std::move(Lanes);
	return Result;
}
}

std::vector<double> FLane::Arclength() const
{
	return Polyline::Arclength(Points);
}

double FLane::Length() const
{
	return Polyline::Length(Points);
}

FLaneGraph BuildLaneGraph(const FStreetModel& Model, const FRoadGraph& Graph, const FFurniture& Furniture,
						  const FBox& Area)
{
	FLaneBuilder Builder(Model, Graph, Furniture, Area);
	return Builder.Build();
}

std::vector<FStreetPoint> LaneHeightQueries(const FLaneGraph& Lanes)
{
	std::vector<FStreetPoint> Queries;
	for (const FBridgeRamp& Bridge : Lanes.Bridges)
	{
		Queries.push_back(Bridge.Line.front());
		Queries.push_back(Bridge.Line.back());
	}
	for (const FLane& Lane : Lanes.Lanes)
	{
		Queries.insert(Queries.end(), Lane.Points.begin(), Lane.Points.end());
	}
	return Queries;
}

void ApplyLaneHeights(FLaneGraph& Lanes, const std::vector<double>& Heights)
{
	size_t Next = 0;
	std::unordered_map<int64_t, const FBridgeRamp*> RampOfWay;
	for (FBridgeRamp& Bridge : Lanes.Bridges)
	{
		Bridge.StartHeight = Heights[Next++];
		Bridge.EndHeight = Heights[Next++];
		RampOfWay[Bridge.Way] = &Bridge;
	}
	for (FLane& Lane : Lanes.Lanes)
	{
		Lane.Heights.assign(Heights.begin() + Next, Heights.begin() + Next + Lane.Points.size());
		Next += Lane.Points.size();
		const auto Ramp = RampOfWay.find(Lane.Way);
		if (Ramp == RampOfWay.end())
		{
			continue;
		}
		const FBridgeRamp& Bridge = *Ramp->second;
		const double Length = Polyline::Length(Bridge.Line);
		for (size_t Point = 0; Point < Lane.Points.size(); ++Point)
		{
			const double T = Polyline::ProjectOnPolyline(Bridge.Line, Lane.Points[Point]).first / std::max(Length, 1e-6);
			Lane.Heights[Point] = Bridge.StartHeight + (Bridge.EndHeight - Bridge.StartHeight) * std::min(std::max(T, 0.0), 1.0);
		}
	}
}

void AssignLaneHeights(FLaneGraph& Lanes, const std::function<double(double X, double Y)>& RoadHeight)
{
	std::vector<double> Heights;
	for (const FStreetPoint& Point : LaneHeightQueries(Lanes))
	{
		Heights.push_back(RoadHeight(Point.X, Point.Y));
	}
	ApplyLaneHeights(Lanes, Heights);
}

namespace
{
void AppendInts(std::string& Text, const std::vector<int>& Values)
{
	Text += "[";
	for (size_t Index = 0; Index < Values.size(); ++Index)
	{
		Text += (Index > 0 ? "," : "") + std::to_string(Values[Index]);
	}
	Text += "]";
}

/**
 * Appends a number rounded to three decimals the way Python prints it. A value that is a multiple of 0.001 prints as
 * exactly those digits, so the integer form is used where the magnitude allows and the general writer elsewhere.
 */
void AppendRounded3(std::string& Text, double Value)
{
	const double Rounded = NumpyRound(Value, 3);
	if (std::abs(Rounded) >= 1e9 || Rounded == 0.0)
	{
		Text += PythonDouble(Rounded);
		return;
	}
	long long Thousandths = std::llround(Rounded * 1000.0);
	if (Thousandths < 0)
	{
		Text += "-";
		Thousandths = -Thousandths;
	}
	Text += std::to_string(Thousandths / 1000);
	Text += ".";
	const int Fraction = static_cast<int>(Thousandths % 1000);
	if (Fraction == 0)
	{
		Text += "0";
		return;
	}
	char Digits[4] = {static_cast<char>('0' + Fraction / 100), static_cast<char>('0' + Fraction / 10 % 10),
					  static_cast<char>('0' + Fraction % 10), '\0'};
	if (Digits[2] == '0')
	{
		Digits[2] = '\0';
		if (Digits[1] == '0')
		{
			Digits[1] = '\0';
		}
	}
	Text += Digits;
}

void AppendLane(std::string& Text, const FLane& Lane)
{
	Text += "{\"id\":" + std::to_string(Lane.Id) + ",\"kind\":" + std::to_string(static_cast<int>(Lane.Kind))
		+ ",\"way\":" + std::to_string(Lane.Way) + ",\"limit\":" + PythonDouble(Lane.LimitKmh)
		+ ",\"tier\":" + std::to_string(Lane.Tier) + ",\"pts\":[";
	for (size_t Point = 0; Point < Lane.Points.size(); ++Point)
	{
		Text += Point > 0 ? "," : "";
		AppendRounded3(Text, Lane.Points[Point].X);
		Text += ",";
		AppendRounded3(Text, Lane.Points[Point].Y);
		Text += ",";
		AppendRounded3(Text, Lane.Heights[Point]);
	}
	Text += "],\"next\":";
	AppendInts(Text, Lane.Next);
	Text += ",\"good\":" + std::string(Lane.bGood ? "1" : "0");
	if (Lane.bRoundabout)
	{
		Text += ",\"ring\":1";
	}
	std::vector<int> Speeds;
	for (double Speed : Lane.CurveKmh)
	{
		Speeds.push_back(static_cast<int>(std::nearbyint(Speed)));
	}
	if (*std::min_element(Speeds.begin(), Speeds.end()) < Lane.LimitKmh - 1)
	{
		Text += ",\"vc\":";
		AppendInts(Text, Speeds);
	}
	if (Lane.Kind == ELaneKind::Road)
	{
		Text += ",\"ctl\":\"" + Lane.Control + "\"";
		if (!Lane.Stops.empty())
		{
			Text += ",\"stops\":[";
			for (size_t Stop = 0; Stop < Lane.Stops.size(); ++Stop)
			{
				Text += (Stop > 0 ? "," : "") + std::string("[") + PythonDouble(PythonRound(Lane.Stops[Stop].S, 2)) + ","
					+ std::to_string(Lane.Stops[Stop].Approach) + "]";
			}
			Text += "]";
		}
	}
	else
	{
		Text += ",\"from\":" + std::to_string(Lane.FromLane) + ",\"to\":" + std::to_string(Lane.ToLane)
			+ ",\"turn\":" + std::to_string(Lane.Turn) + ",\"level\":" + std::to_string(Lane.Level);
		if (!Lane.Conflicts.empty())
		{
			Text += ",\"conf\":[";
			for (size_t Conflict = 0; Conflict < Lane.Conflicts.size(); ++Conflict)
			{
				const FLaneConflict& Item = Lane.Conflicts[Conflict];
				Text += (Conflict > 0 ? "," : "") + std::string("[") + std::to_string(Item.Other) + ","
					+ PythonDouble(PythonRound(Item.OwnFrom, 2)) + "," + PythonDouble(PythonRound(Item.OwnTo, 2)) + ","
					+ PythonDouble(PythonRound(Item.OtherFrom, 2)) + "," + PythonDouble(PythonRound(Item.OtherTo, 2)) + ","
					+ std::to_string(Item.Yields) + "]";
			}
			Text += "]";
		}
	}
	Text += "}";
}
}

std::string LanesJsonText(const FLaneGraph& Lanes)
{
	std::string Text = "{\"version\":1,\"lanes\":[";
	for (size_t Index = 0; Index < Lanes.Lanes.size(); ++Index)
	{
		Text += Index > 0 ? "," : "";
		AppendLane(Text, Lanes.Lanes[Index]);
	}
	Text += "]}";
	return Text;
}

bool WriteLanesJson(const std::string& Path, const FLaneGraph& Lanes)
{
	std::ofstream File(Path, std::ios::binary);
	if (!File)
	{
		return false;
	}
	File << LanesJsonText(Lanes);
	return static_cast<bool>(File);
}
}
