#include "ExactFloatingPoint.h"
#include "BuildingGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "clipper2/clipper.h"

namespace WorldBuilder
{
namespace
{
/** Clipper works on integers; this is the scale of the coordinates it gets (a micrometre). */
constexpr double ClipperScale = 1.0e6;

/** The sign (-1, 0, 1) of the turn from A to B to C, counter-clockwise positive. */
int Orientation(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C)
{
	const long double Cross = (static_cast<long double>(B.X) - A.X) * (static_cast<long double>(C.Y) - A.Y)
		- (static_cast<long double>(B.Y) - A.Y) * (static_cast<long double>(C.X) - A.X);
	if (Cross > 0.0L)
	{
		return 1;
	}
	return Cross < 0.0L ? -1 : 0;
}

/** True when both coordinates are equal. */
bool SamePoint(const FWorldPoint& A, const FWorldPoint& B)
{
	return A.X == B.X && A.Y == B.Y;
}

/** The bounding box of a segment. */
FBox2 SegmentBox(const FWorldPoint& A, const FWorldPoint& B)
{
	FBox2 Box;
	Box.Add(A);
	Box.Add(B);
	return Box;
}

/** True when the segments meet anywhere, end points included. */
bool SegmentsIntersect(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D)
{
	if (!SegmentBox(A, B).Intersects(SegmentBox(C, D)))
	{
		return false;
	}
	const int First = Orientation(A, B, C);
	const int Second = Orientation(A, B, D);
	const int Third = Orientation(C, D, A);
	const int Fourth = Orientation(C, D, B);
	// The boxes overlap, so touching or collinear segments with a zero orientation do meet.
	return First * Second <= 0 && Third * Fourth <= 0;
}

/** The overlap of two collinear segments, as the ends of the shared stretch along the dominant axis. */
bool CollinearOverlap(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D,
	bool& bSinglePoint)
{
	const bool bAlongX = std::abs(B.X - A.X) + std::abs(D.X - C.X) >= std::abs(B.Y - A.Y) + std::abs(D.Y - C.Y);
	auto Coordinate = [bAlongX](const FWorldPoint& Point) { return bAlongX ? Point.X : Point.Y; };
	const double LowFirst = std::min(Coordinate(A), Coordinate(B));
	const double HighFirst = std::max(Coordinate(A), Coordinate(B));
	const double LowSecond = std::min(Coordinate(C), Coordinate(D));
	const double HighSecond = std::max(Coordinate(C), Coordinate(D));
	const double Low = std::max(LowFirst, LowSecond);
	const double High = std::min(HighFirst, HighSecond);
	bSinglePoint = Low == High;
	return Low <= High;
}

/** True when the point lies on the segment A-B. */
bool IsOnSegment(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& Point)
{
	return Orientation(A, B, Point) == 0 && SegmentBox(A, B).Intersects(SegmentBox(Point, Point));
}

/** True when a ray from the point towards +x crosses the edge A-B. */
bool RayCrossesEdge(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& Point)
{
	if ((A.Y > Point.Y) == (B.Y > Point.Y))
	{
		return false;
	}
	const double CrossingX = A.X + (Point.Y - A.Y) * (B.X - A.X) / (B.Y - A.Y);
	return Point.X < CrossingX;
}

/** Even-odd crossing test of a point against one ring; Boundary when it lies on an edge. */
EPointLocation LocateInRing(const FRing& Ring, const FWorldPoint& Point)
{
	bool bInside = false;
	const size_t Count = Ring.size();
	for (size_t Index = 0, Previous = Count - 1; Index < Count; Previous = Index++)
	{
		if (IsOnSegment(Ring[Previous], Ring[Index], Point))
		{
			return EPointLocation::Boundary;
		}
		if (RayCrossesEdge(Ring[Previous], Ring[Index], Point))
		{
			bInside = !bInside;
		}
	}
	return bInside ? EPointLocation::Inside : EPointLocation::Outside;
}

/** The ring as integer coordinates for Clipper. */
Clipper2Lib::Path64 ToClipperPath(const FRing& Ring)
{
	Clipper2Lib::Path64 Path;
	Path.reserve(Ring.size());
	for (const FWorldPoint& Point : Ring)
	{
		Path.emplace_back(static_cast<int64_t>(std::llround(Point.X * ClipperScale)),
			static_cast<int64_t>(std::llround(Point.Y * ClipperScale)));
	}
	return Path;
}

/** The ring back from Clipper integer coordinates. */
FRing FromClipperPath(const Clipper2Lib::Path64& Path)
{
	FRing Ring;
	Ring.reserve(Path.size());
	for (const Clipper2Lib::Point64& Point : Path)
	{
		Ring.push_back({static_cast<double>(Point.x) / ClipperScale, static_cast<double>(Point.y) / ClipperScale});
	}
	return Ring;
}

/** Adds the polygon to the paths with a positive outline and negative holes, so a non-zero fill keeps its holes. */
void AddOrientedPolygon(const FBuildingPolygon& Polygon, Clipper2Lib::Paths64& Paths)
{
	Clipper2Lib::Path64 Outline = ToClipperPath(Polygon.Outline);
	if (Clipper2Lib::Area(Outline) < 0.0)
	{
		std::reverse(Outline.begin(), Outline.end());
	}
	Paths.push_back(std::move(Outline));
	for (const FRing& Hole : Polygon.Holes)
	{
		Clipper2Lib::Path64 HolePath = ToClipperPath(Hole);
		if (Clipper2Lib::Area(HolePath) > 0.0)
		{
			std::reverse(HolePath.begin(), HolePath.end());
		}
		Paths.push_back(std::move(HolePath));
	}
}

/** Reads the outlines and their holes out of a polygon tree, level by level. */
void CollectPolygons(const Clipper2Lib::PolyPath64& Node, std::vector<FBuildingPolygon>& Polygons)
{
	for (const auto& Outline : Node)
	{
		FBuildingPolygon Polygon;
		Polygon.Outline = FromClipperPath(Outline->Polygon());
		for (const auto& Hole : *Outline)
		{
			Polygon.Holes.push_back(FromClipperPath(Hole->Polygon()));
			CollectPolygons(*Hole, Polygons);
		}
		Polygons.push_back(std::move(Polygon));
	}
}

/** Runs a Clipper union of the paths with the fill rule and returns the polygons of the result. */
std::vector<FBuildingPolygon> UnionPaths(const Clipper2Lib::Paths64& Paths, Clipper2Lib::FillRule FillRule)
{
	Clipper2Lib::Clipper64 Clipper;
	Clipper.AddSubject(Paths);
	Clipper2Lib::PolyTree64 Tree;
	Clipper.Execute(Clipper2Lib::ClipType::Union, FillRule, Tree);
	std::vector<FBuildingPolygon> Polygons;
	CollectPolygons(Tree, Polygons);
	return Polygons;
}

/** True when the edges A-B and C-D of a ring break its simplicity: neighbours may only share their common point. */
bool EdgesConflict(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D, bool bAdjacent)
{
	if (!bAdjacent)
	{
		return SegmentsIntersect(A, B, C, D);
	}
	bool bSinglePoint = false;
	return Orientation(A, B, C) == 0 && Orientation(A, B, D) == 0 && CollinearOverlap(A, B, C, D, bSinglePoint) && !bSinglePoint;
}

/** True when no two edges of the ring cross, and neighbouring edges only share their common point. */
bool RingIsSimple(const FRing& Ring)
{
	const size_t Count = Ring.size();
	if (Count < 3)
	{
		return false;
	}
	for (size_t First = 0; First < Count; ++First)
	{
		for (size_t Second = First + 1; Second < Count; ++Second)
		{
			const bool bAdjacent = Second == First + 1 || (First == 0 && Second == Count - 1);
			if (EdgesConflict(Ring[First], Ring[(First + 1) % Count], Ring[Second], Ring[(Second + 1) % Count], bAdjacent))
			{
				return false;
			}
		}
	}
	return true;
}

/** Shoelace sum around the first point, which keeps the numbers small. */
double RingCentroidAccumulate(const FRing& Ring, const FWorldPoint& Base, double& CentroidX, double& CentroidY)
{
	double Area = 0.0;
	for (size_t Index = 0; Index < Ring.size(); ++Index)
	{
		const FWorldPoint& A = Ring[Index];
		const FWorldPoint& B = Ring[(Index + 1) % Ring.size()];
		const double AX = A.X - Base.X, AY = A.Y - Base.Y, BX = B.X - Base.X, BY = B.Y - Base.Y;
		const double Cross = AX * BY - BX * AY;
		Area += Cross;
		CentroidX += (AX + BX) * Cross;
		CentroidY += (AY + BY) * Cross;
	}
	return Area;
}

/** Writes each of the polygon's rings as a closed list of points (the first repeated at the end). */
std::vector<std::vector<FWorldPoint>> ClosedRings(const FBuildingPolygon& Polygon)
{
	std::vector<std::vector<FWorldPoint>> Rings;
	Rings.push_back(Polygon.Outline);
	for (const FRing& Hole : Polygon.Holes)
	{
		Rings.push_back(Hole);
	}
	for (std::vector<FWorldPoint>& Ring : Rings)
	{
		Ring.push_back(Ring.front());
	}
	return Rings;
}

/**
 * GEOS's TopologyPreservingSimplifier on one polygon: every ring is simplified by Douglas Peucker, and a simplification
 * is only taken when the shortcut crosses no other segment of the input or of the output so far.
 */
class FTopologyPreservingSimplifier
{
public:
	explicit FTopologyPreservingSimplifier(double InTolerance) : Tolerance(InTolerance) {}

