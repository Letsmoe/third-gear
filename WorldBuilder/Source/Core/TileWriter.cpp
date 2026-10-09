#include "TileWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include <zlib.h>

namespace WorldBuilder
{
namespace
{
constexpr uint16_t HoleValue = 0xFFFF;
constexpr double MinimumSurfaceArea = 0.05;
constexpr double MinimumMarkingLength = 0.3;
/** The sections in file order after NAME and GRID; each holds a u32 record count and the records. */
const char* const RecordSections[] = {"SURF", "MARK", "BLDG", "BTYP", "ROOF", "VEGE", "POIS"};

/** Heights as centimetres above the base, clamped to the u16 range below the hole value. */
uint16_t ToCentimetres(float Height, float Base)
{
	const double Centimetres = std::round((Height - Base) * 100.0);
	return static_cast<uint16_t>(std::clamp(Centimetres, 0.0, static_cast<double>(HoleValue - 1)));
}

/** A ring relative to the tile corner, without the repeated end point. */
void PackRing(FRecordWriter& Writer, const FPolyline& Ring, const FBox& Box)
{
	Writer.U32(static_cast<uint32_t>(Ring.size()));
	for (const FWorldPoint& Point : Ring)
	{
		Writer.F32(static_cast<float>(Point.X - Box.X0));
		Writer.F32(static_cast<float>(Point.Y - Box.Y0));
	}
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& Raw)
{
	uLongf PackedSize = compressBound(static_cast<uLong>(Raw.size()));
	std::vector<uint8_t> Packed(PackedSize);
	if (compress2(Packed.data(), &PackedSize, Raw.data(), static_cast<uLong>(Raw.size()), 6) != Z_OK)
	{
		throw std::runtime_error("zlib compression failed");
	}
	Packed.resize(PackedSize);
	return Packed;
}
}

FTileWriter::FTileWriter(const FBox& Bounds) : Box(Bounds) {}

uint16_t FTileWriter::Name(const std::string& Text)
{
	const auto Found = NameIndex.find(Text);
	if (Found != NameIndex.end())
	{
		return Found->second;
	}
	const uint16_t Index = static_cast<uint16_t>(Names.size());
	Names.push_back(Text);
	NameIndex.emplace(Text, Index);
	return Index;
}

void FTileWriter::SetGrid(const std::vector<float>& Terrain, const std::vector<float>& Road,
						  const std::vector<float>& Cover, int Columns, int Rows, float CellSize,
						  const std::vector<uint8_t>& Holes)
{
	const float Lowest = std::min(*std::min_element(Terrain.begin(), Terrain.end()),
								  *std::min_element(Road.begin(), Road.end()));
	const float Base = std::floor(Lowest);
	FRecordWriter Writer;
	Writer.U32(static_cast<uint32_t>(Columns));
	Writer.U32(static_cast<uint32_t>(Rows));
	Writer.F32(CellSize);
	Writer.F32(Base);
	for (size_t Index = 0; Index < Terrain.size(); ++Index)
	{
		const bool bHole = !Holes.empty() && Holes[Index] != 0;
		Writer.U16(bHole ? HoleValue : ToCentimetres(Terrain[Index], Base));
	}
	for (const float Height : Road)
	{
		Writer.U16(ToCentimetres(Height, Base));
	}
	for (const float Weight : Cover)
	{
		Writer.U8(static_cast<uint8_t>(std::clamp(std::round(Weight * 255.0), 0.0, 255.0)));
	}
	Grid = std::move(Writer.Bytes);
}

void FTileWriter::AddSurface(const std::string& Material, const FPolygons& Polygons, EHeightMode Mode,
							 const std::vector<float>& Params)
{
	for (const FPolygonWithHoles& Polygon : ToPolygons(ClipToBox(Polygons, Box)))
	{
		if (PolygonArea(Polygon) < MinimumSurfaceArea)
		{
			continue;
		}
		FRecordWriter Writer;
		Writer.U16(Name(Material));
		Writer.U8(static_cast<uint8_t>(Mode));
		Writer.Pad(1);
		for (size_t Index = 0; Index < 6; ++Index)
		{
			Writer.F32(Index < Params.size() ? Params[Index] : 0.0f);
		}
		Writer.U32(static_cast<uint32_t>(Polygon.Rings.size()));
		for (const FPolyline& Ring : Polygon.Rings)
		{
			PackRing(Writer, Ring, Box);
		}
		Records["SURF"].push_back(std::move(Writer.Bytes));
	}
}

void FTileWriter::AddMarking(const std::string& Material, uint8_t Style, const FPolyline& Line, float Width,
							 float DashOn, float DashOff)
{
	for (const FPolylinePiece& Piece : ClipPolylineToBox(Line, Box))
	{
		if (PolylineLength(Piece.Points) < MinimumMarkingLength)
		{
			continue;
		}
		FRecordWriter Writer;
		Writer.U16(Name(Material));
		Writer.U8(Style);
		Writer.Pad(1);
		Writer.F32(Width);
		Writer.F32(DashOn);
		Writer.F32(DashOff);
		Writer.F32(static_cast<float>(Piece.StartDistance));
		Writer.U32(static_cast<uint32_t>(Piece.Points.size()));
		for (const FWorldPoint& Point : Piece.Points)
		{
			Writer.F32(static_cast<float>(Point.X - Box.X0));
			Writer.F32(static_cast<float>(Point.Y - Box.Y0));
		}
		Records["MARK"].push_back(std::move(Writer.Bytes));
	}
}

void FTileWriter::AddPoi(const FPoi& Poi)
{
	FRecordWriter Writer;
	Writer.U8(static_cast<uint8_t>(Poi.Kind));
	Writer.U8(Poi.Flags);
	Writer.U16(Poi.Variant);
	Writer.U16(Poi.Variant2);
	Writer.Pad(2);
	Writer.F32(static_cast<float>(Poi.X - Box.X0));
	Writer.F32(static_cast<float>(Poi.Y - Box.Y0));
	Writer.F32(Poi.Z);
	Writer.F32(Poi.Yaw);
	Writer.F32(Poi.Param0);
	Writer.F32(Poi.Param1);
	Writer.U32(Poi.Link);
	Records["POIS"].push_back(std::move(Writer.Bytes));
}

void FTileWriter::AddRecord(const char* Tag, std::vector<uint8_t> Record)
{
	Records[Tag].push_back(std::move(Record));
}

void FTileWriter::Write(const std::string& Path) const
{
	FRecordWriter NameTable;
	NameTable.U32(static_cast<uint32_t>(Names.size()));
	for (const std::string& Text : Names)
	{
		NameTable.U16(static_cast<uint16_t>(Text.size()));
		NameTable.Raw(Text.data(), Text.size());
	}
	std::vector<std::pair<std::string, std::vector<uint8_t>>> Sections = {{"NAME", NameTable.Bytes}, {"GRID", Grid}};
	for (const char* Tag : RecordSections)
	{
		FRecordWriter Section;
		const auto Found = Records.find(Tag);
		const size_t Count = Found == Records.end() ? 0 : Found->second.size();
		Section.U32(static_cast<uint32_t>(Count));
		for (size_t Index = 0; Index < Count; ++Index)
		{
			Section.Raw(Found->second[Index].data(), Found->second[Index].size());
		}
		Sections.emplace_back(Tag, std::move(Section.Bytes));
	}

	FRecordWriter File;
	File.Raw("TGT1", 4);
	File.U32(TileFormatVersion);
	File.F64(Box.X0);
	File.F64(Box.Y0);
	File.F32(static_cast<float>(Box.X1 - Box.X0));
	File.F32(static_cast<float>(Box.Y1 - Box.Y0));
	File.U32(static_cast<uint32_t>(Sections.size()));
	for (const auto& [Tag, Raw] : Sections)
	{
		const std::vector<uint8_t> Packed = Compress(Raw);
		File.Raw(Tag.data(), 4);
		File.U32(static_cast<uint32_t>(Raw.size()));
		File.U32(static_cast<uint32_t>(Packed.size()));
		File.Raw(Packed.data(), Packed.size());
	}
	std::FILE* Output = std::fopen(Path.c_str(), "wb");
	if (Output == nullptr)
	{
		throw std::runtime_error("cannot write " + Path);
	}
	std::fwrite(File.Bytes.data(), 1, File.Bytes.size(), Output);
	std::fclose(Output);
}
}
