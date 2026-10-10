#pragma once

#include <cstdint>
#include <string>

#include "TileWriter.h"

/**
 * Placements of the building kit's structure pieces (Tools/buildingkit/generate_structures.py, imported by
 * Scripts/import_structures.py): the STRU section, written into the tiles for small structures and into
 * structures.tgtile for the region's tall ones. Read by Plugins/MapRuntime (WorldStructures.cpp).
 */
namespace WorldBuilder
{
enum class EStructureKind : uint8_t
{
	/** A piece standing still, rotated by yaw, pitch and roll (Unreal's rotator). */
	Static = 0,
	/** A turbine rotor: yaw, then Roll is the tilt of the shaft (front end up), Param0 rpm, Param1 start angle. */
	Rotor = 1,
	/** A conductor from the position along yaw and pitch, ScaleX metres long, Param0 sag metres. */
	Wire = 2,
};

struct FStructurePlacement
{
	/** The piece as <family>_<piece>, e.g. pylons_Donau_Anchor. */
	std::string Mesh;
	/** The material variant (a petrol brand, a poster), or empty. */
	std::string Variant;
	EStructureKind Kind = EStructureKind::Static;
	uint8_t Flags = 0;
	/** World metres, x east and y south; Z absolute. */
	double X = 0.0;
	double Y = 0.0;
	float Z = 0.0f;
	/** Degrees, as Unreal's FRotator. */
	float Yaw = 0.0f;
	float Pitch = 0.0f;
	float Roll = 0.0f;
	float ScaleX = 1.0f;
	float ScaleY = 1.0f;
	float ScaleZ = 1.0f;
	float Param0 = 0.0f;
	float Param1 = 0.0f;
};

/**
 * Appends one placement to the writer's STRU section, its position relative to the writer's corner. Record (52 bytes):
 * u16 mesh name, u16 variant name (NoVariant), u8 kind, u8 flags, 2 pad, f32 x, y, z, yaw, pitch, roll, scale x, y, z,
 * param0, param1.
 */
void WriteStructure(FTileWriter& Writer, const FStructurePlacement& Placement);
}
