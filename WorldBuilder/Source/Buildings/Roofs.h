#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "BuildingGeometry.h"

/**
 * Pitched roofs of any outline from the straight skeleton (roofs.py): planes with a texture mapping, the hip and
 * ridge lines for the ridge caps, and the profiles of a plain slope, a mansard and a slope that flattens out at a
 * maximum height.
 *
 * The skeleton is built on the footprint grown by the eave overhang, so the roof planes start at the eave line at
 * height 0; the game puts that line at the wall top.
 */
namespace WorldBuilder
{
constexpr double MaxRoofRise = 7.0;

/** What a roof face is made of. */
enum ERoofFaceKind : uint8_t
{
	/** The roof tile material, v runs up the slope. */
	RoofFaceSlope = 0,
	/** Flat top where the rise is capped, flat roof material, uv in plan metres. */
	RoofFacePlateau = 1,
};

/** One vertex of a roof face: x and y in world metres, height above the eave line, and the texture coordinates. */
struct FRoofVertex
{
	double X = 0.0;
	double Y = 0.0;
	double Height = 0.0;
	double U = 0.0;
	double V = 0.0;
};

struct FRoofFace
{
	uint8_t Kind = RoofFaceSlope;
	std::vector<FRoofVertex> Vertices;
	/** Three indices into the vertices per triangle. */
	std::vector<uint16_t> Triangles;
};

/** A hip or ridge line: x0, y0, z0, x1, y1, z1. */
using FRoofCap = std::array<double, 6>;

struct FRoofGeometry
{
	std::vector<FRoofFace> Faces;
	std::vector<FRoofCap> Caps;
	double Top = 0.0;
};

/** The skeleton roof of the footprint grown by Overhang, or nothing when the skeleton fails. Shape is "mansard" or anything else for a plain slope. */
std::optional<FRoofGeometry> BuildRoof(const FBuildingPolygon& Footprint, std::string_view Shape, double PitchDegrees,
	double Overhang);

/**
 * The skeleton roof for a building the game builds from the kit, or nothing when the game's own roof does: flat
 * roofs, classes without a kit style, and gabled roofs over a plain rectangle (they need gable end walls).
 */
std::optional<FRoofGeometry> RoofForBuilding(const FBuildingPolygon& Footprint, int ClassId, std::string_view RoofShape,
	double PitchDegrees);
}