	/** Simplifies closed rings (first point repeated) in order; returns them closed again. */
	std::vector<std::vector<FWorldPoint>> Simplify(const std::vector<std::vector<FWorldPoint>>& Rings)
	{
		Lines = Rings;
		InputSegments.clear();
		OutputSegments.clear();
		Results.assign(Lines.size(), {});
		for (size_t LineIndex = 0; LineIndex < Lines.size(); ++LineIndex)
		{
			for (size_t Segment = 0; Segment + 1 < Lines[LineIndex].size(); ++Segment)
			{
				InputSegments.push_back({Lines[LineIndex][Segment], Lines[LineIndex][Segment + 1], LineIndex, Segment, true});
			}
		}
		for (size_t LineIndex = 0; LineIndex < Lines.size(); ++LineIndex)
		{
			SimplifyLine(LineIndex);
		}
		std::vector<std::vector<FWorldPoint>> Output;
		for (const std::vector<FSegment>& Result : Results)
		{
			std::vector<FWorldPoint> Points;
			for (const FSegment& Segment : Result)
			{
				Points.push_back(Segment.Start);
			}
			Points.push_back(Result.back().End);
			Output.push_back(std::move(Points));
		}
		return Output;
	}

private:
	struct FSegment
	{
		FWorldPoint Start;
		FWorldPoint End;
		size_t Line = 0;
		size_t Index = 0;
		bool bAlive = true;
	};

	/** Fewest coordinates a simplified ring keeps. */
	static constexpr size_t MinimumRingSize = 4;

	double Tolerance;
	std::vector<std::vector<FWorldPoint>> Lines;
	std::vector<FSegment> InputSegments;
	std::vector<FSegment> OutputSegments;
	std::vector<std::vector<FSegment>> Results;
	/** Which output segment (index into OutputSegments, or none) each result segment of the current line is. */
	std::vector<std::vector<int64_t>> ResultOutputIds;

	/** Simplifies one ring: the sections first, then the ring's own end point. */
	void SimplifyLine(size_t LineIndex)
	{
		CurrentLine = LineIndex;
		const std::vector<FWorldPoint>& Points = Lines[LineIndex];
		if (ResultOutputIds.size() < Lines.size())
		{
			ResultOutputIds.resize(Lines.size());
		}
		SimplifySection(0, Points.size() - 1, 0);
		SimplifyRingEndpoint();
	}

	/** Index of the point between First and Last furthest from the line through them, and its distance. */
	size_t FindFurthestPoint(size_t First, size_t Last, double& Distance) const
	{
		const std::vector<FWorldPoint>& Points = Lines[CurrentLine];
		double MaximumDistance = -1.0;
		size_t MaximumIndex = First;
		for (size_t Index = First + 1; Index < Last; ++Index)
		{
			const double PointDistance = PointSegmentDistance(Points[Index], Points[First], Points[Last]);
			if (PointDistance > MaximumDistance)
			{
				MaximumDistance = PointDistance;
				MaximumIndex = Index;
			}
		}
		Distance = MaximumDistance;
		return MaximumIndex;
	}

	/** Number of coordinates in the result so far. */
	size_t ResultSize() const
	{
		const size_t Segments = Results[CurrentLine].size();
		return Segments == 0 ? 0 : Segments + 1;
	}

	/** True when the shortcut crosses an output segment, or an input segment that is not in the section it replaces. */
	bool HasBadIntersection(size_t SectionStart, size_t SectionEnd, const FWorldPoint& Start, const FWorldPoint& End) const
	{
		const FBox2 CandidateBox = SegmentBox(Start, End);
		for (const FSegment& Segment : OutputSegments)
		{
			if (Segment.bAlive && SegmentBox(Segment.Start, Segment.End).Intersects(CandidateBox)
				&& HasInteriorIntersection(Segment.Start, Segment.End, Start, End))
			{
				return true;
			}
		}
		for (const FSegment& Segment : InputSegments)
		{
			if (!Segment.bAlive || !SegmentBox(Segment.Start, Segment.End).Intersects(CandidateBox)
				|| !HasInteriorIntersection(Segment.Start, Segment.End, Start, End))
			{
				continue;
			}
			const bool bInSection = Segment.Line == CurrentLine && Segment.Index >= SectionStart && Segment.Index < SectionEnd;
			if (!bInSection)
			{
				return true;
			}
		}
		return false;
	}

	/** Adds a segment to the result of the current line, and to the output index when it is a new shortcut. */
	void AddResult(const FSegment& Segment, bool bNewShortcut)
	{
		Results[CurrentLine].push_back(Segment);
		int64_t OutputId = -1;
		if (bNewShortcut)
		{
			OutputId = static_cast<int64_t>(OutputSegments.size());
			OutputSegments.push_back(Segment);
		}
		ResultOutputIds[CurrentLine].push_back(OutputId);
	}

	/** Takes the input segments from First up to Last of the current line out of the index: the shortcut replaces them. */
	void RetireInputSegments(size_t First, size_t Last)
	{
		for (FSegment& Segment : InputSegments)
		{
			if (Segment.Line == CurrentLine && Segment.Index >= First && Segment.Index < Last)
			{
				Segment.bAlive = false;
			}
		}
	}

	/** Replaces the points between First and Last by one segment when that stays within the tolerance and valid. */
	void SimplifySection(size_t First, size_t Last, size_t Depth)
	{
		const std::vector<FWorldPoint>& Points = Lines[CurrentLine];
		++Depth;
		if (Last - First == 1)
		{
			AddResult({Points[First], Points[Last], CurrentLine, First, true}, false);
			return;
		}
		bool bValidToSimplify = true;
		if (ResultSize() < MinimumRingSize && Depth + 1 < MinimumRingSize)
		{
			bValidToSimplify = false;
		}
		double Distance = 0.0;
		const size_t Furthest = FindFurthestPoint(First, Last, Distance);
		if (Distance > Tolerance)
		{
			bValidToSimplify = false;
		}
		if (bValidToSimplify && HasBadIntersection(First, Last, Points[First], Points[Last]))
		{
			bValidToSimplify = false;
		}
		if (bValidToSimplify)
		{
			RetireInputSegments(First, Last);
			AddResult({Points[First], Points[Last], CurrentLine, First, true}, true);
			return;
		}
		SimplifySection(First, Furthest, Depth);
		SimplifySection(Furthest, Last, Depth);
	}

	/** True when the shortcut for the ring end point crosses nothing but the two segments it replaces. */
	bool IsEndpointShortcutValid(const FWorldPoint& Start, const FWorldPoint& End) const
	{
		const FBox2 CandidateBox = SegmentBox(Start, End);
		for (const FSegment& Segment : OutputSegments)
		{
			if (Segment.bAlive && SegmentBox(Segment.Start, Segment.End).Intersects(CandidateBox)
				&& HasInteriorIntersection(Segment.Start, Segment.End, Start, End))
			{
				return false;
			}
		}
		for (const FSegment& Segment : InputSegments)
		{
			if (Segment.bAlive && SegmentBox(Segment.Start, Segment.End).Intersects(CandidateBox)
				&& HasInteriorIntersection(Segment.Start, Segment.End, Start, End))
			{
				return false;
			}
		}
		return true;
	}

