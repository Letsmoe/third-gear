#include "BuildingTiles.h"

#include <algorithm>
#include <cmath>

namespace WorldBuilder
{
namespace
{
/** Python's round() on the u8 fields: half to even, which the default floating point rounding mode does. */
uint8_t RoundToByte(double Value)
{
	return static_cast<uint8_t>(std::clamp(std::nearbyint(Value), 0.0, 255.0));
}

/** The lowest ground along the outline: conformed inside the window, the record's terrain height outside it. */
float BaseHeight(const FBuilding& Building, const FHeightGrid& Ground, const FBox& Window)
{
	double Lowest = 1e300;
	bool bAllInside = true;
	for (const FWorldPoint& Point : Building.Footprint.Outline)
	{
		const bool bInside = Point.X >= Window.X0 && Point.X <= Window.X1 && Point.Y >= Window.Y0 && Point.Y <= Window.Y1;
		if (!bInside)
		{
			bAllInside = false;
			continue;
		}
		Lowest = std::min(Lowest, Ground.Sample(Point.X, Point.Y));
	}
	if (!bAllInside)
	{
		Lowest = std::min(Lowest, static_cast<double>(Building.Record.BaseZ));
	}
	return static_cast<float>(Lowest);
}

/** Rings relative to the tile corner, as SURF packs them. */
void PackRings(FRecordWriter& Record, const FBuildingPolygon& Footprint, const FBox& TileBox)
{
	Record.U32(static_cast<uint32_t>(1 + Footprint.Holes.size()));
	auto PackRing = [&](const FRing& Ring) {
		Record.U32(static_cast<uint32_t>(Ring.size()));
		for (const FWorldPoint& Point : Ring)
		{
			Record.F32(static_cast<float>(Point.X - TileBox.X0));
			Record.F32(static_cast<float>(Point.Y - TileBox.Y0));
		}
	};
	PackRing(Footprint.Outline);
	for (const FRing& Hole : Footprint.Holes)
	{
		PackRing(Hole);
	}
}

void WriteBuildingRecord(FTileWriter& Writer, const FBuilding& Building, float BaseZ, const FBox& TileBox)
{
	const FBuildingRecord& Values = Building.Record;
	FRecordWriter Record;
	Record.U64(static_cast<uint64_t>(Building.OsmId));
	Record.U16(Writer.Name(Values.Facade));
	Record.U16(Writer.Name(Values.RoofSection));
	Record.U8(Values.RoofShape);
	Record.U8(Values.Tint);
	Record.U8(Values.Variation);
	Record.Pad(1);
	Record.F32(BaseZ);
	Record.F32(Values.EaveHeight);
	for (const FWorldPoint& Corner : Values.RoofRectangle)
	{
		const bool bGabled = Values.bHasRoofRectangle;
		Record.F32(bGabled ? static_cast<float>(Corner.X - TileBox.X0) : 0.0f);
		Record.F32(bGabled ? static_cast<float>(Corner.Y - TileBox.Y0) : 0.0f);
	}
	PackRings(Record, Building.Footprint, TileBox);
	Writer.AddRecord("BLDG", std::move(Record.Bytes));
}

void WriteTypeRecord(FTileWriter& Writer, const FBuilding& Building)
{
	const FBuildingType& Type = Building.Type;
	FRecordWriter Record;
	Record.U64(static_cast<uint64_t>(Building.OsmId));
	Record.U8(static_cast<uint8_t>(Type.ClassId));
	Record.U8(static_cast<uint8_t>(Type.RoofShape));
	Record.U8(RoundToByte(Type.PitchDegrees));
	Record.U8(static_cast<uint8_t>(Type.Storeys));
	Record.U8(static_cast<uint8_t>(Type.AtticLevels));
	Record.U8(static_cast<uint8_t>(Type.Flags));
	Record.U8(static_cast<uint8_t>(Type.TagBits));
	Record.U8(RoundToByte(Type.Plinth / 0.05));
	Record.F32(static_cast<float>(Type.StoreyHeight));
	Record.F32(static_cast<float>(Type.GroundHeight));
	Record.F32(static_cast<float>(Type.EaveHeight));
	Record.F32(static_cast<float>(Type.RidgeYaw));
	Record.F32(static_cast<float>(Type.FrontYaw));
	Writer.AddRecord("BTYP", std::move(Record.Bytes));
}

void WriteRoofRecord(FTileWriter& Writer, const FBuilding& Building, const FRoofGeometry& Roof, const FBox& TileBox)
{
	FRecordWriter Record;
	Record.U64(static_cast<uint64_t>(Building.OsmId));
	Record.U32(static_cast<uint32_t>(Roof.Faces.size()));
	Record.U32(static_cast<uint32_t>(Roof.Caps.size()));
	for (const FRoofFace& Face : Roof.Faces)
	{
		Record.U8(Face.Kind);
		Record.Pad(1);
		Record.U16(static_cast<uint16_t>(Face.Vertices.size()));
		Record.U16(static_cast<uint16_t>(Face.Triangles.size() / 3));
		Record.U16(0);
		for (const FRoofVertex& Vertex : Face.Vertices)
		{
			Record.F32(static_cast<float>(Vertex.X - TileBox.X0));
			Record.F32(static_cast<float>(Vertex.Y - TileBox.Y0));
			Record.F32(static_cast<float>(Vertex.Height));
			Record.F32(static_cast<float>(Vertex.U));
			Record.F32(static_cast<float>(Vertex.V));
		}
		for (const uint16_t Index : Face.Triangles)
		{
			Record.U16(Index);
		}
	}
	for (const FRoofCap& Cap : Roof.Caps)
	{
		Record.F32(static_cast<float>(Cap[0] - TileBox.X0));
		Record.F32(static_cast<float>(Cap[1] - TileBox.Y0));
		Record.F32(static_cast<float>(Cap[2]));
		Record.F32(static_cast<float>(Cap[3] - TileBox.X0));
		Record.F32(static_cast<float>(Cap[4] - TileBox.Y0));
		Record.F32(static_cast<float>(Cap[5]));
	}
	Writer.AddRecord("ROOF", std::move(Record.Bytes));
}
}

void WriteBuilding(FTileWriter& Writer, const FBuilding& Building, const FHeightGrid& Ground, const FBox& Window,
				   const FBox& TileBox)
{
	WriteBuildingRecord(Writer, Building, BaseHeight(Building, Ground, Window), TileBox);
	WriteTypeRecord(Writer, Building);
	if (Building.Roof.has_value())
	{
		WriteRoofRecord(Writer, Building, *Building.Roof, TileBox);
	}
}
}
