#include "Roads.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "Assumptions.h"
#include "DualCarriageways.h"
#include "OsmTags.h"
#include "clipper2/clipper.h"

namespace WorldBuilder
{
namespace
{
constexpr double BuildingCellSize = 64.0;
constexpr double UrbanBuildingDistance = 35.0;
constexpr double UrbanMaxSpeed = 70.0;

int64_t CellIndex(double Coordinate)
{
	return static_cast<int64_t>(std::floor(Coordinate / BuildingCellSize));
}

int64_t CellKey(int64_t CellX, int64_t CellY)
{
	return CellX * 1000003 + CellY;
}

double Cross(const FStreetPoint& Origin, const FStreetPoint& First, const FStreetPoint& Second)
{
	return (First.X - Origin.X) * (Second.Y - Origin.Y) - (First.Y - Origin.Y) * (Second.X - Origin.X);
}

/** The distance between two segments: 0 where they cross, else the closest approach of an end point. */
double SegmentDistance(const FStreetPoint& FirstStart, const FStreetPoint& FirstEnd, const FStreetPoint& SecondStart,
					   const FStreetPoint& SecondEnd)
{
	const double A = Cross(FirstStart, FirstEnd, SecondStart);
	const double B = Cross(FirstStart, FirstEnd, SecondEnd);
	const double C = Cross(SecondStart, SecondEnd, FirstStart);
	const double D = Cross(SecondStart, SecondEnd, FirstEnd);
	if (((A > 0) != (B > 0)) && ((C > 0) != (D > 0)) && A != 0 && B != 0 && C != 0 && D != 0)
	{
		return 0.0;
	}
	return std::min({Polyline::DistanceToSegment(FirstStart, FirstEnd, SecondStart),
					 Polyline::DistanceToSegment(FirstStart, FirstEnd, SecondEnd),
					 Polyline::DistanceToSegment(SecondStart, SecondEnd, FirstStart),
					 Polyline::DistanceToSegment(SecondStart, SecondEnd, FirstEnd)});
}

/** True when the point lies inside the rings by the even-odd rule. */
bool IsInsideRings(const std::vector<FStreetPolyline>& Rings, const FStreetPoint& Point)
{
	bool bInside = false;
	for (const FStreetPolyline& Ring : Rings)
	{
		for (size_t Index = 0, Previous = Ring.size() - 1; Index < Ring.size(); Previous = Index++)
		{
			const FStreetPoint& Current = Ring[Index];
			const FStreetPoint& Before = Ring[Previous];
			if ((Current.Y > Point.Y) == (Before.Y > Point.Y))
			{
				continue;
			}
			const double CrossingX = (Before.X - Current.X) * (Point.Y - Current.Y) / (Before.Y - Current.Y) + Current.X;
			if (Point.X < CrossingX)
			{
				bInside = !bInside;
			}
		}
	}
	return bInside;
}
}

FBuildingIndex::FBuildingIndex(const std::vector<FOsmArea>& Buildings)
{
	for (const FOsmArea& Building : Buildings)
	{
		for (const FOsmPolygon& Source : Building.Polygons)
		{
			FPolygon Polygon;
			Polygon.MinX = Polygon.MinY = 1e300;
			Polygon.MaxX = Polygon.MaxY = -1e300;
			for (const std::vector<FWorldPoint>& SourceRing : Source.Rings)
			{
				FStreetPolyline Ring;
				for (const FWorldPoint& Point : SourceRing)
				{
					Ring.push_back({Point.X, Point.Y});
					Polygon.MinX = std::min(Polygon.MinX, Point.X);
					Polygon.MinY = std::min(Polygon.MinY, Point.Y);
					Polygon.MaxX = std::max(Polygon.MaxX, Point.X);
					Polygon.MaxY = std::max(Polygon.MaxY, Point.Y);
				}
				if (Ring.size() >= 3)
				{
					Polygon.Rings.push_back(std::move(Ring));
				}
			}
			if (Polygon.Rings.empty())
			{
				continue;
			}
			const int Id = static_cast<int>(Polygons.size());
			for (int64_t CellX = CellIndex(Polygon.MinX); CellX <= CellIndex(Polygon.MaxX); ++CellX)
			{
				for (int64_t CellY = CellIndex(Polygon.MinY); CellY <= CellIndex(Polygon.MaxY); ++CellY)
				{
					Cells[CellKey(CellX, CellY)].push_back(Id);
				}
			}
			Polygons.push_back(std::move(Polygon));
		}
	}
}

bool FBuildingIndex::IsEmpty() const
{
	return Polygons.empty();
}

bool FBuildingIndex::IsPolygonNear(const FPolygon& Polygon, const FStreetPolyline& Line, double Distance) const
{
	if (IsInsideRings(Polygon.Rings, Line.front()))
	{
		return true;
	}
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		const FStreetPoint& Start = Line[Index];
		const FStreetPoint& End = Line[Index + 1];
		const bool bFar = std::min(Start.X, End.X) - Distance > Polygon.MaxX
			|| std::max(Start.X, End.X) + Distance < Polygon.MinX
			|| std::min(Start.Y, End.Y) - Distance > Polygon.MaxY
			|| std::max(Start.Y, End.Y) + Distance < Polygon.MinY;
		if (bFar)
		{
			continue;
		}
		for (const FStreetPolyline& Ring : Polygon.Rings)
		{
			for (size_t Corner = 0, Previous = Ring.size() - 1; Corner < Ring.size(); Previous = Corner++)
			{
				if (SegmentDistance(Start, End, Ring[Previous], Ring[Corner]) <= Distance)
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool FBuildingIndex::IsNear(const FStreetPolyline& Line, double Distance) const
{
	std::vector<int> Candidates;
	for (size_t Index = 0; Index + 1 < Line.size(); ++Index)
	{
		const double MinX = std::min(Line[Index].X, Line[Index + 1].X) - Distance;
		const double MaxX = std::max(Line[Index].X, Line[Index + 1].X) + Distance;
		const double MinY = std::min(Line[Index].Y, Line[Index + 1].Y) - Distance;
		const double MaxY = std::max(Line[Index].Y, Line[Index + 1].Y) + Distance;
		for (int64_t CellX = CellIndex(MinX); CellX <= CellIndex(MaxX); ++CellX)
		{
			for (int64_t CellY = CellIndex(MinY); CellY <= CellIndex(MaxY); ++CellY)
			{
				const auto Cell = Cells.find(CellKey(CellX, CellY));
				if (Cell != Cells.end())
				{
					Candidates.insert(Candidates.end(), Cell->second.begin(), Cell->second.end());
				}
			}
		}
	}
	std::sort(Candidates.begin(), Candidates.end());
	Candidates.erase(std::unique(Candidates.begin(), Candidates.end()), Candidates.end());
	for (int Candidate : Candidates)
	{
		if (IsPolygonNear(Polygons[Candidate], Line, Distance))
		{
			return true;
		}
	}
	return false;
}

bool IsTunnel(const FStreetWay& Way)
{
	const std::string* Tunnel = FindTag(Way.Tags, "tunnel");
	const bool bInTunnel = Tunnel != nullptr && *Tunnel != "no" && *Tunnel != "building_passage";
	return bInTunnel || OsmTags::Number(FindTag(Way.Tags, "layer"), 0) < 0;
}

bool IsGround(const FStreetWay& Way)
{
	const std::string* Bridge = FindTag(Way.Tags, "bridge");
	const std::string* Tunnel = FindTag(Way.Tags, "tunnel");
	const bool bNoBridge = Bridge == nullptr || *Bridge == "no";
	const bool bNoTunnel = Tunnel == nullptr || *Tunnel == "no" || *Tunnel == "building_passage";
	return bNoBridge && OsmTags::Number(FindTag(Way.Tags, "layer"), 0) >= 0 && bNoTunnel;
}

bool IsUrban(const FStreetWay& Way, const FBuildingIndex& Buildings)
{
	if (Buildings.IsEmpty())
	{
		return true;
	}
	const std::optional<double> Speed = OsmTags::Number(FindTag(Way.Tags, "maxspeed"));
	if (Speed.has_value() && *Speed != 0.0 && *Speed >= UrbanMaxSpeed)
	{
		return false;
	}
	return Buildings.IsNear(Way.Points, UrbanBuildingDistance);
}

void TakeCorrectedSections(const FRoadLines& Lines, FSectionsByWay& Sections,
						   std::unordered_map<int64_t, double>& Widths)
{
	std::unordered_map<int64_t, std::vector<const FSegment*>> SegmentsOf;
	for (const FSegment& Segment : Lines.Network->Segments)
	{
		SegmentsOf[Segment.Way->Id].push_back(&Segment);
	}
	for (const auto& [WayId, Segments] : SegmentsOf)
	{
		if (Segments.size() == 1)
		{
			Sections[WayId] = Segments[0]->Section;
			Widths[WayId] = Segments[0]->Section.Width();
		}
	}
}

namespace
{
/**
 * The area of a ring by the even-odd rule, which is what make_valid keeps of a ring that crosses itself: each lobe
 * counts once, however it winds.
 */
double EvenOddArea(const FStreetPolyline& Ring)
{
	Clipper2Lib::PathD Path;
	for (const FStreetPoint& Point : Ring)
	{
		Path.emplace_back(Point.X, Point.Y);
	}
	const Clipper2Lib::PathsD Valid = Clipper2Lib::Union(Clipper2Lib::PathsD{Path}, Clipper2Lib::FillRule::EvenOdd, 4);
	return std::abs(Clipper2Lib::Area(Valid));
}

/**
 * The convex hull of the points, clockwise in a y-up frame and starting at the lowest point (smallest y, then smallest
 * x) like GEOS's rings, so that equal-area rectangles are tried in the same order (Andrew's monotone chain).
 */
FStreetPolyline ConvexHull(FStreetPolyline Points)
{
	std::sort(Points.begin(), Points.end(), [](const FStreetPoint& Left, const FStreetPoint& Right) {
		return Left.X < Right.X || (Left.X == Right.X && Left.Y < Right.Y);
	});
	Points.erase(std::unique(Points.begin(), Points.end()), Points.end());
	if (Points.size() < 3)
	{
		return Points;
	}
	FStreetPolyline Hull(2 * Points.size());
	size_t Count = 0;
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		while (Count >= 2 && Cross(Hull[Count - 2], Hull[Count - 1], Points[Index]) <= 0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index];
	}
	for (size_t Index = Points.size() - 1, Lower = Count + 1; Index > 0; --Index)
	{
		while (Count >= Lower && Cross(Hull[Count - 2], Hull[Count - 1], Points[Index - 1]) <= 0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index - 1];
	}
	Hull.resize(Count - 1);
	std::reverse(Hull.begin(), Hull.end());
	const auto Lowest = std::min_element(Hull.begin(), Hull.end(), [](const FStreetPoint& Left, const FStreetPoint& Right) {
		return Left.Y < Right.Y || (Left.Y == Right.Y && Left.X < Right.X);
	});
	std::rotate(Hull.begin(), Lowest, Hull.end());
	return Hull;
}

/**
 * The corners of the smallest rectangle around the points, or nothing when the points are degenerate. The rectangle
 * has a side on one of the hull's edges, and its corners run clockwise from the start of that side, as in GEOS. When
 * several edges give the same area (any acute triangle), GEOS picks one by rounding noise that can't be reproduced.
 */
std::optional<FStreetPolyline> MinimumRotatedRectangle(const FStreetPolyline& Points)
{
	const FStreetPolyline Hull = ConvexHull(Points);
	if (Hull.size() < 3)
	{
		return std::nullopt;
	}
	double BestArea = 1e300;
	FStreetPolyline Best;
	for (size_t Index = 0; Index < Hull.size(); ++Index)
	{
		const FStreetPoint Start = Hull[Index];
		const FStreetPoint Direction = Polyline::Unit(Hull[(Index + 1) % Hull.size()] - Start);
		const FStreetPoint Normal = {Direction.Y, -Direction.X};  // into the clockwise hull
		double MinAlong = 1e300;
		double MaxAlong = -1e300;
		double MaxAcross = -1e300;
		for (const FStreetPoint& Point : Hull)
		{
			const double Along = Dot(Point - Start, Direction);
			const double Across = Dot(Point - Start, Normal);
			MinAlong = std::min(MinAlong, Along);
			MaxAlong = std::max(MaxAlong, Along);
			MaxAcross = std::max(MaxAcross, Across);
		}
		const double Area = (MaxAlong - MinAlong) * MaxAcross;
		if (Area < BestArea)
		{
			BestArea = Area;
			Best = {Start + Direction * MinAlong, Start + Direction * MaxAlong,
					Start + Direction * MaxAlong + Normal * MaxAcross, Start + Direction * MinAlong + Normal * MaxAcross};
		}
	}
	return Best;
}

/**
 * The parts of a straight line inside a polygon by the even-odd rule, as start and end distances along the line from
 * its start; Direction is a unit vector.
 */
std::vector<std::pair<double, double>> InsideIntervals(const FStreetPolyline& Ring, const FStreetPoint& Start,
													   const FStreetPoint& Direction)
{
	std::vector<double> Crossings;
	for (size_t Index = 0, Previous = Ring.size() - 1; Index < Ring.size(); Previous = Index++)
	{
		const FStreetPoint& Current = Ring[Index];
		const FStreetPoint& Before = Ring[Previous];
		const double SideCurrent = Direction.X * (Current.Y - Start.Y) - Direction.Y * (Current.X - Start.X);
		const double SideBefore = Direction.X * (Before.Y - Start.Y) - Direction.Y * (Before.X - Start.X);
		if ((SideCurrent > 0) == (SideBefore > 0))
		{
			continue;
		}
		const double Fraction = SideBefore / (SideBefore - SideCurrent);
		const FStreetPoint Crossing = Before + (Current - Before) * Fraction;
		Crossings.push_back(Dot(Crossing - Start, Direction));
	}
	std::sort(Crossings.begin(), Crossings.end());
	std::vector<std::pair<double, double>> Intervals;
	for (size_t Index = 0; Index + 1 < Crossings.size(); Index += 2)
	{
		Intervals.emplace_back(Crossings[Index], Crossings[Index + 1]);
	}
	return Intervals;
}
}

std::vector<FStreetPolyline> HatchStripes(const FStreetPolyline& Area)
{
	if (Area.size() < 3 || EvenOddArea(Area) < 1.0)
	{
		return {};
	}
	const std::optional<FStreetPolyline> Rectangle = MinimumRotatedRectangle(Area);
	if (!Rectangle.has_value())
	{
		return {};
	}
	const FStreetPolyline& Corners = *Rectangle;
	const FStreetPoint Sides[] = {Corners[1] - Corners[0], Corners[2] - Corners[1]};
	FStreetPoint Axis = Sides[0];
	if (Norm(Sides[1]) > Norm(Sides[0]))
	{
		Axis = Sides[1];
	}
	Axis = Axis / Norm(Axis);
	const double Angle = Assumptions::HatchAngleDegrees * std::numbers::pi / 180.0;
	const double Cosine = std::cos(Angle);
	const double Sine = std::sin(Angle);
	const FStreetPoint Along = {Axis.X * Cosine - Axis.Y * Sine, Axis.X * Sine + Axis.Y * Cosine};
	const FStreetPoint Across = {-Along.Y, Along.X};
	double MinX = 1e300;
	double MinY = 1e300;
	double MaxX = -1e300;
	double MaxY = -1e300;
	for (const FStreetPoint& Point : Area)
	{
		MinX = std::min(MinX, Point.X);
		MinY = std::min(MinY, Point.Y);
		MaxX = std::max(MaxX, Point.X);
		MaxY = std::max(MaxY, Point.Y);
	}
	// The centre is the mean of the envelope's five ring points, whose first is repeated at the end.
	const FStreetPoint Centre = {(MinX + MaxX + MaxX + MinX + MinX) / 5, (MinY + MinY + MaxY + MaxY + MinY) / 5};
	const double Reach = std::hypot(MaxX - MinX, MaxY - MinY);
	std::vector<FStreetPolyline> Stripes;
	const int StripeCount = static_cast<int>(std::ceil(2 * Reach / Assumptions::HatchSpacing));
	for (int Stripe = 0; Stripe < StripeCount; ++Stripe)
	{
		const double Offset = -Reach + Stripe * Assumptions::HatchSpacing;
		const FStreetPoint Base = Centre + Across * Offset;
		const FStreetPoint LineStart = Base - Along * Reach;
		for (const auto& [From, To] : InsideIntervals(Area, LineStart, Along))
		{
			if (To - From > 0.3)
			{
				Stripes.push_back({LineStart + Along * From, LineStart + Along * To});
			}
		}
	}
	return Stripes;
}

std::vector<FMarking> GoreMarkings(const std::vector<FGore>& Gores)
{
	std::vector<FMarking> Result;
	for (const FGore& Gore : Gores)
	{
		for (const FStreetPolyline& Line : Gore.Outline)
		{
			Result.push_back({"edge", Line});
		}
		for (FStreetPolyline& Stripe : HatchStripes(Gore.Hatched))
		{
			Result.push_back({"hatch", std::move(Stripe)});
		}
	}
	return Result;
}

FStreetModel BuildStreetModel(const FOsmData& Data)
{
	FStreetModel Model;
	const FBuildingIndex Buildings(Data.Buildings);
	std::vector<FStreetWay> GroundWays;
	for (const FOsmWay& Road : Data.Roads)
	{
		FStreetWay Way = MakeStreetWay(Road);
		if (IsTunnel(Way) || Way.Points.size() < 2)
		{
			continue;
		}
		Model.Urban[Way.Id] = IsUrban(Way, Buildings);
		Model.Sections[Way.Id] = MakeCrossSection(Way.Tags, FRoadContext{Model.Urban[Way.Id]});
		Model.Widths[Way.Id] = Model.Sections[Way.Id].Width();
		if (IsGround(Way))
		{
			GroundWays.push_back(std::move(Way));
		}
		else
		{
			Model.BridgeWays.push_back(std::move(Way));
		}
	}
	Model.GroundWays = std::make_shared<const std::vector<FStreetWay>>(DualCarriageways::Straighten(GroundWays, Model.Sections));
	std::vector<FStreetPoint> SignalPoints;
	for (const FOsmPoint& Point : Data.Points)
	{
		if (OsmTags::TagEquals(Point.Tags, "highway", "traffic_signals"))
		{
			SignalPoints.push_back({Point.Position.X, Point.Position.Y});
		}
	}
	Model.Lines = BuildRoadLines(Model.GroundWays, Model.Sections, Model.Urban, SignalPoints);
	TakeCorrectedSections(Model.Lines, Model.Sections, Model.Widths);
	Model.Markings = Model.Lines.Painted();
	for (FMarking& Marking : GoreMarkings(Model.Lines.Gores))
	{
		Model.Markings.push_back(std::move(Marking));
	}
	return Model;
}
}