	/** Drops the point where the ring starts and ends when the two segments around it are within the tolerance. */
	void SimplifyRingEndpoint()
	{
		std::vector<FSegment>& Result = Results[CurrentLine];
		if (Result.size() <= RingEndpointMinimumSegments)
		{
			return;
		}
		const FSegment FirstSegment = Result.front();
		const FSegment LastSegment = Result.back();
		const FWorldPoint& Start = LastSegment.Start;
		const FWorldPoint& End = FirstSegment.End;
		if (PointSegmentDistance(FirstSegment.Start, Start, End) > Tolerance || !IsEndpointShortcutValid(Start, End))
		{
			return;
		}
		std::vector<int64_t>& Ids = ResultOutputIds[CurrentLine];
		for (int64_t Id : {Ids.front(), Ids.back()})
		{
			if (Id >= 0)
			{
				OutputSegments[static_cast<size_t>(Id)].bAlive = false;
			}
		}
		FSegment Shortcut = {Start, End, CurrentLine, 0, true};
		Result.pop_back();
		Ids.pop_back();
		Result.front() = Shortcut;
		OutputSegments.push_back(Shortcut);
		Ids.front() = static_cast<int64_t>(OutputSegments.size() - 1);
	}

	/** A ring needs more than this many segments before its end point is tried. */
	static constexpr size_t RingEndpointMinimumSegments = MinimumRingSize;
	size_t CurrentLine = 0;
};

/** The convex hull, clockwise from the lowest point (ties: the leftmost), without a repeated end point. */
std::vector<FWorldPoint> ConvexHullClockwise(std::vector<FWorldPoint> Points)
{
	std::sort(Points.begin(), Points.end(), [](const FWorldPoint& A, const FWorldPoint& B) {
		return A.X < B.X || (A.X == B.X && A.Y < B.Y);
	});
	Points.erase(std::unique(Points.begin(), Points.end(), SamePoint), Points.end());
	if (Points.size() < 3)
	{
		return Points;
	}
	std::vector<FWorldPoint> Hull(2 * Points.size());
	size_t Count = 0;
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		while (Count >= 2 && Orientation(Hull[Count - 2], Hull[Count - 1], Points[Index]) <= 0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index];
	}
	const size_t LowerCount = Count + 1;
	for (size_t Index = Points.size() - 1; Index > 0; --Index)
	{
		while (Count >= LowerCount && Orientation(Hull[Count - 2], Hull[Count - 1], Points[Index - 1]) <= 0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index - 1];
	}
	Hull.resize(Count - 1);
	std::reverse(Hull.begin(), Hull.end());
	size_t Pivot = 0;
	for (size_t Index = 1; Index < Hull.size(); ++Index)
	{
		if (Hull[Index].Y < Hull[Pivot].Y || (Hull[Index].Y == Hull[Pivot].Y && Hull[Index].X < Hull[Pivot].X))
		{
			Pivot = Index;
		}
	}
	std::rotate(Hull.begin(), Hull.begin() + static_cast<std::ptrdiff_t>(Pivot), Hull.end());
	return Hull;
}

/** The rectangle around a clockwise convex hull with one side along the hull edge from Start. */
struct FEdgeRectangle
{
	std::array<FWorldPoint, 4> Corners;
	double Area = 0.0;
};

/** The smallest rectangle with one side along the hull edge that starts at Start. */
FEdgeRectangle RectangleAlongEdge(const std::vector<FWorldPoint>& Hull, size_t Start)
{
	const FWorldPoint& Origin = Hull[Start];
	const FWorldPoint& End = Hull[(Start + 1) % Hull.size()];
	const double Length = std::hypot(End.X - Origin.X, End.Y - Origin.Y);
	const FWorldPoint Along = {(End.X - Origin.X) / Length, (End.Y - Origin.Y) / Length};
	// The hull runs clockwise, so its inside is on the right of every edge.
	const FWorldPoint Inward = {Along.Y, -Along.X};
	double MinimumAlong = std::numeric_limits<double>::max();
	double MaximumAlong = -std::numeric_limits<double>::max();
	double MaximumInward = 0.0;
	for (const FWorldPoint& Point : Hull)
	{
		const double AlongDistance = (Point.X - Origin.X) * Along.X + (Point.Y - Origin.Y) * Along.Y;
		const double InwardDistance = (Point.X - Origin.X) * Inward.X + (Point.Y - Origin.Y) * Inward.Y;
		MinimumAlong = std::min(MinimumAlong, AlongDistance);
		MaximumAlong = std::max(MaximumAlong, AlongDistance);
		MaximumInward = std::max(MaximumInward, InwardDistance);
	}
	FEdgeRectangle Rectangle;
	Rectangle.Corners[0] = {Origin.X + Along.X * MinimumAlong, Origin.Y + Along.Y * MinimumAlong};
	Rectangle.Corners[1] = {Origin.X + Along.X * MaximumAlong, Origin.Y + Along.Y * MaximumAlong};
	Rectangle.Corners[2] = {Rectangle.Corners[1].X + Inward.X * MaximumInward, Rectangle.Corners[1].Y + Inward.Y * MaximumInward};
	Rectangle.Corners[3] = {Rectangle.Corners[0].X + Inward.X * MaximumInward, Rectangle.Corners[0].Y + Inward.Y * MaximumInward};
	Rectangle.Area = (MaximumAlong - MinimumAlong) * MaximumInward;
	return Rectangle;
}
}

void FBox2::Add(const FWorldPoint& Point)
{
	MinX = std::min(MinX, Point.X);
	MinY = std::min(MinY, Point.Y);
	MaxX = std::max(MaxX, Point.X);
	MaxY = std::max(MaxY, Point.Y);
}

bool FBox2::Intersects(const FBox2& Other) const
{
	return !(Other.MinX > MaxX || Other.MaxX < MinX || Other.MinY > MaxY || Other.MaxY < MinY);
}

FBox2 FBox2::Expanded(double Distance) const
{
	FBox2 Result;
	Result.MinX = MinX - Distance;
	Result.MinY = MinY - Distance;
	Result.MaxX = MaxX + Distance;
	Result.MaxY = MaxY + Distance;
	return Result;
}

double FBox2::DistanceTo(const FBox2& Other) const
{
	const double GapX = std::max({0.0, Other.MinX - MaxX, MinX - Other.MaxX});
	const double GapY = std::max({0.0, Other.MinY - MaxY, MinY - Other.MaxY});
	return std::hypot(GapX, GapY);
}

FBox2 BoundingBox(const FRing& Ring)
{
	FBox2 Box;
	for (const FWorldPoint& Point : Ring)
	{
		Box.Add(Point);
	}
	return Box;
}

FBox2 BoundingBox(const FBuildingPolygon& Polygon)
{
	return BoundingBox(Polygon.Outline);
}

double SignedArea(const FRing& Ring)
{
	double Sum = 0.0;
	for (size_t Index = 0; Index < Ring.size(); ++Index)
	{
		const FWorldPoint& A = Ring[Index];
		const FWorldPoint& B = Ring[(Index + 1) % Ring.size()];
		Sum += A.X * B.Y - B.X * A.Y;
	}
	return 0.5 * Sum;
}

double PolygonArea(const FBuildingPolygon& Polygon)
{
	double Area = std::abs(SignedArea(Polygon.Outline));
	for (const FRing& Hole : Polygon.Holes)
	{
		Area -= std::abs(SignedArea(Hole));
	}
	return Area;
}

FWorldPoint PolygonCentroid(const FBuildingPolygon& Polygon)
{
	const FWorldPoint Base = Polygon.Outline.front();
	double CentroidX = 0.0;
	double CentroidY = 0.0;
	double OutlineArea = RingCentroidAccumulate(Polygon.Outline, Base, CentroidX, CentroidY);
	double Sign = OutlineArea < 0.0 ? -1.0 : 1.0;
	double TotalArea = Sign * OutlineArea;
	CentroidX *= Sign;
	CentroidY *= Sign;
	for (const FRing& Hole : Polygon.Holes)
	{
		double HoleX = 0.0;
		double HoleY = 0.0;
		const double HoleArea = RingCentroidAccumulate(Hole, Base, HoleX, HoleY);
		const double HoleSign = HoleArea < 0.0 ? -1.0 : 1.0;
		TotalArea -= HoleSign * HoleArea;
		CentroidX -= HoleSign * HoleX;
		CentroidY -= HoleSign * HoleY;
	}
	if (TotalArea == 0.0)
	{
		return Base;
	}
	return {Base.X + CentroidX / (3.0 * TotalArea), Base.Y + CentroidY / (3.0 * TotalArea)};
}

