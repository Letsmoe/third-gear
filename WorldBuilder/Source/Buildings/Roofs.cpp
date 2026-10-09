#include "ExactFloatingPoint.h"
#include "Roofs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

#include "Earcut.h"
#include "Skeleton.h"

namespace WorldBuilder
{
namespace
{
constexpr double MansardLowerRise = 2.6;
constexpr double MansardLowerDegrees = 70.0;
constexpr double MansardUpperDegrees = 28.0;

/** Degrees as radians. */
double Radians(double Degrees)
{
	return Degrees * std::numbers::pi / 180.0;
}

/** One stretch of a roof profile: its slope in degrees and the time where it ends (none for the last, flat one). */
struct FProfileStep
{
	double SlopeDegrees;
	std::optional<double> EndTime;
};

/** A point of a face polygon while it is cut: x, y and the skeleton time. */
struct FTimedPoint
{
	double X;
	double Y;
	double Time;
};

/** Piecewise slope profile as steps, capped at MaxRoofRise. */
std::vector<FProfileStep> ProfileFor(std::string_view Shape, double PitchDegrees)
{
	if (Shape == "mansard")
	{
		const double LowerTime = MansardLowerRise / std::tan(Radians(MansardLowerDegrees));
		const double UpperStart = MansardLowerRise;
		const double CapTime = LowerTime + (MaxRoofRise - UpperStart) / std::tan(Radians(MansardUpperDegrees));
		return {{MansardLowerDegrees, LowerTime}, {MansardUpperDegrees, CapTime}, {0.0, std::nullopt}};
	}
	const double Pitch = std::max(std::min(PitchDegrees, 65.0), 8.0);
	return {{Pitch, MaxRoofRise / std::tan(Radians(Pitch))}, {0.0, std::nullopt}};
}

/** Height and slope length of the roof at a skeleton time (the distance from the eave line). */
void HeightAndLength(const std::vector<FProfileStep>& Profile, double Time, double& Height, double& Length)
{
	Height = 0.0;
	Length = 0.0;
	double Start = 0.0;
	for (const FProfileStep& Step : Profile)
	{
		const double Span = Step.EndTime.has_value() ? std::min(Time, *Step.EndTime) - Start : Time - Start;
		if (Span <= 0.0)
		{
			break;
		}
		Height += Span * std::tan(Radians(Step.SlopeDegrees));
		Length += Span / std::cos(Radians(Step.SlopeDegrees));
		if (!Step.EndTime.has_value() || Time <= *Step.EndTime)
		{
			break;
		}
		Start = *Step.EndTime;
	}
}

/** The point where the edge from Previous to Current reaches the skeleton time Time; false when the edge is level. */
bool CrossingAt(const FTimedPoint& Previous, const FTimedPoint& Current, double Time, FTimedPoint& Crossing)
{
	const double Span = Current.Time - Previous.Time;
	if (std::abs(Span) <= 1e-12)
	{
		return false;
	}
	const double Fraction = (Time - Previous.Time) / Span;
	Crossing = {Previous.X + (Current.X - Previous.X) * Fraction, Previous.Y + (Current.Y - Previous.Y) * Fraction,
		Previous.Time + (Current.Time - Previous.Time) * Fraction};
	return true;
}

/** Sutherland-Hodgman clip of an (x, y, time) polygon against time = Time. */
std::vector<FTimedPoint> ClipAt(const std::vector<FTimedPoint>& Polygon, double Time, bool bKeepAbove)
{
	std::vector<FTimedPoint> Result;
	for (size_t Index = 0; Index < Polygon.size(); ++Index)
	{
		const FTimedPoint& Current = Polygon[Index];
		const FTimedPoint& Previous = Polygon[(Index + Polygon.size() - 1) % Polygon.size()];
		const bool bCurrentInside = bKeepAbove ? Current.Time >= Time - 1e-9 : Current.Time <= Time + 1e-9;
		const bool bPreviousInside = bKeepAbove ? Previous.Time >= Time - 1e-9 : Previous.Time <= Time + 1e-9;
		FTimedPoint Crossing;
		if (bCurrentInside != bPreviousInside && CrossingAt(Previous, Current, Time, Crossing))
		{
			Result.push_back(Crossing);
		}
		if (bCurrentInside)
		{
			Result.push_back(Current);
		}
	}
	return Result;
}

/** The area of the polygon seen from above. */
double PlanArea(const std::vector<FTimedPoint>& Polygon)
{
	double Sum = 0.0;
	for (size_t Index = 0; Index < Polygon.size(); ++Index)
	{
		const FTimedPoint& A = Polygon[Index];
		const FTimedPoint& B = Polygon[(Index + 1) % Polygon.size()];
		Sum += A.X * B.Y - B.X * A.Y;
	}
	return 0.5 * std::abs(Sum);
}

/** Pieces of a polygon, each inside one slope step of the profile: (step index, piece). */
std::vector<std::pair<size_t, std::vector<FTimedPoint>>> SplitByRegimes(const std::vector<FTimedPoint>& Polygon,
	const std::vector<FProfileStep>& Profile)
{
	std::vector<double> Bounds = {0.0};
	for (const FProfileStep& Step : Profile)
	{
		if (Step.EndTime.has_value())
		{
			Bounds.push_back(*Step.EndTime);
		}
	}
	Bounds.push_back(std::numeric_limits<double>::infinity());
	std::vector<std::pair<size_t, std::vector<FTimedPoint>>> Pieces;
	for (size_t Regime = 0; Regime < Profile.size(); ++Regime)
	{
		const double Low = Bounds[Regime];
		const double High = Bounds[Regime + 1];
		std::vector<FTimedPoint> Piece = Low > 0.0 ? ClipAt(Polygon, Low, true) : Polygon;
		if (High != std::numeric_limits<double>::infinity() && !Piece.empty())
		{
			Piece = ClipAt(Piece, High, false);
		}
		if (Piece.size() >= 3 && PlanArea(Piece) > 1e-4)
		{
			Pieces.emplace_back(Regime, std::move(Piece));
		}
	}
	return Pieces;
}

/** The ring's points oriented counter-clockwise or clockwise (by shoelace area). */
FRing Oriented(const FRing& Ring, bool bCounterClockwise)
{
	double Sum = 0.0;
	for (size_t Index = 0; Index < Ring.size(); ++Index)
	{
		const FWorldPoint& A = Ring[Index];
		const FWorldPoint& B = Ring[(Index + 1) % Ring.size()];
		Sum += A.X * B.Y - B.X * A.Y;
	}
	const double Area = 0.5 * Sum;
	if ((Area > 0.0) == bCounterClockwise)
	{
		return Ring;
	}
	return FRing(Ring.rbegin(), Ring.rend());
}

/** The footprint grown by the overhang with mitred corners, simplified; false if it falls apart. */
bool EaveOutline(const FBuildingPolygon& Footprint, double Overhang, FBuildingPolygon& Outline)
{
	FBuildingPolygon Grown = Footprint;
	if (Overhang > 0.0 && !BufferMitre(Footprint, Overhang, Grown))
	{
		return false;
	}
	if (Grown.Outline.empty())
	{
		return false;
	}
	Outline = SimplifyPolygon(Grown, 0.05);
	return true;
}

/** One face of the roof from a piece of an edge's skeleton face, or nothing when it cannot be triangulated. */
std::optional<FRoofFace> MakeFace(const std::vector<FTimedPoint>& Piece, bool bPlateau, const std::vector<FProfileStep>& Profile,
	const FSkeletonEdge& Edge)
{
	FRoofFace Face;
	Face.Kind = bPlateau ? RoofFacePlateau : RoofFaceSlope;
	std::vector<FWorldPoint> Ring;
	for (const FTimedPoint& Point : Piece)
	{
		double Height = 0.0;
		double Length = 0.0;
		HeightAndLength(Profile, Point.Time, Height, Length);
		FRoofVertex Vertex;
		Vertex.X = Point.X;
		Vertex.Y = Point.Y;
		Vertex.Height = Height;
		if (bPlateau)
		{
			Vertex.U = Point.X;
			Vertex.V = Point.Y;
		}
		else
		{
			Vertex.U = (Point.X - Edge.Origin.X) * Edge.Direction.X + (Point.Y - Edge.Origin.Y) * Edge.Direction.Y;
			Vertex.V = -Length;
		}
		Face.Vertices.push_back(Vertex);
		Ring.push_back({Point.X, Point.Y});
	}
	const std::vector<uint32_t> Indices = TriangulateRing(Ring);
	if (Indices.empty())
	{
		return std::nullopt;
	}
	for (uint32_t Index : Indices)
	{
		Face.Triangles.push_back(static_cast<uint16_t>(Index));
	}
	return Face;
}

/** A face with the skeleton edge and profile step it came from. */
struct FPlaneFace
{
	int EdgeId;
	size_t Regime;
	size_t FaceIndex;
};

/** A plane z = a x + b y + c through a face's vertices, in coordinates relative to Origin (least squares). */
struct FPlane
{
	double OriginX = 0.0;
	double OriginY = 0.0;
	double SlopeX = 0.0;
	double SlopeY = 0.0;
	double Offset = 0.0;

