#include "WorldTileData.h"

#include "Misc/Compression.h"
#include "Misc/FileHelper.h"

namespace
{
constexpr uint32 TileFormatVersion = 2;

/** Four ASCII characters as the little-endian u32 they are stored as. */
constexpr uint32 MakeTag(const char (&Text)[5])
{
	return uint32(uint8(Text[0])) | uint32(uint8(Text[1])) << 8 | uint32(uint8(Text[2])) << 16 | uint32(uint8(Text[3])) << 24;
}

/** Little-endian reader over a byte buffer that remembers the first overrun instead of crashing. */
class FByteReader
{
public:
	explicit FByteReader(TConstArrayView<uint8> InBytes) : Bytes(InBytes) {}

	bool HasFailed() const { return bFailed; }
	bool IsAtEnd() const { return Offset >= Bytes.Num(); }

	/** Copies Count bytes into Destination, or fails. */
	void Read(void* Destination, int64 Count)
	{
		if (bFailed || Count < 0 || Offset + Count > Bytes.Num())
		{
			bFailed = true;
			FMemory::Memzero(Destination, FMath::Max<int64>(Count, 0));
			return;
		}
		FMemory::Memcpy(Destination, Bytes.GetData() + Offset, Count);
		Offset += Count;
	}

	template <typename T>
	T Get()
	{
		T Value;
		Read(&Value, sizeof(T));
		return Value;
	}

	void Skip(int64 Count)
	{
		if (Offset + Count > Bytes.Num())
		{
			bFailed = true;
			return;
		}
		Offset += Count;
	}

	/** Reads Count elements of T into Out. */
	template <typename T>
	void GetArray(TArray<T>& Out, int64 Count)
	{
		if (Count < 0 || Offset + Count * int64(sizeof(T)) > Bytes.Num())
		{
			bFailed = true;
			return;
		}
		Out.SetNumUninitialized(Count);
		Read(Out.GetData(), Count * sizeof(T));
	}

	/** Reads a point list: u32 count, then f32 x/y pairs. */
	void GetPoints(TArray<FVector2f>& Out)
	{
		const uint32 Count = Get<uint32>();
		GetArray(Out, Count);
	}

private:
	TConstArrayView<uint8> Bytes;
	int64 Offset = 0;
	bool bFailed = false;
};

void ReadPolygon(FByteReader& Reader, FWorldPolygon& Polygon)
{
	const uint32 RingCount = Reader.Get<uint32>();
	if (RingCount > 100000)
	{
		Reader.Skip(MAX_int32); // forces failure
		return;
	}
	Polygon.Rings.SetNum(RingCount);
	for (TArray<FVector2f>& Ring : Polygon.Rings)
	{
		Reader.GetPoints(Ring);
	}
}

void ReadNames(FByteReader& Reader, TArray<FString>& Names)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		const uint16 Length = Reader.Get<uint16>();
		TArray<uint8> Utf8;
		Reader.GetArray(Utf8, Length);
		Names.Add(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()), Utf8.Num())));
	}
}

void ReadGrid(FByteReader& Reader, FWorldTileGrid& Grid)
{
	Grid.NumX = Reader.Get<uint32>();
	Grid.NumY = Reader.Get<uint32>();
	Grid.CellSize = Reader.Get<float>();
	Grid.BaseZ = Reader.Get<float>();
	const int64 Count = int64(Grid.NumX) * Grid.NumY;
	Reader.GetArray(Grid.TerrainCm, Count);
	Reader.GetArray(Grid.RoadCm, Count);
	Reader.GetArray(Grid.Cover, Count * 3);
}

void ReadSurfaces(FByteReader& Reader, TArray<FWorldSurface>& Surfaces)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		FWorldSurface& Surface = Surfaces.AddDefaulted_GetRef();
		Surface.Material = Reader.Get<uint16>();
		Surface.HeightMode = static_cast<EWorldSurfaceHeight>(Reader.Get<uint8>());
		Reader.Skip(1);
		Reader.Read(Surface.Params, sizeof(Surface.Params));
		ReadPolygon(Reader, Surface.Polygon);
	}
}

void ReadMarkings(FByteReader& Reader, TArray<FWorldMarking>& Markings)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		FWorldMarking& Marking = Markings.AddDefaulted_GetRef();
		Marking.Material = Reader.Get<uint16>();
		Marking.Style = Reader.Get<uint8>();
		Reader.Skip(1);
		Marking.Width = Reader.Get<float>();
		Marking.DashOn = Reader.Get<float>();
		Marking.DashOff = Reader.Get<float>();
		Marking.Phase = Reader.Get<float>();
		Reader.GetPoints(Marking.Points);
	}
}