namespace
{
/** Narrows [Low, High] to the vertex heights of the ring closest to CentreY from below and from above. */
void NarrowScanLine(const FRing& Ring, double CentreY, double& Low, double& High)
{
	for (const FWorldPoint& Point : Ring)
	{
		if (Point.Y <= CentreY)
		{
			Low = std::max(Low, Point.Y);
		}
		else
		{
			High = std::min(High, Point.Y);
		}
	}
}

/** The height of the horizontal line GEOS cuts a polygon with: between the vertices around the middle of its height. */
double ScanLineHeight(const FBuildingPolygon& Polygon)
{
	const FBox2 Box = BoundingBox(Polygon);
	const double CentreY = 0.5 * (Box.MinY + Box.MaxY);
	double High = Box.MaxY;
	double Low = Box.MinY;
	NarrowScanLine(Polygon.Outline, CentreY, Low, High);
	for (const FRing& Hole : Polygon.Holes)
	{
		NarrowScanLine(Hole, CentreY, Low, High);
	}
	return 0.5 * (High + Low);
}

/** The x positions where the ring crosses the line at ScanY, counting a vertex on the line once. */
void AddRingCrossings(const FRing& Ring, double ScanY, std::vector<double>& Crossings)
{
	for (size_t Index = 0; Index < Ring.size(); ++Index)
	{
		const FWorldPoint& Start = Ring[Index];
		const FWorldPoint& End = Ring[(Index + 1) % Ring.size()];
		if (ScanY > std::max(Start.Y, End.Y) || ScanY < std::min(Start.Y, End.Y) || Start.Y == End.Y)
		{
			continue;
		}
		if ((Start.Y == ScanY && End.Y < ScanY) || (End.Y == ScanY && Start.Y < ScanY))
		{
			continue;
		}
		if (Start.X == End.X)
		{
			Crossings.push_back(Start.X);
			continue;
		}
		const double Slope = (End.Y - Start.Y) / (End.X - Start.X);
		Crossings.push_back(Start.X + (ScanY - Start.Y) / Slope);
	}
}
}

FWorldPoint RepresentativePoint(const FBuildingPolygon& Polygon)
{
	const double ScanY = ScanLineHeight(Polygon);
	std::vector<double> Crossings;
	AddRingCrossings(Polygon.Outline, ScanY, Crossings);
	for (const FRing& Hole : Polygon.Holes)
	{
		AddRingCrossings(Hole, ScanY, Crossings);
	}
	std::sort(Crossings.begin(), Crossings.end());
	FWorldPoint Best = Polygon.Outline.front();
	double BestWidth = -1.0;
	for (size_t Index = 0; Index + 1 < Crossings.size(); Index += 2)
	{
		const double Width = Crossings[Index + 1] - Crossings[Index];
		if (Width > BestWidth)
		{
			BestWidth = Width;
			Best = {0.5 * (Crossings[Index] + Crossings[Index + 1]), ScanY};
		}
	}
	return Best;
}

EPointLocation LocatePoint(const FBuildingPolygon& Polygon, const FWorldPoint& Point)
{
	const EPointLocation InOutline = LocateInRing(Polygon.Outline, Point);
	if (InOutline != EPointLocation::Inside)
	{
		return InOutline;
	}
	for (const FRing& Hole : Polygon.Holes)
	{
		const EPointLocation InHole = LocateInRing(Hole, Point);
		if (InHole == EPointLocation::Boundary)
		{
			return EPointLocation::Boundary;
		}
		if (InHole == EPointLocation::Inside)
		{
			return EPointLocation::Outside;
		}
	}
	return EPointLocation::Inside;
}

double PointSegmentDistance(const FWorldPoint& Point, const FWorldPoint& A, const FWorldPoint& B)
{
	const double DeltaX = B.X - A.X;
	const double DeltaY = B.Y - A.Y;
	const double LengthSquared = DeltaX * DeltaX + DeltaY * DeltaY;
	if (LengthSquared == 0.0)
	{
		return std::hypot(Point.X - A.X, Point.Y - A.Y);
	}
	const double Fraction = ((Point.X - A.X) * DeltaX + (Point.Y - A.Y) * DeltaY) / LengthSquared;
	if (Fraction <= 0.0)
	{
		return std::hypot(Point.X - A.X, Point.Y - A.Y);
	}
	if (Fraction >= 1.0)
	{
		return std::hypot(Point.X - B.X, Point.Y - B.Y);
	}
	// Distance to the line, which is the more precise form for a point near the segment's middle.
	return std::abs((A.Y - Point.Y) * DeltaX - (A.X - Point.X) * DeltaY) / std::sqrt(LengthSquared);
}

double SegmentDistance(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D)
{
	if (SegmentsIntersect(A, B, C, D))
	{
		return 0.0;
	}
	return std::min({PointSegmentDistance(A, C, D), PointSegmentDistance(B, C, D), PointSegmentDistance(C, A, B),
		PointSegmentDistance(D, A, B)});
}

bool HasInteriorIntersection(const FWorldPoint& A, const FWorldPoint& B, const FWorldPoint& C, const FWorldPoint& D)
{
	if (!SegmentBox(A, B).Intersects(SegmentBox(C, D)))
	{
		return false;
	}
	const int First = Orientation(A, B, C);
	const int Second = Orientation(A, B, D);
	const int Third = Orientation(C, D, A);
	const int Fourth = Orientation(C, D, B);
	if (First == 0 && Second == 0)
	{
		bool bSinglePoint = false;
		if (!CollinearOverlap(A, B, C, D, bSinglePoint) || bSinglePoint)
		{
			return false;
		}
		const bool bIdentical = (SamePoint(A, C) && SamePoint(B, D)) || (SamePoint(A, D) && SamePoint(B, C));
		return !bIdentical;
	}
	if (First * Second > 0 || Third * Fourth > 0)
	{
		return false;
	}
	if (First != 0 && Second != 0 && Third != 0 && Fourth != 0)
	{
		return true;
	}
	// One end point lies on the other segment: that is interior unless both segments end there.
	FWorldPoint Meeting = B;
	if (First == 0)
	{
		Meeting = C;
	}
	else if (Second == 0)
	{
		Meeting = D;
	}
	else if (Third == 0)
	{
		Meeting = A;
	}
	const bool bEndOfFirst = SamePoint(Meeting, A) || SamePoint(Meeting, B);
	const bool bEndOfSecond = SamePoint(Meeting, C) || SamePoint(Meeting, D);
	return !(bEndOfFirst && bEndOfSecond);
}

double PolygonSegmentDistance(const FBuildingPolygon& Polygon, const FBox2& PolygonBox, const FWorldPoint& A,
	const FWorldPoint& B, double Limit)
{
	const FBox2 Box = SegmentBox(A, B);
	const double BoxDistance = PolygonBox.DistanceTo(Box);
	if (BoxDistance > Limit)
	{
		return BoxDistance;
	}
	if (LocatePoint(Polygon, A) != EPointLocation::Outside || LocatePoint(Polygon, B) != EPointLocation::Outside)
	{
		return 0.0;
	}
	double Best = std::numeric_limits<double>::max();
	auto VisitRing = [&](const FRing& Ring) {
		for (size_t Index = 0; Index < Ring.size(); ++Index)
		{
			const FWorldPoint& Start = Ring[Index];
			const FWorldPoint& End = Ring[(Index + 1) % Ring.size()];
			if (SegmentBox(Start, End).DistanceTo(Box) < Best)
			{
				Best = std::min(Best, SegmentDistance(Start, End, A, B));
			}
		}
	};
	VisitRing(Polygon.Outline);
	for (const FRing& Hole : Polygon.Holes)
	{
		VisitRing(Hole);
	}
	return Best;
}

namespace
{
/** The smallest distance from the segments of a ring to a polygon, looking only at segments closer than Best. */
double RingToPolygonDistance(const FRing& Ring, const FBuildingPolygon& Polygon, const FBox2& PolygonBox, double Best)
{
	for (size_t Index = 0; Index < Ring.size() && Best > 0.0; ++Index)
	{
		const FWorldPoint& Start = Ring[Index];
		const FWorldPoint& End = Ring[(Index + 1) % Ring.size()];
		if (SegmentBox(Start, End).DistanceTo(PolygonBox) < Best)
		{
			Best = std::min(Best, PolygonSegmentDistance(Polygon, PolygonBox, Start, End, Best));
		}
	}
	return Best;
}
}