	double At(double X, double Y) const { return SlopeX * (X - OriginX) + SlopeY * (Y - OriginY) + Offset; }
};

/** The least squares plane through the vertices of a face. */
FPlane FitPlane(const FRoofFace& Face)
{
	FPlane Plane;
	const double Count = static_cast<double>(Face.Vertices.size());
	for (const FRoofVertex& Vertex : Face.Vertices)
	{
		Plane.OriginX += Vertex.X / Count;
		Plane.OriginY += Vertex.Y / Count;
		Plane.Offset += Vertex.Height / Count;
	}
	double Sxx = 0.0, Sxy = 0.0, Syy = 0.0, Sxz = 0.0, Syz = 0.0;
	for (const FRoofVertex& Vertex : Face.Vertices)
	{
		const double X = Vertex.X - Plane.OriginX;
		const double Y = Vertex.Y - Plane.OriginY;
		const double Z = Vertex.Height - Plane.Offset;
		Sxx += X * X;
		Sxy += X * Y;
		Syy += Y * Y;
		Sxz += X * Z;
		Syz += Y * Z;
	}
	const double Determinant = Sxx * Syy - Sxy * Sxy;
	if (std::abs(Determinant) > 1e-12)
	{
		Plane.SlopeX = (Sxz * Syy - Syz * Sxy) / Determinant;
		Plane.SlopeY = (Sxx * Syz - Sxy * Sxz) / Determinant;
	}
	return Plane;
}

/** Integer key of a vertex rounded to a millimetre, to find the edges two faces share. */
using FVertexKey = std::array<int64_t, 3>;

/** The vertex rounded to millimetres. */
FVertexKey KeyOf(const FRoofVertex& Vertex)
{
	return {std::llround(Vertex.X * 1000.0), std::llround(Vertex.Y * 1000.0), std::llround(Vertex.Height * 1000.0)};
}

/** A face edge, by the face it belongs to and its end points. */
struct FFaceEdge
{
	size_t Face;
	const FRoofVertex* Start;
	const FRoofVertex* End;
};

/** The edges of the faces that are above the eave line, grouped by the two end points they share. */
class FFaceEdgeIndex
{
public:
	/** Adds the edges of one sloped face, noted as belonging to the plane face with index PlaneIndex. */
	void AddFace(const FRoofFace& Face, size_t PlaneIndex)
	{
		const size_t Count = Face.Vertices.size();
		for (size_t Corner = 0; Corner < Count; ++Corner)
		{
			const FRoofVertex& A = Face.Vertices[Corner];
			const FRoofVertex& B = Face.Vertices[(Corner + 1) % Count];
			if (std::min(A.Height, B.Height) < 0.05 && std::max(A.Height, B.Height) < 0.05)
			{
				continue;
			}
			Edges[GroupOf(A, B)].push_back({PlaneIndex, &A, &B});
		}
	}

