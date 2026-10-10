#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Geometry.h"
#include "Structures.h"

/**
 * Helpers for placing the building kit's pieces, which are modelled in a frame of their own (Blender, metres, Z up,
 * the piece's front toward -Y) and placed in the world (x east, y south) as Unreal imports them: Blender's Y is
 * flipped, so a kit piece's Blender -Y side is local +Y in Unreal and a Blender rotation of +r degrees is an Unreal yaw
 * of -r.
 */
namespace WorldBuilder
{
/** A number from 0 to 1 fixed by a position, so placements don't change with the order they are made in. */
double PositionNoise(double X, double Y, uint64_t Salt);

/**
 * A kit frame laid on the world: origin, and the direction of the frame's +v axis (Blender +Y, "behind" a piece whose
 * front faces -Y). Pieces are given as the kit's layout code gives them: (u, v) on the frame's plane, a height and a
 * Blender rotation.
 */
class FPlacementFrame
{
public:
	/** Origin in world metres; BackX and BackY the direction of +v in the world (normalised here). */
	FPlacementFrame(double OriginX, double OriginY, double BackX, double BackY);

	/** The piece at (U, V) metres in the frame and Z absolute, turned by RotationDegrees (Blender, counter-clockwise). */
	FStructurePlacement Piece(const std::string& Mesh, double U, double V, float Z, double RotationDegrees = 0.0,
							  const std::string& Variant = std::string()) const;

	/** World position of a frame point. */
	FWorldPoint ToWorld(double U, double V) const;

	/** World direction of the frame's +u axis (Blender's +X) and of its +v axis. */
	FWorldPoint UAxis() const { return {CosYaw, SinYaw}; }
	FWorldPoint VAxis() const { return {BackX, BackY}; }

private:
	double OriginX;
	double OriginY;
	double BackX;
	double BackY;
	double CosYaw;
	double SinYaw;
	double FrameYawDegrees;
};

/** A rectangle at any angle: centre, the unit direction of its first axis, and the half sizes along both axes. */
struct FOrientedRectangle
{
	FWorldPoint Centre;
	/** Unit vector of the first axis; the second is that turned a quarter turn (-y, x). */
	FWorldPoint Axis;
	double HalfFirst = 0.0;
	double HalfSecond = 0.0;

	double Area() const { return 4.0 * HalfFirst * HalfSecond; }
	/** The world point at (First, Second) metres from the centre along the two axes. */
	FWorldPoint Point(double First, double Second) const;
};

/** The smallest-area rectangle around a ring (rotating calipers over the convex hull). */
FOrientedRectangle MinimumAreaRectangle(const FPolyline& Ring);

/** True when any of a grid of points over the rectangle (spacing Step) is inside the polygon set test. */
template <typename FTest>
bool RectangleHits(const FOrientedRectangle& Rectangle, double Step, FTest&& Test)
{
	for (double First = -Rectangle.HalfFirst; First <= Rectangle.HalfFirst + 1e-9; First += Step)
	{
		for (double Second = -Rectangle.HalfSecond; Second <= Rectangle.HalfSecond + 1e-9; Second += Step)
		{
			const FWorldPoint Point = Rectangle.Point(First, Second);
			if (Test(Point.X, Point.Y))
			{
				return true;
			}
		}
	}
	return false;
}
}