double PolygonDistance(const FBuildingPolygon& First, const FBuildingPolygon& Second, double Limit)
{
	const FBox2 SecondBox = BoundingBox(Second);
	const double BoxDistance = BoundingBox(First).DistanceTo(SecondBox);
	if (BoxDistance > Limit)
	{
		return BoxDistance;
	}
	if (LocatePoint(First, Second.Outline.front()) != EPointLocation::Outside
		|| LocatePoint(Second, First.Outline.front()) != EPointLocation::Outside)
	{
		return 0.0;
	}
	double Best = RingToPolygonDistance(First.Outline, Second, SecondBox, std::numeric_limits<double>::max());
	for (const FRing& Hole : First.Holes)
	{
		Best = RingToPolygonDistance(Hole, Second, SecondBox, Best);
	}
	return Best;
}

bool IsSimplePolygon(const FBuildingPolygon& Polygon)
{
	if (!RingIsSimple(Polygon.Outline))
	{
		return false;
	}
	for (const FRing& Hole : Polygon.Holes)
	{
		if (!RingIsSimple(Hole))
		{
			return false;
		}
	}
	return true;
}

std::vector<FBuildingPolygon> MakeValidPolygon(const FBuildingPolygon& Polygon)
{
	Clipper2Lib::Paths64 Paths;
	Paths.push_back(ToClipperPath(Polygon.Outline));
	for (const FRing& Hole : Polygon.Holes)
	{
		Paths.push_back(ToClipperPath(Hole));
	}
	return UnionPaths(Paths, Clipper2Lib::FillRule::EvenOdd);
}

std::vector<FBuildingPolygon> UnionPolygons(const std::vector<FBuildingPolygon>& Polygons)
{
	Clipper2Lib::Paths64 Paths;
	for (const FBuildingPolygon& Polygon : Polygons)
	{
		AddOrientedPolygon(Polygon, Paths);
	}
	return UnionPaths(Paths, Clipper2Lib::FillRule::NonZero);
}

FBuildingPolygon SimplifyPolygon(const FBuildingPolygon& Polygon, double Tolerance)
{
	FTopologyPreservingSimplifier Simplifier(Tolerance);
	std::vector<std::vector<FWorldPoint>> Closed = Simplifier.Simplify(ClosedRings(Polygon));
	FBuildingPolygon Result;
	for (size_t Index = 0; Index < Closed.size(); ++Index)
	{
		FRing Ring(Closed[Index].begin(), Closed[Index].end() - 1);
		if (Index == 0)
		{
			Result.Outline = std::move(Ring);
		}
		else
		{
			Result.Holes.push_back(std::move(Ring));
		}
	}
	return Result;
}

bool MinimumRotatedRectangle(const FBuildingPolygon& Polygon, std::array<FWorldPoint, 4>& Corners)
{
	const std::vector<FWorldPoint> Hull = ConvexHullClockwise(Polygon.Outline);
	if (Hull.size() < 3)
	{
		return false;
	}
	double SmallestArea = std::numeric_limits<double>::max();
	for (size_t Start = 0; Start < Hull.size(); ++Start)
	{
		const FEdgeRectangle Rectangle = RectangleAlongEdge(Hull, Start);
		if (Rectangle.Area < SmallestArea)
		{
			SmallestArea = Rectangle.Area;
			Corners = Rectangle.Corners;
		}
	}
	return true;
}

namespace
{
/** The mitre corner of the offset curve at vertex Index, on the outside of the ring. */
FWorldPoint OffsetCorner(const FRing& Ring, size_t Index, double Distance)
{
	const size_t Count = Ring.size();
	const FWorldPoint& Previous = Ring[(Index + Count - 1) % Count];
	const FWorldPoint& Here = Ring[Index];
	const FWorldPoint& Next = Ring[(Index + 1) % Count];
	// The outside is on the left of a clockwise ring and on the right of a counter-clockwise one.
	const double Side = SignedArea(Ring) < 0.0 ? 1.0 : -1.0;
	const double InLength = std::hypot(Here.X - Previous.X, Here.Y - Previous.Y);
	const double OutLength = std::hypot(Next.X - Here.X, Next.Y - Here.Y);
	const FWorldPoint InNormal = {-(Here.Y - Previous.Y) / InLength * Side, (Here.X - Previous.X) / InLength * Side};
	const FWorldPoint OutNormal = {-(Next.Y - Here.Y) / OutLength * Side, (Next.X - Here.X) / OutLength * Side};
	const double Scale = Distance / (1.0 + InNormal.X * OutNormal.X + InNormal.Y * OutNormal.Y);
	return {Here.X + (InNormal.X + OutNormal.X) * Scale, Here.Y + (InNormal.Y + OutNormal.Y) * Scale};
}

/** Orients the ring clockwise or counter-clockwise by its signed area. */
void OrientRing(FRing& Ring, bool bClockwise)
{
	if ((SignedArea(Ring) < 0.0) != bClockwise)
	{
		std::reverse(Ring.begin(), Ring.end());
	}
}

/**
 * Puts a buffer outline in the order GEOS returns it for a clockwise ring: clockwise, starting at the offset corner of
 * the input's first vertex. Roof tie breaks depend on where the ring starts.
 */
void OrderLikeGeos(const FBuildingPolygon& Source, double Distance, FBuildingPolygon& Result)
{
	OrientRing(Result.Outline, true);
	for (FRing& Hole : Result.Holes)
	{
		OrientRing(Hole, false);
	}
	if (Source.Outline.size() < 3)
	{
		return;
	}
	const FWorldPoint Corner = OffsetCorner(Source.Outline, 0, Distance);
	size_t Nearest = 0;
	double NearestDistance = std::numeric_limits<double>::max();
	for (size_t Index = 0; Index < Result.Outline.size(); ++Index)
	{
		const double Away = std::hypot(Result.Outline[Index].X - Corner.X, Result.Outline[Index].Y - Corner.Y);
		if (Away < NearestDistance)
		{
			NearestDistance = Away;
			Nearest = Index;
		}
	}
	std::rotate(Result.Outline.begin(), Result.Outline.begin() + static_cast<std::ptrdiff_t>(Nearest), Result.Outline.end());
}
}