	/** The groups of edges in the order the first edge of each was added. */
	const std::vector<std::vector<FFaceEdge>>& Groups() const { return Edges; }

private:
	/** The index of the group for the edge between A and B, in either direction; a new one when there is none yet. */
	size_t GroupOf(const FRoofVertex& A, const FRoofVertex& B)
	{
		FVertexKey First = KeyOf(A);
		FVertexKey Second = KeyOf(B);
		if (Second < First)
		{
			std::swap(First, Second);
		}
		const std::array<int64_t, 6> Key = {First[0], First[1], First[2], Second[0], Second[1], Second[2]};
		auto Found = GroupIndex.find(Key);
		if (Found == GroupIndex.end())
		{
			Found = GroupIndex.emplace(Key, Edges.size()).first;
			Edges.emplace_back();
		}
		return Found->second;
	}

	std::map<std::array<int64_t, 6>, size_t> GroupIndex;
	std::vector<std::vector<FFaceEdge>> Edges;
};

/** The mean of the vertex positions of a face. */
void FaceCentre(const FRoofFace& Face, double& CentreX, double& CentreY)
{
	CentreX = 0.0;
	CentreY = 0.0;
	for (const FRoofVertex& Vertex : Face.Vertices)
	{
		CentreX += Vertex.X / static_cast<double>(Face.Vertices.size());
		CentreY += Vertex.Y / static_cast<double>(Face.Vertices.size());
	}
}

/** True when the plane of First runs above Second beyond their shared line: a peak, not a valley. */
bool IsPeak(const FRoofFace& First, const FRoofFace& Second)
{
	double CentreX = 0.0;
	double CentreY = 0.0;
	FaceCentre(Second, CentreX, CentreY);
	const double Actual = FitPlane(Second).At(CentreX, CentreY);
	const double Extended = FitPlane(First).At(CentreX, CentreY);
	return Extended > Actual + 1e-3;
}

/** Convex edges shared by two sloped faces of different original edges: hips and ridges, as 3D segments. */
std::vector<FRoofCap> HipLines(const std::vector<FRoofFace>& Faces, const std::vector<FPlaneFace>& PlaneFaces)
{
	FFaceEdgeIndex EdgeIndex;
	for (size_t Index = 0; Index < PlaneFaces.size(); ++Index)
	{
		const FRoofFace& Face = Faces[PlaneFaces[Index].FaceIndex];
		if (Face.Kind == RoofFaceSlope)
		{
			EdgeIndex.AddFace(Face, Index);
		}
	}
	std::vector<FRoofCap> Caps;
	for (const std::vector<FFaceEdge>& Sides : EdgeIndex.Groups())
	{
		if (Sides.size() != 2 || PlaneFaces[Sides[0].Face].EdgeId == PlaneFaces[Sides[1].Face].EdgeId)
		{
			continue;
		}
		const FRoofVertex& A = *Sides[0].Start;
		const FRoofVertex& B = *Sides[0].End;
		if (std::hypot(B.X - A.X, B.Y - A.Y) < 0.5 || (std::min(A.Height, B.Height) < 0.3 && std::max(A.Height, B.Height) < 0.3))
		{
			continue;
		}
		const FRoofFace& First = Faces[PlaneFaces[Sides[0].Face].FaceIndex];
		const FRoofFace& Second = Faces[PlaneFaces[Sides[1].Face].FaceIndex];
		if (IsPeak(First, Second))
		{
			Caps.push_back({A.X, A.Y, A.Height, B.X, B.Y, B.Height});
		}
	}
	return Caps;
}

/** Typology classes that the game builds from the kit (WorldKitBuildings.cpp), with the style's eave overhang in metres. */
std::optional<double> KitOverhang(int ClassId)
{
	switch (ClassId)
	{
	case 0:
	case 1:
	case 7:
	case 9:
		return 0.42;
	case 2:
	case 4:
	case 5:
	case 6:
		return 0.32;
	case 10:
	case 15:
		return 0.6;
	default:
		return std::nullopt;
	}
}

/** True for the roof shapes the skeleton roofs make. */
bool IsPitchedShape(std::string_view Shape)
{
	return Shape == "gabled" || Shape == "hipped" || Shape == "half_hipped" || Shape == "pyramidal" || Shape == "mansard"
		|| Shape == "gambrel";
}
}

std::optional<FRoofGeometry> BuildRoof(const FBuildingPolygon& Footprint, std::string_view Shape, double PitchDegrees,
	double Overhang)
{
	FBuildingPolygon Outline;
	if (!EaveOutline(Footprint, Overhang, Outline))
	{
		return std::nullopt;
	}
	std::vector<FRing> Rings = {Oriented(Outline.Outline, true)};
	for (const FRing& Hole : Outline.Holes)
	{
		Rings.push_back(Oriented(Hole, false));
	}
	const FSkeleton Skeleton = StraightSkeleton(Rings);
	if (!Skeleton.bOk || Skeleton.Faces.empty())
	{
		return std::nullopt;
	}
	const std::vector<FProfileStep> Profile = ProfileFor(Shape, PitchDegrees);
	FRoofGeometry Geometry;
	std::vector<FPlaneFace> PlaneFaces;
	for (const FSkeletonFace& SkeletonFace : Skeleton.Faces)
	{
		std::vector<FTimedPoint> Polygon;
		for (int Node : SkeletonFace.Nodes)
		{
			const FSkeletonNode& Point = Skeleton.Nodes[static_cast<size_t>(Node)];
			Polygon.push_back({Point.X, Point.Y, Point.Time});
		}
		const FSkeletonEdge& Edge = Skeleton.Edges[static_cast<size_t>(SkeletonFace.Edge)];
		for (const auto& [Regime, Piece] : SplitByRegimes(Polygon, Profile))
		{
			const bool bPlateau = Profile[Regime].SlopeDegrees == 0.0;
			std::optional<FRoofFace> Face = MakeFace(Piece, bPlateau, Profile, Edge);
			if (!Face.has_value())
			{
				continue;
			}
			Geometry.Faces.push_back(std::move(*Face));
			PlaneFaces.push_back({SkeletonFace.Edge, Regime, Geometry.Faces.size() - 1});
		}
	}
	if (Geometry.Faces.empty())
	{
		return std::nullopt;
	}
	for (const FRoofFace& Face : Geometry.Faces)
	{
		for (const FRoofVertex& Vertex : Face.Vertices)
		{
			Geometry.Top = std::max(Geometry.Top, Vertex.Height);
		}
	}
	Geometry.Caps = HipLines(Geometry.Faces, PlaneFaces);
	return Geometry;
}

std::optional<FRoofGeometry> RoofForBuilding(const FBuildingPolygon& Footprint, int ClassId, std::string_view RoofShape,
	double PitchDegrees)
{
	const std::optional<double> Overhang = KitOverhang(ClassId);
	if (!Overhang.has_value() || !IsPitchedShape(RoofShape))
	{
		return std::nullopt;
	}
	bool bSimpleRectangle = false;
	std::array<FWorldPoint, 4> Rectangle;
	if (Footprint.Holes.empty() && MinimumRotatedRectangle(Footprint, Rectangle))
	{
		const double RectangleArea = std::abs(SignedArea(FRing(Rectangle.begin(), Rectangle.end())));
		bSimpleRectangle = PolygonArea(Footprint) / RectangleArea > 0.93;
	}
	if (RoofShape == "gabled" && bSimpleRectangle)
	{
		return std::nullopt;
	}
	const std::string_view Effective = (RoofShape == "mansard" || RoofShape == "gambrel") ? "mansard" : "hipped";
	return BuildRoof(Footprint, Effective, PitchDegrees, *Overhang);
}
}
