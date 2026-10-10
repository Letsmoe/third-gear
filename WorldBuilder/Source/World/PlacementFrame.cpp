#include "PlacementFrame.h"

#include <algorithm>
#include <cmath>

namespace WorldBuilder
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double DegreesPerRadian = 180.0 / Pi;

double Cross(const FWorldPoint& Origin, const FWorldPoint& A, const FWorldPoint& B)
{
	return (A.X - Origin.X) * (B.Y - Origin.Y) - (A.Y - Origin.Y) * (B.X - Origin.X);
}

/** The convex hull of points (Andrew's monotone chain), counter-clockwise in a y-up frame. */
FPolyline ConvexHull(FPolyline Points)
{
	std::sort(Points.begin(), Points.end(), [](const FWorldPoint& A, const FWorldPoint& B) {
		return A.X < B.X || (A.X == B.X && A.Y < B.Y);
	});
	if (Points.size() < 3)
	{
		return Points;
	}
	FPolyline Hull(2 * Points.size());
	size_t Count = 0;
	for (size_t Index = 0; Index < Points.size(); ++Index)
	{
		while (Count >= 2 && Cross(Hull[Count - 2], Hull[Count - 1], Points[Index]) <= 0.0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index];
	}
	const size_t Lower = Count + 1;
	for (size_t Index = Points.size() - 1; Index > 0; --Index)
	{
		while (Count >= Lower && Cross(Hull[Count - 2], Hull[Count - 1], Points[Index - 1]) <= 0.0)
		{
			--Count;
		}
		Hull[Count++] = Points[Index - 1];
	}
	Hull.resize(Count - 1);
	return Hull;
}
}

double PositionNoise(double X, double Y, uint64_t Salt)
{
	const uint64_t Bits = (static_cast<uint64_t>(std::llround(X * 100.0)) * 0x9E3779B97F4A7C15ull)
		^ (static_cast<uint64_t>(std::llround(Y * 100.0)) * 0xBF58476D1CE4E5B9ull) ^ (Salt * 0x94D049BB133111EBull);
	return static_cast<double>((Bits >> 11) % 1000000) / 1000000.0;
}

FPlacementFrame::FPlacementFrame(double InOriginX, double InOriginY, double InBackX, double InBackY)
	: OriginX(InOriginX), OriginY(InOriginY)
{
	const double Length = std::hypot(InBackX, InBackY);
	BackX = Length > 0.0 ? InBackX / Length : 0.0;
	BackY = Length > 0.0 ? InBackY / Length : -1.0;
	// The yaw that turns the kit's +v (Unreal local -Y) onto the back direction; local +X then lies along the u axis.
	const double Yaw = std::atan2(BackX, -BackY);
	CosYaw = std::cos(Yaw);
	SinYaw = std::sin(Yaw);
	FrameYawDegrees = Yaw * DegreesPerRadian;
}

FWorldPoint FPlacementFrame::ToWorld(double U, double V) const
{
	return {OriginX + U * CosYaw + V * BackX, OriginY + U * SinYaw + V * BackY};
}

FStructurePlacement FPlacementFrame::Piece(const std::string& Mesh, double U, double V, float Z, double RotationDegrees,
										   const std::string& Variant) const
{
	const FWorldPoint Position = ToWorld(U, V);
	FStructurePlacement Placement;
	Placement.Mesh = Mesh;
	Placement.Variant = Variant;
	Placement.X = Position.X;
	Placement.Y = Position.Y;
	Placement.Z = Z;
	Placement.Yaw = static_cast<float>(FrameYawDegrees - RotationDegrees);
	return Placement;
}

FWorldPoint FOrientedRectangle::Point(double First, double Second) const
{
	return {Centre.X + First * Axis.X - Second * Axis.Y, Centre.Y + First * Axis.Y + Second * Axis.X};
}

FOrientedRectangle MinimumAreaRectangle(const FPolyline& Ring)
{
	FOrientedRectangle Best;
	const FPolyline Hull = ConvexHull(Ring);
	if (Hull.size() < 3)
	{
		return Best;
	}
	double BestArea = 1e300;
	for (size_t Index = 0; Index < Hull.size(); ++Index)
	{
		const FWorldPoint& From = Hull[Index];
		const FWorldPoint& To = Hull[(Index + 1) % Hull.size()];
		const double Length = std::hypot(To.X - From.X, To.Y - From.Y);
		if (Length < 1e-9)
		{
			continue;
		}
		const double AxisX = (To.X - From.X) / Length;
		const double AxisY = (To.Y - From.Y) / Length;
		double MinFirst = 1e300;
		double MaxFirst = -1e300;
		double MinSecond = 1e300;
		double MaxSecond = -1e300;
		for (const FWorldPoint& Point : Hull)
		{
			const double First = (Point.X - From.X) * AxisX + (Point.Y - From.Y) * AxisY;
			const double Second = -(Point.X - From.X) * AxisY + (Point.Y - From.Y) * AxisX;
			MinFirst = std::min(MinFirst, First);
			MaxFirst = std::max(MaxFirst, First);
			MinSecond = std::min(MinSecond, Second);
			MaxSecond = std::max(MaxSecond, Second);
		}
		const double Area = (MaxFirst - MinFirst) * (MaxSecond - MinSecond);
		if (Area >= BestArea)
		{
			continue;
		}
		BestArea = Area;
		const double CentreFirst = (MinFirst + MaxFirst) / 2.0;
		const double CentreSecond = (MinSecond + MaxSecond) / 2.0;
		Best.Axis = {AxisX, AxisY};
		Best.Centre = {From.X + CentreFirst * AxisX - CentreSecond * AxisY, From.Y + CentreFirst * AxisY + CentreSecond * AxisX};
		Best.HalfFirst = (MaxFirst - MinFirst) / 2.0;
		Best.HalfSecond = (MaxSecond - MinSecond) / 2.0;
	}
	return Best;
}
}