namespace
{
constexpr double MitreLimit = 4.0;
/** Offset ends closer than this fraction of the distance count as one point (GEOS's OFFSET_SEGMENT_SEPARATION_FACTOR). */
constexpr double OffsetSeparationFactor = 1.0e-3;
/** Consecutive curve points closer than this fraction of the distance are one point. */
constexpr double CurveVertexSnapFactor = 1.0e-6;
/** Round joins are approximated with 16 segments per quarter circle, the default of shapely 2. */
constexpr double FilletAngleQuantum = std::numbers::pi / 2.0 / 16.0;

/** A segment with the sign of the side the buffer grows to: +1 for the left of the travel direction, -1 for the right. */
struct FOffsetSegment
{
	FWorldPoint Start;
	FWorldPoint End;
};

/** The segment shifted sideways by Distance to the given side (GEOS's computeOffsetSegment). */
FOffsetSegment OffsetSegment(const FWorldPoint& Start, const FWorldPoint& End, double SideSign, double Distance)
{
	const double DeltaX = End.X - Start.X;
	const double DeltaY = End.Y - Start.Y;
	const double Length = std::sqrt(DeltaX * DeltaX + DeltaY * DeltaY);
	const double UX = SideSign * Distance * DeltaX / Length;
	const double UY = SideSign * Distance * DeltaY / Length;
	return {{Start.X - UY, Start.Y + UX}, {End.X - UY, End.Y + UX}};
}

/** Intersection of the infinite lines, conditioned around the middle of the segments' overlapping box like GEOS's Intersection. */
bool LineIntersection(const FWorldPoint& P1, const FWorldPoint& P2, const FWorldPoint& Q1, const FWorldPoint& Q2,
	FWorldPoint& Result)
{
	const double MinX0 = std::min(P1.X, P2.X), MinY0 = std::min(P1.Y, P2.Y);
	const double MaxX0 = std::max(P1.X, P2.X), MaxY0 = std::max(P1.Y, P2.Y);
	const double MinX1 = std::min(Q1.X, Q2.X), MinY1 = std::min(Q1.Y, Q2.Y);
	const double MaxX1 = std::max(Q1.X, Q2.X), MaxY1 = std::max(Q1.Y, Q2.Y);
	const double MiddleX = (std::max(MinX0, MinX1) + std::min(MaxX0, MaxX1)) / 2.0;
	const double MiddleY = (std::max(MinY0, MinY1) + std::min(MaxY0, MaxY1)) / 2.0;
	const double P1X = P1.X - MiddleX, P1Y = P1.Y - MiddleY, P2X = P2.X - MiddleX, P2Y = P2.Y - MiddleY;
	const double Q1X = Q1.X - MiddleX, Q1Y = Q1.Y - MiddleY, Q2X = Q2.X - MiddleX, Q2Y = Q2.Y - MiddleY;
	const double PX = P1Y - P2Y, PY = P2X - P1X, PW = P1X * P2Y - P2X * P1Y;
	const double QX = Q1Y - Q2Y, QY = Q2X - Q1X, QW = Q1X * Q2Y - Q2X * Q1Y;
	const double X = PY * QW - QY * PW;
	const double Y = QX * PW - PX * QW;
	const double W = PX * QY - QX * PY;
	const double IntersectionX = X / W;
	const double IntersectionY = Y / W;
	if (!std::isfinite(IntersectionX) || !std::isfinite(IntersectionY))
	{
		return false;
	}
	Result = {IntersectionX + MiddleX, IntersectionY + MiddleY};
	return true;
}

/** The direction from one point to the other in radians. */
double Angle(const FWorldPoint& From, const FWorldPoint& To)
{
	return std::atan2(To.Y - From.Y, To.X - From.X);
}

/** The angle brought into (-pi, pi]. */
double NormalizeAngle(double Value)
{
	while (Value > std::numbers::pi)
	{
		Value -= 2.0 * std::numbers::pi;
	}
	while (Value <= -std::numbers::pi)
	{
		Value += 2.0 * std::numbers::pi;
	}
	return Value;
}

/** The signed angle at Tip1 from Tail to Tip2, in (-pi, pi]. */
double AngleBetweenOriented(const FWorldPoint& Tail, const FWorldPoint& Tip1, const FWorldPoint& Tip2)
{
	const double Delta = Angle(Tip1, Tip2) - Angle(Tip1, Tail);
	if (Delta <= -std::numbers::pi)
	{
		return Delta + 2.0 * std::numbers::pi;
	}
	if (Delta > std::numbers::pi)
	{
		return Delta - 2.0 * std::numbers::pi;
	}
	return Delta;
}

/** One offset curve under construction: the points, without ones that coincide with the previous. */
class FOffsetCurve
{
public:
	FOffsetCurve(double InDistance, double InSideSign, bool bInRound)
		: Distance(InDistance), SideSign(InSideSign), bRound(bInRound), MinimumVertexDistance(InDistance * CurveVertexSnapFactor)
	{
	}

	/** Adds the points of the offset curve for the corner at Middle between Previous and Next. */
	void AddCorner(const FWorldPoint& Previous, const FWorldPoint& Middle, const FWorldPoint& Next)
	{
		if (Middle.X == Next.X && Middle.Y == Next.Y)
		{
			return;
		}
		const FOffsetSegment First = OffsetSegment(Previous, Middle, SideSign, Distance);
		const FOffsetSegment Second = OffsetSegment(Middle, Next, SideSign, Distance);
		const int Turn = Orientation(Previous, Middle, Next);
		if (Turn == 0)
		{
			return; // collinear, the offset lines are parallel
		}
		const bool bOutside = (Turn < 0 && SideSign > 0.0) || (Turn > 0 && SideSign < 0.0);
		if (bOutside)
		{
			AddOutsideTurn(Previous, Middle, Next, First, Second, Turn);
		}
		else
		{
			AddInsideTurn(Middle, First, Second);
		}
	}

	/** Adds the end of the last segment and returns the curve closed. */
	std::vector<FWorldPoint> Finish()
	{
		if (!Points.empty() && (Points.front().X != Points.back().X || Points.front().Y != Points.back().Y))
		{
			Points.push_back(Points.front());
		}
		return Points;
	}

	/** Adds a curve point unless it coincides with the previous one. */
	void AddPoint(const FWorldPoint& Point)
	{
		if (!Points.empty())
		{
			const FWorldPoint& Last = Points.back();
			if (std::hypot(Point.X - Last.X, Point.Y - Last.Y) < MinimumVertexDistance)
			{
				return;
			}
		}
		Points.push_back(Point);
	}

private:
	double Distance;
	double SideSign;
	bool bRound;
	double MinimumVertexDistance;
	std::vector<FWorldPoint> Points;

	/** The arc of radius Distance around Middle from the end of the first offset segment to the start of the second. */
	void AddFillet(const FWorldPoint& Middle, const FWorldPoint& From, const FWorldPoint& To, int Turn)
	{
		double StartAngle = std::atan2(From.Y - Middle.Y, From.X - Middle.X);
		const double EndAngle = std::atan2(To.Y - Middle.Y, To.X - Middle.X);
		if (Turn < 0)
		{
			if (StartAngle <= EndAngle)
			{
				StartAngle += 2.0 * std::numbers::pi;
			}
		}
		else if (StartAngle >= EndAngle)
		{
			StartAngle -= 2.0 * std::numbers::pi;
		}
		AddPoint(From);
		const double DirectionFactor = Turn < 0 ? -1.0 : 1.0;
		const double TotalAngle = std::abs(StartAngle - EndAngle);
		const int SegmentCount = static_cast<int>(TotalAngle / FilletAngleQuantum + 0.5);
		if (SegmentCount >= 1)
		{
			const double AngleIncrement = TotalAngle / SegmentCount;
			for (double Current = 0.0; Current < TotalAngle; Current += AngleIncrement)
			{
				const double Angle = StartAngle + DirectionFactor * Current;
				AddPoint({Middle.X + Distance * std::cos(Angle), Middle.Y + Distance * std::sin(Angle)});
			}
		}
		AddPoint(To);
	}

	void AddOutsideTurn(const FWorldPoint& Previous, const FWorldPoint& Middle, const FWorldPoint& Next, const FOffsetSegment& First,
		const FOffsetSegment& Second, int Turn)
	{
		if (std::hypot(First.End.X - Second.Start.X, First.End.Y - Second.Start.Y) < Distance * OffsetSeparationFactor)
		{
			AddPoint(First.End);
			return;
		}
		if (bRound)
		{
			AddPoint(First.End);
			AddFillet(Middle, First.End, Second.Start, Turn);
			AddPoint(Second.Start);
			return;
		}
		FWorldPoint Corner;
		bool bWithinLimit = false;
		if (LineIntersection(First.Start, First.End, Second.Start, Second.End, Corner))
		{
			bWithinLimit = std::hypot(Corner.X - Middle.X, Corner.Y - Middle.Y) / std::abs(Distance) <= MitreLimit;
		}
		if (bWithinLimit)
		{
			AddPoint(Corner);
			return;
		}
		AddLimitedMitre(Previous, Middle, Next);
	}

	/** The bevel that cuts a mitre off at MitreLimit times the distance. */
	void AddLimitedMitre(const FWorldPoint& Previous, const FWorldPoint& Middle, const FWorldPoint& Next)
	{
		const double FirstAngle = Angle(Middle, Previous);
		const double AngleDelta = AngleBetweenOriented(Previous, Middle, Next);
		const double HalfDelta = AngleDelta / 2.0;
		const double MiddleAngle = NormalizeAngle(FirstAngle + HalfDelta);
		const double MitreMiddleAngle = NormalizeAngle(MiddleAngle + std::numbers::pi);
		const double MitreDistance = MitreLimit * Distance;
		const double BevelDelta = MitreDistance * std::abs(std::sin(HalfDelta));
		const double BevelHalfLength = Distance - BevelDelta;
		const FWorldPoint BevelMiddle = {Middle.X + MitreDistance * std::cos(MitreMiddleAngle),
			Middle.Y + MitreDistance * std::sin(MitreMiddleAngle)};
		const double DeltaX = BevelMiddle.X - Middle.X;
		const double DeltaY = BevelMiddle.Y - Middle.Y;
		const double Length = std::sqrt(DeltaX * DeltaX + DeltaY * DeltaY);
		auto PointAlongOffset = [&](double Offset) {
			const double UX = Offset * DeltaX / Length;
			const double UY = Offset * DeltaY / Length;
			return FWorldPoint{Middle.X + 1.0 * DeltaX - UY, Middle.Y + 1.0 * DeltaY + UX};
		};
		const FWorldPoint BevelLeft = PointAlongOffset(BevelHalfLength);
		const FWorldPoint BevelRight = PointAlongOffset(-BevelHalfLength);
		if (SideSign > 0.0)
		{
			AddPoint(BevelLeft);
			AddPoint(BevelRight);
		}
		else
		{
			AddPoint(BevelRight);
			AddPoint(BevelLeft);
		}
	}