void ReadBuildings(FByteReader& Reader, TArray<FWorldBuilding>& Buildings)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		FWorldBuilding& Building = Buildings.AddDefaulted_GetRef();
		Building.OsmId = Reader.Get<uint64>();
		Building.Facade = Reader.Get<uint16>();
		Building.Roof = Reader.Get<uint16>();
		Building.RoofShape = static_cast<EWorldRoofShape>(Reader.Get<uint8>());
		Building.Tint = Reader.Get<uint8>();
		Building.Variation = Reader.Get<uint8>();
		Reader.Skip(1);
		Building.BaseZ = Reader.Get<float>();
		Building.EaveHeight = Reader.Get<float>();
		Reader.Read(Building.RoofRectangle, sizeof(Building.RoofRectangle));
		ReadPolygon(Reader, Building.Footprint);
	}
}

/** BTYP: one typing record per BLDG record, in the same order; fills the typing fields of the buildings. */
void ReadBuildingTypes(FByteReader& Reader, TArray<FWorldBuilding>& Buildings)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		const uint64 OsmId = Reader.Get<uint64>();
		FWorldBuilding* Building = Buildings.IsValidIndex(Index) && Buildings[Index].OsmId == OsmId ? &Buildings[Index] : nullptr;
		FWorldBuilding Scratch;
		FWorldBuilding& Target = Building ? *Building : Scratch;
		Target.ClassId = Reader.Get<uint8>();
		Target.TypedRoofShape = Reader.Get<uint8>();
		Target.PitchDegrees = Reader.Get<uint8>();
		Target.Storeys = Reader.Get<uint8>();
		Target.AtticLevels = Reader.Get<uint8>();
		Target.TypeFlags = Reader.Get<uint8>();
		Target.TagBits = Reader.Get<uint8>();
		Target.PlinthMetres = Reader.Get<uint8>() * 0.05f;
		Target.StoreyHeight = Reader.Get<float>();
		Target.GroundHeight = Reader.Get<float>();
		Target.TypedEaveHeight = Reader.Get<float>();
		Target.RidgeYaw = Reader.Get<float>();
		Target.FrontYaw = Reader.Get<float>();
		Target.bTyped = Building != nullptr;
	}
}

void ReadPlants(FByteReader& Reader, TArray<FWorldPlant>& Plants)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		FWorldPlant& Plant = Plants.AddDefaulted_GetRef();
		Plant.Model = Reader.Get<uint16>();
		Reader.Skip(2);
		Plant.Position.X = Reader.Get<float>();
		Plant.Position.Y = Reader.Get<float>();
		Plant.Position.Z = Reader.Get<float>();
		Plant.YawDegrees = Reader.Get<float>();
		Plant.CrownDiameter = Reader.Get<float>();
		Plant.Height = Reader.Get<float>();
		Plant.TrunkDiameter = Reader.Get<float>();
	}
}

void ReadPois(FByteReader& Reader, TArray<FWorldPoi>& Pois)
{
	const uint32 Count = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < Count && !Reader.HasFailed(); ++Index)
	{
		FWorldPoi& Poi = Pois.AddDefaulted_GetRef();
		Poi.Kind = static_cast<EWorldPoiKind>(Reader.Get<uint8>());
		Poi.Flags = Reader.Get<uint8>();
		Poi.Variant = Reader.Get<uint16>();
		Poi.Variant2 = Reader.Get<uint16>();
		Reader.Skip(2);
		Poi.Position.X = Reader.Get<float>();
		Poi.Position.Y = Reader.Get<float>();
		Poi.Position.Z = Reader.Get<float>();
		Poi.YawDegrees = Reader.Get<float>();
		Poi.Param0 = Reader.Get<float>();
		Poi.Param1 = Reader.Get<float>();
		Poi.Link = Reader.Get<uint32>();
	}
}

/** Decompresses one section and hands it to the reader for its tag. Unknown tags are skipped. */
bool ReadSection(uint32 Tag, TConstArrayView<uint8> Raw, FWorldTileData& Out)
{
	FByteReader Reader(Raw);
	switch (Tag)
	{
	case MakeTag("NAME"): ReadNames(Reader, Out.Names); break;
	case MakeTag("GRID"): ReadGrid(Reader, Out.Grid); break;
	case MakeTag("SURF"): ReadSurfaces(Reader, Out.Surfaces); break;
	case MakeTag("MARK"): ReadMarkings(Reader, Out.Markings); break;
	case MakeTag("BLDG"): ReadBuildings(Reader, Out.Buildings); break;
	case MakeTag("BTYP"): ReadBuildingTypes(Reader, Out.Buildings); break;
	case MakeTag("VEGE"): ReadPlants(Reader, Out.Plants); break;
	case MakeTag("POIS"): ReadPois(Reader, Out.Pois); break;
	default: break;
	}
	return !Reader.HasFailed();
}
}

float FWorldTileGrid::TerrainAtVertex(int32 X, int32 Y) const
{
	X = FMath::Clamp(X, 0, NumX - 1);
	Y = FMath::Clamp(Y, 0, NumY - 1);
	const uint16 Value = TerrainCm[Y * NumX + X];
	if (Value == HoleValue)
	{
		return BaseZ;
	}
	return BaseZ + Value * 0.01f;
}

