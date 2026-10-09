#include "RoadSurfaces.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <thread>
#include <unordered_map>

#include "OsmTags.h"
#include "Terrain.h"

namespace WorldBuilder
{
namespace
{
/** Kerb radius at junction corners (assumptions.CORNER_RADIUS, roads.py's FILLET_RADIUS). */
constexpr double FilletRadius = 4.0;
constexpr double PavementWidth = 2.5;
/** Painted lines stay this far inside the road edge, and shorter pieces than this are dropped. */
constexpr double MarkingInset = 0.1;
constexpr double MinimumMarkingLength = 2.0;
/** The window around a point in which its road height is smoothed (four sigma of the 4 m Gaussian, and more). */
constexpr double RoadHeightReach = 24.0;
/** Chunks the road surface is inset in, so a marking only unions the few chunks it crosses. */
constexpr double InsetChunk = 250.0;
constexpr double InsetChunkMargin = 1.0;

const std::set<std::string> PavementClasses = {"primary", "secondary", "tertiary", "residential", "unclassified",
											   "living_street", "primary_link", "secondary_link", "tertiary_link"};
const std::set<std::string> CobbleSurfaces = {"sett", "cobblestone", "unhewn_cobblestone", "cobblestone:flattened"};
const std::set<std::string> PaverSurfaces = {"paving_stones", "paving_stones:30", "concrete:plates", "grass_paver"};

FPolyline ToWorld(const FStreetPolyline& Line)
{
	FPolyline Points;
	Points.reserve(Line.size());
	for (const FStreetPoint& Point : Line)
	{
		Points.push_back({Point.X, Point.Y});
	}
	return Points;
}

/** "asphalt", "pavers" or "cobble" from a way's surface tag. */
std::string SurfaceKind(const FTags& Tags)
{
	const std::string Surface = OsmTags::ValueOrEmpty(Tags, "surface");
	if (CobbleSurfaces.count(Surface))
	{
		return "cobble";
	}
	if (PaverSurfaces.count(Surface))
	{
		return "pavers";
	}
	return "asphalt";
}

/** A disc as a polygon with SegmentsPerQuarter segments per quarter circle. */
FPolygons Disc(double X, double Y, double Radius, int SegmentsPerQuarter)
{
	Clipper2Lib::PathD Ring;
	const int Count = 4 * SegmentsPerQuarter;
	for (int Index = 0; Index < Count; ++Index)
	{
		const double Angle = 2.0 * 3.14159265358979323846 * Index / Count;
		Ring.emplace_back(X + Radius * std::cos(Angle), Y + Radius * std::sin(Angle));
	}
	return {Ring};
}

/**
 * A segment's carriageway: a round-capped band of its width, or where it tapers, the polygon between its tapered
 * kerbs closed with a disc of the width at each end (which fills the outer side of bends at the node).
 */
FPolygons SegmentSurface(const FRoadLines& Lines, const FSegmentLayout& Layout)
{
	const FSegment& Segment = *Layout.Segment;
	const FPolyline Centre = ToWorld(Segment.Xy);
	const FKerbs Kerbs = Lines.Kerbs(Layout);
	const bool bTapered = Layout.Start.Taper > 0.0 || Layout.End.Taper > 0.0;
	if (!bTapered || !Kerbs.Left || !Kerbs.Right)
	{
		return BufferPolyline(Centre, Segment.Section.Width() / 2.0, ECapStyle::Round, 4);
	}
	Clipper2Lib::PathD Band;
	for (const FStreetPoint& Point : *Kerbs.Left)
	{
		Band.emplace_back(Point.X, Point.Y);
	}
	for (auto Point = Kerbs.Right->rbegin(); Point != Kerbs.Right->rend(); ++Point)
	{
		Band.emplace_back(Point->X, Point->Y);
	}
	// Even-odd keeps both lobes of a band that crosses itself, as shapely's make_valid does.
	FPolygons Pieces = Clipper2Lib::Union({Band}, Clipper2Lib::FillRule::EvenOdd, 4);
	for (const FPolygons& Cap : {Disc(Centre.front().X, Centre.front().Y, Layout.WidthAtEnd(true) / 2.0, 4),
								 Disc(Centre.back().X, Centre.back().Y, Layout.WidthAtEnd(false) / 2.0, 4)})
	{
		Pieces.insert(Pieces.end(), Cap.begin(), Cap.end());
	}
	return UnionOf(Pieces);
}

/** A polyline moved sideways: positive to the left in the coordinates' own sense, as shapely's offset_curve. */
FPolyline OffsetPolylineLeft(const FPolyline& Line, double Distance)
{
	FPolyline Moved;
	for (size_t Index = 0; Index < Line.size(); ++Index)
	{
		const FWorldPoint& Before = Line[Index == 0 ? 0 : Index - 1];
		const FWorldPoint& After = Line[Index + 1 < Line.size() ? Index + 1 : Index];
		double DirectionX = After.X - Before.X;
		double DirectionY = After.Y - Before.Y;
		const double Length = std::hypot(DirectionX, DirectionY);
		if (Length < 1e-9)
		{
			continue;
		}
		DirectionX /= Length;
		DirectionY /= Length;
		Moved.push_back({Line[Index].X - DirectionY * Distance, Line[Index].Y + DirectionX * Distance});
	}
	return Moved;
}

/** The pavement band of an urban way: both sides, or the tagged one (roads.py's _pavements). */
FPolygons PavementBand(const FStreetWay& Way, double Width)
{
	const FPolyline Line = ToWorld(Way.Points);
	std::string Sidewalk = OsmTags::ValueOrEmpty(Way.Tags, "sidewalk");
	if (!OsmTags::HasTag(Way.Tags, "sidewalk"))
	{
		Sidewalk = OsmTags::ValueOrEmpty(Way.Tags, "sidewalk:both");
	}
	if (Sidewalk == "left" || Sidewalk == "right")
	{
		const double Side = Sidewalk == "left" ? 1.0 : -1.0;
		const FPolyline Middle = OffsetPolylineLeft(Line, Side * (Width / 2.0 + PavementWidth / 2.0));
		return BufferPolyline(Middle, PavementWidth / 2.0 + 0.3, ECapStyle::Flat, 8);
	}
	return BufferPolyline(Line, Width / 2.0 + PavementWidth, ECapStyle::Flat, 8);
}

bool HasPavement(const FStreetWay& Way, bool bUrban)
{
	if (!PavementClasses.count(OsmTags::ValueOrEmpty(Way.Tags, "highway")) || !bUrban)
	{
		return false;
	}
	std::string Sidewalk = OsmTags::ValueOrEmpty(Way.Tags, "sidewalk");
	if (!OsmTags::HasTag(Way.Tags, "sidewalk"))
	{
		Sidewalk = OsmTags::ValueOrEmpty(Way.Tags, "sidewalk:both");
	}
	return Sidewalk != "no" && Sidewalk != "none";
}

/** Discs around every junction node (three or more road ends), sized by the widest road there. */
std::vector<FPolygons> JunctionZones(const std::vector<FStreetWay>& Ways, const std::unordered_map<int64_t, double>& Widths)
{
	std::unordered_map<int64_t, int> Degree;
	std::unordered_map<int64_t, FStreetPoint> Position;
	std::unordered_map<int64_t, double> WidestWay;
	for (const FStreetWay& Way : Ways)
	{
		const auto Width = Widths.find(Way.Id);
		const double WayWidth = Width != Widths.end() ? Width->second : 6.0;
		const size_t Last = Way.NodeIds.size() - 1;
		for (size_t Index = 0; Index < Way.NodeIds.size(); ++Index)
		{
			const int64_t Node = Way.NodeIds[Index];
			Degree[Node] += Index == 0 || Index == Last ? 1 : 2;
			Position[Node] = Way.Points[Index];
			WidestWay[Node] = std::max(WidestWay.count(Node) ? WidestWay[Node] : 0.0, WayWidth);
		}
	}
	std::vector<FPolygons> Zones;
	for (const auto& [Node, Count] : Degree)
	{
		if (Count < 3)
		{
			continue;
		}
		const double Radius = WidestWay[Node] * 0.6 + 3.0;
		Zones.push_back(Disc(Position[Node].X, Position[Node].Y, Radius, 6));
	}
	return Zones;
}

/** The kerb radii inside one junction zone: what closing the road surface around it adds inside the zone. */
FPolygons Fillet(const FPolygonSet& Surface, const FPolygons& Zone)
{
	const FBox ZoneBounds = BoundsOf(Zone);
	const double Reach = 3.0 * FilletRadius;
	const FBox Window{ZoneBounds.X0 - Reach, ZoneBounds.Y0 - Reach, ZoneBounds.X1 + Reach, ZoneBounds.Y1 + Reach};
	const FPolygons Local = Surface.InWindow(Window);
	if (Local.empty())
	{
		return {};
	}
	const FPolygons Closed = OffsetPolygons(OffsetPolygons(Local, FilletRadius, 4), -FilletRadius, 4);
	return Intersection(Difference(Closed, Local), Zone);
}

/** Runs Work(Index) for every index on worker threads. */
template <typename FWork>
void ParallelFor(size_t Count, unsigned Threads, FWork&& Work)
{
	std::atomic<size_t> Next{0};
	std::vector<std::thread> Workers;
	for (unsigned Worker = 0; Worker < std::max(1u, Threads); ++Worker)
	{
		Workers.emplace_back([&]() {
			for (size_t Index = Next++; Index < Count; Index = Next++)
			{
				Work(Index);
			}
		});
	}
	for (std::thread& Worker : Workers)
	{
		Worker.join();
	}
}

/** The road surface shrunk by the marking inset, in overlapping chunks (union of all equals the inset surface). */
FPolygonSet InsetGround(const FPolygonSet& Ground, unsigned Threads)
{
	FBox Bounds{1e300, 1e300, -1e300, -1e300};
	for (const FPolygons& Piece : Ground.All())
	{
		const FBox Box = BoundsOf(Piece);
		Bounds = {std::min(Bounds.X0, Box.X0), std::min(Bounds.Y0, Box.Y0), std::max(Bounds.X1, Box.X1), std::max(Bounds.Y1, Box.Y1)};
	}
	FPolygonSet Result(InsetChunk);
	if (Ground.Size() == 0)
	{
		return Result;
	}
	const int64_t FirstColumn = static_cast<int64_t>(std::floor(Bounds.X0 / InsetChunk));
	const int64_t FirstRow = static_cast<int64_t>(std::floor(Bounds.Y0 / InsetChunk));
	const int64_t Columns = static_cast<int64_t>(std::floor(Bounds.X1 / InsetChunk)) - FirstColumn + 1;
	const int64_t Rows = static_cast<int64_t>(std::floor(Bounds.Y1 / InsetChunk)) - FirstRow + 1;
	std::vector<FPolygons> Chunks(static_cast<size_t>(Columns * Rows));
	ParallelFor(Chunks.size(), Threads, [&](size_t Index) {
		const double X0 = (FirstColumn + static_cast<int64_t>(Index) % Columns) * InsetChunk;
		const double Y0 = (FirstRow + static_cast<int64_t>(Index) / Columns) * InsetChunk;
		// The margin covers the chunk's cut edges, which the inset moves inward by the inset.
		const FBox Window{X0 - InsetChunkMargin, Y0 - InsetChunkMargin, X0 + InsetChunk + InsetChunkMargin,
						  Y0 + InsetChunk + InsetChunkMargin};
		const FPolygons Local = Ground.InWindow(Window);
		if (!Local.empty())
		{
			Chunks[Index] = OffsetPolygons(Local, -MarkingInset, 8);
		}
	});
	for (FPolygons& Chunk : Chunks)
	{
		Result.Add(std::move(Chunk));
	}
	return Result;
}

/** A clipped piece of line in the original's direction (Clipper may return it reversed). */
FPolyline Oriented(FPolyline Piece, const FPolyline& Original)
{
	if (Piece.size() < 2 || Original.size() < 2)
	{
		return Piece;
	}
	const double OriginalX = Original.back().X - Original.front().X;
	const double OriginalY = Original.back().Y - Original.front().Y;
	const double PieceX = Piece.back().X - Piece.front().X;
	const double PieceY = Piece.back().Y - Piece.front().Y;
	if (OriginalX * PieceX + OriginalY * PieceY < 0.0)
	{
		std::reverse(Piece.begin(), Piece.end());
	}
	return Piece;
}

FBox BoxOf(const FPolyline& Line)
{
	FBox Box{1e300, 1e300, -1e300, -1e300};
	for (const FWorldPoint& Point : Line)
	{
		Box = {std::min(Box.X0, Point.X), std::min(Box.Y0, Point.Y), std::max(Box.X1, Point.X), std::max(Box.Y1, Point.Y)};
	}
	return Box;
}
}

double RoadHeightAt(const FTerrainGrid& Terrain, const FPolygonSet& Ground, double X, double Y)
{
	const FBox Window{std::floor(X) - RoadHeightReach, std::floor(Y) - RoadHeightReach, std::floor(X) + RoadHeightReach + 1.0,
					  std::floor(Y) + RoadHeightReach + 1.0};
	const FHeightGrid Local = Terrain.FineWindow(Window.X0, Window.Y0, Window.X1, Window.Y1);
	return RoadHeightField(Local, Ground.InWindow(Window)).Sample(X, Y);
}

FRoadSurfaces::FRoadSurfaces(const FStreetModel& Model, const FTerrainGrid& Terrain, const FPolygonSet& Buildings,
							 unsigned Threads)
	: BuildingPieces(&Buildings)
{
	// Each way's carriageway from its segments, sorted into the material sets.
	std::unordered_map<int64_t, FPolygons> WaySurfaces;
	for (const FSegmentLayout& Layout : Model.Lines.Layouts)
	{
		const FPolygons Surface = SegmentSurface(Model.Lines, Layout);
		FPolygons& Target = WaySurfaces[Layout.Segment->Way->Id];
		Target.insert(Target.end(), Surface.begin(), Surface.end());
	}
	FPolygonSet Carriageways;
	for (const FStreetWay& Way : *Model.GroundWays)
	{
		auto Surface = WaySurfaces.find(Way.Id);
		if (Surface == WaySurfaces.end())
		{
			continue;
		}
		const FPolygons Merged = UnionOf(Surface->second);
		const std::string Kind = SurfaceKind(Way.Tags);
		if (Kind == "pavers")
		{
			PaverPieces.Add(Merged);
		}
		else if (Kind == "cobble")
		{
			CobblePieces.Add(Merged);
		}
		else
		{
			AsphaltPieces.Add(Merged);
		}
		Carriageways.Add(Merged);
		GroundPieces.Add(Merged);
	}
	for (const FGore& Gore : Model.Lines.Gores)
	{
		Clipper2Lib::PathD Ring;
		for (const FStreetPoint& Point : Gore.Paved)
		{
			Ring.emplace_back(Point.X, Point.Y);
		}
		const FPolygons Paved = Clipper2Lib::Union({Ring}, Clipper2Lib::FillRule::EvenOdd, 4);
		Carriageways.Add(Paved);
		GroundPieces.Add(Paved);
	}
	// Kerb radii only inside junctions: along the road the same closing would pave over narrow medians and islands.
	const std::vector<FPolygons> Zones = JunctionZones(*Model.GroundWays, Model.Widths);
	std::vector<FPolygons> Fillets(Zones.size());
	ParallelFor(Zones.size(), Threads, [&](size_t Index) { Fillets[Index] = Fillet(Carriageways, Zones[Index]); });
	for (FPolygons& Piece : Fillets)
	{
		GroundPieces.Add(std::move(Piece));
	}

	for (const FStreetWay& Way : *Model.GroundWays)
	{
		const auto Urban = Model.Urban.find(Way.Id);
		if (HasPavement(Way, Urban != Model.Urban.end() && Urban->second))
		{
			PavementBands.Add(PavementBand(Way, Model.Widths.at(Way.Id)));
		}
	}

	for (const FStreetWay& Way : Model.BridgeWays)
	{
		FBridgeDeck& Deck = Bridges.emplace_back();
		const FPolyline Line = ToWorld(Way.Points);
		Deck.Polygon = BufferPolyline(Line, Model.Widths.at(Way.Id) / 2.0, ECapStyle::Flat, 8);
		Deck.Start = Line.front();
		Deck.End = Line.back();
	}
	ParallelFor(Bridges.size(), Threads, [&](size_t Index) {
		FBridgeDeck& Deck = Bridges[Index];
		Deck.StartHeight = RoadHeightAt(Terrain, GroundPieces, Deck.Start.X, Deck.Start.Y);
		Deck.EndHeight = RoadHeightAt(Terrain, GroundPieces, Deck.End.X, Deck.End.Y);
	});
	for (size_t Index = 0; Index < Bridges.size(); ++Index)
	{
		BridgeIndex.Insert(BoundsOf(Bridges[Index].Polygon), static_cast<int>(Index));
	}

	// Painted lines kept on the road: their parts inside the slightly shrunk surface, longer than 2 m.
	const FPolygonSet Inset = InsetGround(GroundPieces, Threads);
	std::vector<std::vector<FMarkingLine>> Clipped(Model.Markings.size());
	ParallelFor(Model.Markings.size(), Threads, [&](size_t Index) {
		const FMarking& Marking = Model.Markings[Index];
		if (Marking.Points.size() < 2)
		{
			return;
		}
		const FPolyline Line = ToWorld(Marking.Points);
		for (FPolyline& Piece : Inset.ClipLine(Line))
		{
			if (PolylineLength(Piece) > MinimumMarkingLength)
			{
				Clipped[Index].push_back({Marking.Kind, Oriented(std::move(Piece), Line)});
			}
		}
	});
	for (std::vector<FMarkingLine>& Pieces : Clipped)
	{
		for (FMarkingLine& Piece : Pieces)
		{
			MarkingIndex.Insert(BoxOf(Piece.Points), static_cast<int>(MarkingLines.size()));
			MarkingLines.push_back(std::move(Piece));
		}
	}
}

FWindowRoads FRoadSurfaces::InWindow(const FBox& Window) const
{
	FWindowRoads Roads;
	Roads.Ground = GroundPieces.InWindow(Window);
	const FPolygons Bands = PavementBands.InWindow(Window);
	if (!Bands.empty())
	{
		const FPolygons NearBuildings = OffsetPolygons(BuildingPieces->InWindow(Window), 0.2, 8);
		const FPolygons Raw = Difference(Difference(Bands, Roads.Ground), NearBuildings);
		// Drops slivers narrower than 0.6 m.
		Roads.Pavement = OffsetPolygons(OffsetPolygons(Raw, -0.3, 8), 0.3, 8);
	}
	if (Roads.Ground.empty())
	{
		return Roads;
	}
	// Asphalt roads win where they meet paved or cobbled ones; whatever else is on the road is asphalt too.
	const FPolygons AsphaltRoads = AsphaltPieces.InWindow(Window);
	Roads.Pavers = Difference(PaverPieces.InWindow(Window), AsphaltRoads);
	Roads.Cobble = Difference(Difference(CobblePieces.InWindow(Window), AsphaltRoads), Roads.Pavers);
	FPolygons Claimed = Roads.Pavers;
	Claimed.insert(Claimed.end(), Roads.Cobble.begin(), Roads.Cobble.end());
	Roads.Asphalt = Difference(Roads.Ground, Claimed);
	return Roads;
}

std::vector<const FBridgeDeck*> FRoadSurfaces::BridgesNear(const FBox& Box) const
{
	std::vector<const FBridgeDeck*> Result;
	for (const int Item : BridgeIndex.Query(Box))
	{
		Result.push_back(&Bridges[Item]);
	}
	return Result;
}

std::vector<const FMarkingLine*> FRoadSurfaces::MarkingsNear(const FBox& Box) const
{
	std::vector<const FMarkingLine*> Result;
	for (const int Item : MarkingIndex.Query(Box))
	{
		Result.push_back(&MarkingLines[Item]);
	}
	return Result;
}
}

