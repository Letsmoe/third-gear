#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Geometry.h"

/**
 * Writes .tgtile files, byte for byte as Tools/osmimport/osmimport/worldtile.py's TileWriter; the format is
 * documented there and read by Plugins/MapRuntime (WorldTileData.cpp).
 */
namespace WorldBuilder
{
constexpr uint32_t TileFormatVersion = 2;
constexpr uint16_t NoVariant = 0xFFFF;

/** SURF height modes: how the runtime gets a vertex's height. */
enum class EHeightMode : uint8_t
{
	Road = 0,     /** road height grid + params[0] */
	Terrain = 1,  /** terrain grid + params[0] */
	Constant = 2, /** params[0] */
	Ramp = 3,     /** params[0] at (params[2], params[3]) to params[1] at (params[4], params[5]) */
};

enum class EPoiKind : uint8_t
{
	Lamp = 0,
	SignalHead = 1,
	Sign = 2,
	ParkedCar = 3,
};

/** One piece of street furniture; see the POIS section in worldtile.py. */
struct FPoi
{
	EPoiKind Kind = EPoiKind::Lamp;
	uint8_t Flags = 0;
	uint16_t Variant = 0;
	uint16_t Variant2 = NoVariant;
	double X = 0.0;
	double Y = 0.0;
	float Z = 0.0f;
	float Yaw = 0.0f;
	float Param0 = 0.0f;
	float Param1 = 0.0f;
	uint32_t Link = 0;
};

class FTileWriter
{
public:
	explicit FTileWriter(const FBox& Bounds);

	const FBox& Bounds() const { return Box; }

	/** The index of a material or model name in the tile's string table, added on first use. */
	uint16_t Name(const std::string& Text);

	/**
	 * The vertex grid: terrain and road heights (Rows x Columns, metres) and land cover weights (three per vertex,
	 * 0..1). Terrain vertices marked in Holes (may be empty) get the hole value.
	 */
	void SetGrid(const std::vector<float>& Terrain, const std::vector<float>& Road, const std::vector<float>& Cover,
				 int Columns, int Rows, float CellSize, const std::vector<uint8_t>& Holes = {});

	/** The parts of the polygons inside the tile as draped surfaces; pieces under 0.05 m² are dropped. */
	void AddSurface(const std::string& Material, const FPolygons& Polygons, EHeightMode Mode,
					const std::vector<float>& Params = {});

	/** The parts of a marking line inside the tile; the phase keeps dashes continuous across tiles. */
	void AddMarking(const std::string& Material, uint8_t Style, const FPolyline& Line, float Width, float DashOn,
					float DashOff);

	void AddPoi(const FPoi& Poi);

	/** Appends a ready-made record to a section (BLDG, BTYP, ROOF, VEGE), for the parts that pack their own. */
	void AddRecord(const char* Tag, std::vector<uint8_t> Record);

	/** Writes the file; throws std::runtime_error when it can't. */
	void Write(const std::string& Path) const;

private:
	FBox Box;
	std::vector<std::string> Names;
	std::unordered_map<std::string, uint16_t> NameIndex;
	std::vector<uint8_t> Grid;
	std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> Records;
};

/** Little-endian packing for records. */
class FRecordWriter
{
public:
	std::vector<uint8_t> Bytes;

	void U8(uint8_t Value) { Bytes.push_back(Value); }
	void U16(uint16_t Value) { Raw(&Value, sizeof(Value)); }
	void U32(uint32_t Value) { Raw(&Value, sizeof(Value)); }
	void U64(uint64_t Value) { Raw(&Value, sizeof(Value)); }
	void F32(float Value) { Raw(&Value, sizeof(Value)); }
	void F64(double Value) { Raw(&Value, sizeof(Value)); }
	void Pad(size_t Count) { Bytes.insert(Bytes.end(), Count, 0); }
	void Raw(const void* Data, size_t Size)
	{
		const uint8_t* Start = static_cast<const uint8_t*>(Data);
		Bytes.insert(Bytes.end(), Start, Start + Size);
	}
};
}