	/** Adds the point where the two offset segments of an inside turn cross. */
	void AddInsideTurn(const FWorldPoint& Middle, const FOffsetSegment& First, const FOffsetSegment& Second)
	{
		FWorldPoint Crossing;
		if (SegmentsIntersect(First.Start, First.End, Second.Start, Second.End)
			&& LineIntersection(First.Start, First.End, Second.Start, Second.End, Crossing))
		{
			AddPoint(Crossing);
			return;
		}
		// The offsets do not meet: a closing segment through the corner keeps the curve connected (it ends up inside the buffer).
		AddPoint(First.End);
		AddPoint(Middle);
		AddPoint(Second.Start);
	}
};

/** The offset curve of a single ring, the way GEOS builds it and in its vertex order (empty for a degenerate ring). */
FRing OffsetCurve(const FRing& Ring, double Distance, bool bRound)
{
	const size_t Count = Ring.size();
	if (Count < 3)
	{
		return {};
	}
	const double SideSign = SignedArea(Ring) < 0.0 ? 1.0 : -1.0;
	FOffsetCurve Builder(Distance, SideSign, bRound);
	for (size_t Index = 0; Index < Count; ++Index)
	{
		Builder.AddCorner(Ring[(Index + Count - 1) % Count], Ring[Index], Ring[(Index + 1) % Count]);
	}
	FRing Closed = Builder.Finish();
	if (Closed.size() < 4)
	{
		return {};
	}
	Closed.pop_back();
	return Closed;
}

/** Distance within which a union vertex counts as a curve vertex or crossing of the curve. */
constexpr double RestoreSnapDistance = 1.0e-5;

/** The vertex of the curve within a hair of the point, if there is one. */
bool FindCurveVertexNear(const FRing& Curve, const FWorldPoint& Point, FWorldPoint& Found)
{
	for (const FWorldPoint& CurvePoint : Curve)
	{
		if (std::abs(Point.X - CurvePoint.X) < RestoreSnapDistance && std::abs(Point.Y - CurvePoint.Y) < RestoreSnapDistance)
		{
			Found = CurvePoint;
			return true;
		}
	}
	return false;
}

/** The place where two non-neighbouring segments of the curve cross, if one is within a hair of the point. */
bool FindCurveCrossingNear(const FRing& Curve, const FWorldPoint& Point, FWorldPoint& Found)
{
	const size_t Count = Curve.size();
	for (size_t First = 0; First < Count; ++First)
	{
		for (size_t Second = First + 2; Second < Count; ++Second)
		{
			const bool bNeighbours = First == 0 && Second == Count - 1;
			const FWorldPoint& A = Curve[First];
			const FWorldPoint& B = Curve[(First + 1) % Count];
			const FWorldPoint& C = Curve[Second];
			const FWorldPoint& D = Curve[(Second + 1) % Count];
			FWorldPoint Crossing;
			if (bNeighbours || !SegmentsIntersect(A, B, C, D) || !LineIntersection(A, B, C, D, Crossing))
			{
				continue;
			}
			if (std::abs(Crossing.X - Point.X) < RestoreSnapDistance && std::abs(Crossing.Y - Point.Y) < RestoreSnapDistance)
			{
				Found = Crossing;
				return true;
			}
		}
	}
	return false;
}

/**
 * Puts the vertices of a union result back to the exact places GEOS has them: a vertex within a hair of a curve vertex
 * is that vertex, and any other is where two segments of the curve cross, which is computed from those segments,
 * undoing Clipper's integer rounding.
 */
void RestoreCurveVertices(FRing& Ring, const FRing& Curve)
{
	for (FWorldPoint& Point : Ring)
	{
		FWorldPoint Found;
		if (FindCurveVertexNear(Curve, Point, Found) || FindCurveCrossingNear(Curve, Point, Found))
		{
			Point = Found;
		}
	}
}
}

bool BufferMitre(const FBuildingPolygon& Polygon, double Distance, FBuildingPolygon& Result)
{
	const FRing Curve = Polygon.Holes.empty() ? OffsetCurve(Polygon.Outline, Distance, false) : FRing();
	if (!Curve.empty() && RingIsSimple(Curve))
	{
		Result.Outline = Curve;
		Result.Holes.clear();
		OrientRing(Result.Outline, true);
		return true;
	}
	Clipper2Lib::Paths64 Paths;
	AddOrientedPolygon(Polygon, Paths);
	const Clipper2Lib::Paths64 Grown = Clipper2Lib::InflatePaths(
		Paths, Distance * ClipperScale, Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon, MitreLimit);
	const std::vector<FBuildingPolygon> Pieces = UnionPaths(Grown, Clipper2Lib::FillRule::NonZero);
	if (Pieces.empty())
	{
		return false;
	}
	size_t Largest = 0;
	for (size_t Index = 1; Index < Pieces.size(); ++Index)
	{
		if (PolygonArea(Pieces[Index]) > PolygonArea(Pieces[Largest]))
		{
			Largest = Index;
		}
	}
	Result = Pieces[Largest];
	RestoreCurveVertices(Result.Outline, Curve);
	OrderLikeGeos(Polygon, Distance, Result);
	return true;
}

bool LineIntersectionPoint(const FWorldPoint& P1, const FWorldPoint& P2, const FWorldPoint& Q1, const FWorldPoint& Q2,
	FWorldPoint& Result)
{
	return LineIntersection(P1, P2, Q1, Q2, Result);
}

std::vector<FBuildingPolygon> BufferRound(const FBuildingPolygon& Polygon, double Distance)
{
	if (Polygon.Holes.empty())
	{
		FRing Curve = OffsetCurve(Polygon.Outline, Distance, true);
		if (!Curve.empty() && RingIsSimple(Curve))
		{
			FBuildingPolygon Exact;
			Exact.Outline = std::move(Curve);
			return {Exact};
		}
	}
	constexpr double ArcTolerance = 0.001;
	Clipper2Lib::Paths64 Paths;
	AddOrientedPolygon(Polygon, Paths);
	const Clipper2Lib::Paths64 Grown = Clipper2Lib::InflatePaths(Paths, Distance * ClipperScale, Clipper2Lib::JoinType::Round,
		Clipper2Lib::EndType::Polygon, 2.0, ArcTolerance * ClipperScale);
	return UnionPaths(Grown, Clipper2Lib::FillRule::NonZero);
}

void FMeasuredLine::ComputeLength()
{
	Length = 0.0;
	for (size_t Index = 0; Index + 1 < Points.size(); ++Index)
	{
		Length += std::hypot(Points[Index + 1].X - Points[Index].X, Points[Index + 1].Y - Points[Index].Y);
	}
}

double FMeasuredLine::Project(const FWorldPoint& Point) const
{
	double MinimumDistance = std::numeric_limits<double>::max();
	double Measure = 0.0;
	double SegmentStart = 0.0;
	for (size_t Index = 0; Index + 1 < Points.size(); ++Index)
	{
		const FWorldPoint& A = Points[Index];
		const FWorldPoint& B = Points[Index + 1];
		const double DeltaX = B.X - A.X;
		const double DeltaY = B.Y - A.Y;
		const double SegmentLength = std::hypot(DeltaX, DeltaY);
		const double Distance = PointSegmentDistance(Point, A, B);
		if (Distance < MinimumDistance)
		{
			const double LengthSquared = DeltaX * DeltaX + DeltaY * DeltaY;
			const double Fraction = LengthSquared == 0.0 ? 0.0 : ((Point.X - A.X) * DeltaX + (Point.Y - A.Y) * DeltaY) / LengthSquared;
			MinimumDistance = Distance;
			Measure = SegmentStart + std::clamp(Fraction, 0.0, 1.0) * SegmentLength;
		}
		SegmentStart += SegmentLength;
	}
	return Measure;
}