namespace WorldBuilder
{
namespace
{
/** The point at a distance along a polyline, clamped to its ends. */
FWorldPoint PointAlong(const FPolyline& Line, double Distance)
{
	double Remaining = std::max(Distance, 0.0);
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		const double Length = std::hypot(Line[Index + 1].X - Line[Index].X, Line[Index + 1].Y - Line[Index].Y);
		if (Remaining <= Length && Length > 0.0)
		{
			const double Along = Remaining / Length;
			return {Line[Index].X + (Line[Index + 1].X - Line[Index].X) * Along,
					Line[Index].Y + (Line[Index + 1].Y - Line[Index].Y) * Along};
		}
		Remaining -= Length;
	}
	return Line.back();
}
}

FStartPose FindStart(const FStreetModel& Model, const FTerrainGrid& Terrain, const FPolygonSet& Ground)
{
	static const std::set<std::string> StartClasses = {"residential", "tertiary", "secondary", "unclassified"};
	const FStreetWay* Best = nullptr;
	double BestDistance = 0.0;
	for (const FStreetWay& Way : *Model.GroundWays)
	{
		const FPolyline Line = ToWorld(Way.Points);
		const double Length = PolylineLength(Line);
		if (!StartClasses.count(OsmTags::ValueOrEmpty(Way.Tags, "highway")) || Length < 60.0)
		{
			continue;
		}
		const FWorldPoint Middle = PointAlong(Line, Length * 0.5);
		const double Distance = std::hypot(Middle.X, Middle.Y);
		if (Best == nullptr || Distance < BestDistance)
		{
			Best = &Way;
			BestDistance = Distance;
		}
	}
	FStartPose Pose;
	if (Best == nullptr)
	{
		Pose.Z = RoadHeightAt(Terrain, Ground, 0.0, 0.0);
		return Pose;
	}
	const FPolyline Line = ToWorld(Best->Points);
	const double Middle = PolylineLength(Line) * 0.5;
	const FWorldPoint Here = PointAlong(Line, Middle);
	const FWorldPoint Ahead = PointAlong(Line, Middle + 5.0);
	const double DeltaX = Ahead.X - Here.X;
	const double DeltaY = Ahead.Y - Here.Y;
	const double Length = std::max(std::hypot(DeltaX, DeltaY), 1e-6);
	// Half a lane right of the centre line: right of travel in the y-south frame (build_area.find_start).
	const double Offset = Model.Widths.at(Best->Id) / 4.0;
	Pose.X = Here.X - DeltaY / Length * Offset;
	Pose.Y = Here.Y + DeltaX / Length * Offset;
	Pose.Z = RoadHeightAt(Terrain, Ground, Pose.X, Pose.Y);
	Pose.Yaw = std::atan2(DeltaY, DeltaX) * 180.0 / 3.14159265358979323846;
	Pose.Road = OsmTags::ValueOrEmpty(Best->Tags, "name");
	return Pose;
}
}
