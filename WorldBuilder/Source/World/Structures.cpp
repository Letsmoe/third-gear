#include "Structures.h"

namespace WorldBuilder
{
void WriteStructure(FTileWriter& Writer, const FStructurePlacement& Placement)
{
	FRecordWriter Record;
	Record.U16(Writer.Name(Placement.Mesh));
	Record.U16(Placement.Variant.empty() ? NoVariant : Writer.Name(Placement.Variant));
	Record.U8(static_cast<uint8_t>(Placement.Kind));
	Record.U8(Placement.Flags);
	Record.Pad(2);
	Record.F32(static_cast<float>(Placement.X - Writer.Bounds().X0));
	Record.F32(static_cast<float>(Placement.Y - Writer.Bounds().Y0));
	Record.F32(Placement.Z);
	for (const float Value : {Placement.Yaw, Placement.Pitch, Placement.Roll, Placement.ScaleX, Placement.ScaleY,
							  Placement.ScaleZ, Placement.Param0, Placement.Param1})
	{
		Record.F32(Value);
	}
	Writer.AddRecord("STRU", std::move(Record.Bytes));
}
}