bool FWorldTileGrid::IsHole(int32 X, int32 Y) const
{
	X = FMath::Clamp(X, 0, NumX - 1);
	Y = FMath::Clamp(Y, 0, NumY - 1);
	return TerrainCm[Y * NumX + X] == HoleValue;
}

FColor FWorldTileGrid::CoverAtVertex(int32 X, int32 Y) const
{
	X = FMath::Clamp(X, 0, NumX - 1);
	Y = FMath::Clamp(Y, 0, NumY - 1);
	const int32 Index = (Y * NumX + X) * 3;
	return FColor(Cover[Index], Cover[Index + 1], Cover[Index + 2], 255);
}

float FWorldTileGrid::SampleBilinear(const TArray<uint16>& Heights, float LocalX, float LocalY) const
{
	const float GridX = FMath::Clamp(LocalX / CellSize, 0.f, NumX - 1.0001f);
	const float GridY = FMath::Clamp(LocalY / CellSize, 0.f, NumY - 1.0001f);
	const int32 X0 = FMath::FloorToInt(GridX);
	const int32 Y0 = FMath::FloorToInt(GridY);
	const float TX = GridX - X0;
	const float TY = GridY - Y0;
	const auto At = [&](int32 X, int32 Y) { return float(Heights[Y * NumX + X]); };
	const float Top = FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), TX);
	const float Bottom = FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), TX);
	return BaseZ + FMath::Lerp(Top, Bottom, TY) * 0.01f;
}

float FWorldTileGrid::TerrainAt(float LocalX, float LocalY) const
{
	return SampleBilinear(TerrainCm, LocalX, LocalY);
}

float FWorldTileGrid::RoadAt(float LocalX, float LocalY) const
{
	return SampleBilinear(RoadCm, LocalX, LocalY);
}

float FWorldTileData::SurfaceHeightAt(const FWorldSurface& Surface, float LocalX, float LocalY) const
{
	switch (Surface.HeightMode)
	{
	case EWorldSurfaceHeight::Road:
		return Grid.RoadAt(LocalX, LocalY) + Surface.Params[0];
	case EWorldSurfaceHeight::Terrain:
		return Grid.TerrainAt(LocalX, LocalY) + Surface.Params[0];
	case EWorldSurfaceHeight::Constant:
		return Surface.Params[0];
	case EWorldSurfaceHeight::Ramp:
	{
		const FVector2D Point(Origin.X + LocalX, Origin.Y + LocalY);
		const FVector2D Start(Surface.Params[2], Surface.Params[3]);
		const FVector2D End(Surface.Params[4], Surface.Params[5]);
		const FVector2D Axis = End - Start;
		const double Along = FMath::Clamp(FVector2D::DotProduct(Point - Start, Axis) / FMath::Max(Axis.SizeSquared(), 1e-6), 0.0, 1.0);
		return FMath::Lerp(Surface.Params[0], Surface.Params[1], float(Along));
	}
	}
	return 0.f;
}

bool FWorldTileData::Load(const FString& Path, FWorldTileData& Out, FString& Error)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		Error = FString::Printf(TEXT("cannot read %s"), *Path);
		return false;
	}
	FByteReader Reader(Bytes);
	const uint32 Magic = Reader.Get<uint32>();
	const uint32 Version = Reader.Get<uint32>();
	if (Magic != MakeTag("TGT1"))
	{
		Error = FString::Printf(TEXT("%s: not a world tile"), *Path);
		return false;
	}
	if (Version != TileFormatVersion)
	{
		Error = FString::Printf(TEXT("%s: world tile format version %u, this build reads version %u; rebuild the world with Tools/osmimport/build_world.py"),
			*Path, Version, TileFormatVersion);
		return false;
	}
	Out.Origin.X = Reader.Get<double>();
	Out.Origin.Y = Reader.Get<double>();
	Out.Size.X = Reader.Get<float>();
	Out.Size.Y = Reader.Get<float>();
	const uint32 SectionCount = Reader.Get<uint32>();
	for (uint32 Index = 0; Index < SectionCount && !Reader.HasFailed(); ++Index)
	{
		const uint32 Tag = Reader.Get<uint32>();
		const uint32 RawSize = Reader.Get<uint32>();
		const uint32 PackedSize = Reader.Get<uint32>();
		TArray<uint8> Packed;
		Reader.GetArray(Packed, PackedSize);
		TArray<uint8> Raw;
		Raw.SetNumUninitialized(RawSize);
		if (Reader.HasFailed() || !FCompression::UncompressMemory(NAME_Zlib, Raw.GetData(), RawSize, Packed.GetData(), PackedSize))
		{
			Error = FString::Printf(TEXT("%s: section %u is damaged"), *Path, Index);
			return false;
		}
		if (!ReadSection(Tag, Raw, Out))
		{
			Error = FString::Printf(TEXT("%s: section %u has the wrong layout"), *Path, Index);
			return false;
		}
	}
	if (Reader.HasFailed() || Out.Grid.NumX < 2 || Out.Grid.NumY < 2)
	{
		Error = FString::Printf(TEXT("%s: truncated or without a height grid"), *Path);
		return false;
	}
	return true;
}