FWorldPoint FMeasuredLine::Interpolate(double Distance) const
{
	if (Distance <= 0.0 || Points.size() < 2)
	{
		return Points.front();
	}
	double SegmentStart = 0.0;
	for (size_t Index = 0; Index + 1 < Points.size(); ++Index)
	{
		const FWorldPoint& A = Points[Index];
		const FWorldPoint& B = Points[Index + 1];
		const double SegmentLength = std::hypot(B.X - A.X, B.Y - A.Y);
		if (Distance <= SegmentStart + SegmentLength && SegmentLength > 0.0)
		{
			const double Fraction = (Distance - SegmentStart) / SegmentLength;
			return {A.X + Fraction * (B.X - A.X), A.Y + Fraction * (B.Y - A.Y)};
		}
		SegmentStart += SegmentLength;
	}
	return Points.back();
}

int64_t FSegmentGrid::CellOfX(double X) const
{
	return static_cast<int64_t>(std::floor(X / Cell)) - OriginX;
}

int64_t FSegmentGrid::CellOfY(double Y) const
{
	return static_cast<int64_t>(std::floor(Y / Cell)) - OriginY;
}

void FSegmentGrid::InsertSegment(uint32_t Polyline, uint32_t Segment, const FWorldPoint& Start, const FWorldPoint& End)
{
	const FBox2 Box = SegmentBox(Start, End);
	for (int64_t CellY = CellOfY(Box.MinY); CellY <= CellOfY(Box.MaxY); ++CellY)
	{
		for (int64_t CellX = CellOfX(Box.MinX); CellX <= CellOfX(Box.MaxX); ++CellX)
		{
			Cells[static_cast<size_t>(CellY * CountX + CellX)].push_back({Polyline, Segment});
		}
	}
}

void FSegmentGrid::Build(const std::vector<FMeasuredLine>& Polylines, double CellSize)
{
	Lines = &Polylines;
	Cell = CellSize;
	FBox2 Total;
	for (const FMeasuredLine& Line : Polylines)
	{
		for (const FWorldPoint& Point : Line.Points)
		{
			Total.Add(Point);
		}
	}
	Cells.clear();
	if (Total.MinX > Total.MaxX)
	{
		CountX = CountY = 0;
		return;
	}
	OriginX = static_cast<int64_t>(std::floor(Total.MinX / Cell));
	OriginY = static_cast<int64_t>(std::floor(Total.MinY / Cell));
	CountX = CellOfX(Total.MaxX) + 1;
	CountY = CellOfY(Total.MaxY) + 1;
	Cells.assign(static_cast<size_t>(CountX * CountY), {});
	for (size_t LineIndex = 0; LineIndex < Polylines.size(); ++LineIndex)
	{
		const std::vector<FWorldPoint>& Points = Polylines[LineIndex].Points;
		for (size_t Segment = 0; Segment + 1 < Points.size(); ++Segment)
		{
			InsertSegment(static_cast<uint32_t>(LineIndex), static_cast<uint32_t>(Segment), Points[Segment], Points[Segment + 1]);
		}
	}
}

void FSegmentGrid::VisitCell(int64_t CellX, int64_t CellY, int64_t FirstX, int64_t FirstY, const FBox2& Box, double Distance,
	const std::function<void(size_t, const FWorldPoint&, const FWorldPoint&)>& Visit) const
{
	for (const FEntry& Entry : Cells[static_cast<size_t>(CellY * CountX + CellX)])
	{
		const std::vector<FWorldPoint>& Points = (*Lines)[Entry.Polyline].Points;
		const FWorldPoint& Start = Points[Entry.Segment];
		const FWorldPoint& End = Points[Entry.Segment + 1];
		// A segment spanning several cells is only reported from the first of them that the query covers.
		const FBox2 SegmentBounds = SegmentBox(Start, End);
		const int64_t HomeX = std::max<int64_t>(CellOfX(SegmentBounds.MinX), FirstX);
		const int64_t HomeY = std::max<int64_t>(CellOfY(SegmentBounds.MinY), FirstY);
		if (CellX == HomeX && CellY == HomeY && SegmentBounds.DistanceTo(Box) <= Distance)
		{
			Visit(Entry.Polyline, Start, End);
		}
	}
}

void FSegmentGrid::Query(const FBox2& Box, double Distance,
	const std::function<void(size_t, const FWorldPoint&, const FWorldPoint&)>& Visit) const
{
	if (CountX == 0)
	{
		return;
	}
	const FBox2 Reach = Box.Expanded(Distance);
	const int64_t FirstX = std::max<int64_t>(CellOfX(Reach.MinX), 0);
	const int64_t LastX = std::min<int64_t>(CellOfX(Reach.MaxX), CountX - 1);
	const int64_t FirstY = std::max<int64_t>(CellOfY(Reach.MinY), 0);
	const int64_t LastY = std::min<int64_t>(CellOfY(Reach.MaxY), CountY - 1);
	for (int64_t CellY = FirstY; CellY <= LastY; ++CellY)
	{
		for (int64_t CellX = FirstX; CellX <= LastX; ++CellX)
		{
			VisitCell(CellX, CellY, FirstX, FirstY, Box, Distance, Visit);
		}
	}
}

void FStrTree::Build(const std::vector<FBox2>& Boxes)
{
	Nodes.clear();
	Nodes.reserve(Boxes.size() + Boxes.size() / 4 + 8);
	for (size_t Index = 0; Index < Boxes.size(); ++Index)
	{
		FNode Leaf;
		Leaf.Box = Boxes[Index];
		Leaf.Item = static_cast<uint32_t>(Index);
		Nodes.push_back(Leaf);
	}
	size_t Begin = 0;
	size_t End = Nodes.size();
	while (End - Begin > 1)
	{
		CreateParentNodes(Begin, End);
		Begin = End;
		End = Nodes.size();
	}
}

void FStrTree::AddParentNode(size_t GroupBegin, size_t GroupEnd)
{
	FNode Parent;
	Parent.bLeaf = false;
	Parent.FirstChild = static_cast<uint32_t>(GroupBegin);
	Parent.EndChild = static_cast<uint32_t>(GroupEnd);
	for (size_t Child = GroupBegin; Child < GroupEnd; ++Child)
	{
		const FBox2& ChildBox = Nodes[Child].Box;
		Parent.Box.Add({ChildBox.MinX, ChildBox.MinY});
		Parent.Box.Add({ChildBox.MaxX, ChildBox.MaxY});
	}
	Nodes.push_back(Parent);
}

void FStrTree::CreateParentNodes(size_t Begin, size_t End)
{
	constexpr size_t NodeCapacity = 10;
	const size_t ChildCount = End - Begin;
	const size_t MinimumLeafCount = static_cast<size_t>(std::ceil(static_cast<double>(ChildCount) / NodeCapacity));
	const size_t SliceCount = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(MinimumLeafCount))));
	const size_t SliceCapacity = static_cast<size_t>(std::ceil(static_cast<double>(ChildCount) / SliceCount));
	auto First = Nodes.begin() + static_cast<std::ptrdiff_t>(Begin);
	std::sort(First, Nodes.begin() + static_cast<std::ptrdiff_t>(End), [](const FNode& A, const FNode& B) {
		return A.Box.MinX + A.Box.MaxX < B.Box.MinX + B.Box.MaxX;
	});
	for (size_t SliceBegin = Begin; SliceBegin < End; SliceBegin += SliceCapacity)
	{
		const size_t SliceEnd = std::min(SliceBegin + SliceCapacity, End);
		std::sort(Nodes.begin() + static_cast<std::ptrdiff_t>(SliceBegin), Nodes.begin() + static_cast<std::ptrdiff_t>(SliceEnd),
			[](const FNode& A, const FNode& B) { return A.Box.MinY + A.Box.MaxY < B.Box.MinY + B.Box.MaxY; });
		for (size_t GroupBegin = SliceBegin; GroupBegin < SliceEnd; GroupBegin += NodeCapacity)
		{
			AddParentNode(GroupBegin, std::min(GroupBegin + NodeCapacity, SliceEnd));
		}
	}
}

void FStrTree::Query(const FBox2& Box, std::vector<uint32_t>& Result) const
{
	Result.clear();
	if (Nodes.empty() || !Nodes.back().Box.Intersects(Box))
	{
		return;
	}
	QueryNode(Nodes.back(), Box, Result);
}

void FStrTree::QueryNode(const FNode& Node, const FBox2& Box, std::vector<uint32_t>& Result) const
{
	if (Node.bLeaf)
	{
		Result.push_back(Node.Item);
		return;
	}
	for (uint32_t Child = Node.FirstChild; Child < Node.EndChild; ++Child)
	{
		if (Nodes[Child].Box.Intersects(Box))
		{
			QueryNode(Nodes[Child], Box, Result);
		}
	}
}
}
