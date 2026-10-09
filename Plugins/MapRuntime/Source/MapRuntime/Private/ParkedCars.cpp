#include "ParkedCars.h"

#include "AITrafficCar.h"
#include "AITrafficSubsystem.h"
#include "WorldFurniture.h"
#include "WorldTileData.h"

namespace
{
static TAutoConsoleVariable<int32> CVarParkedCars(TEXT("tg.ParkedCars"), 1, TEXT("1 builds the parked cars along the streets, 0 leaves them out."));

constexpr float ParkedMetresToCm = 100.f;
/** The collision box is a little smaller than the body so a car brushing past doesn't catch on the mirrors. */
constexpr float ColliderShrink = 0.96f;

TSharedPtr<FTrafficVehicleModel> MakeParkedModel(const TCHAR* Folder, const TCHAR* Type, const TCHAR* Tag)
{
	TSharedPtr<FTrafficVehicleModel> Model = MakeShared<FTrafficVehicleModel>();
	Model->Folder = Folder;
	Model->Type = Type;
	Model->Tag = Tag;
	return Model;
}

/** Hash of a position to [0, 1): the same car always gets the same colour. */
float PositionHash(const FVector3f& Position)
{
	uint32 Hash = HashCombine(GetTypeHash(FMath::RoundToInt(Position.X * 10.f)), GetTypeHash(FMath::RoundToInt(Position.Y * 10.f)));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(Position.Z * 10.f)));
	Hash ^= Hash >> 15;
	Hash *= 0x2c1b3c6dU;
	Hash ^= Hash >> 12;
	return float(Hash & 0xFFFFFF) / float(0x1000000);
}
}

namespace ParkedCars
{
const TArray<TSharedPtr<FTrafficVehicleModel>>& GetModels()
{
	static const TArray<TSharedPtr<FTrafficVehicleModel>> Models = {
		MakeParkedModel(TEXT("vehicle07_Car"), TEXT("vehCar"), TEXT("vehicle07")),
		MakeParkedModel(TEXT("vehicle02_Car"), TEXT("vehCar"), TEXT("vehicle02")),
		MakeParkedModel(TEXT("vehicle03_Car"), TEXT("vehCar"), TEXT("vehicle03")),
		MakeParkedModel(TEXT("vehicle05_Car"), TEXT("vehCar"), TEXT("vehicle05")),
		MakeParkedModel(TEXT("vehicle06_Car"), TEXT("vehCar"), TEXT("vehicle06")),
		MakeParkedModel(TEXT("vehicle01_Van"), TEXT("vehVan"), TEXT("vehicle01")),
	};
	return Models;
}

bool IsDisabled()
{
	return CVarParkedCars.GetValueOnAnyThread() == 0 || FParse::Param(FCommandLine::Get(), TEXT("NoParkedCars"));
}

void AddParkedCar(const FWorldPoi& Poi, const FVector2D& TileOriginM, FWorldFurnitureInstances& Out)
{
	if (IsDisabled() || Poi.Variant >= GetModels().Num())
	{
		return;
	}
	FParkedCarPlacement& Car = Out.ParkedCars.AddDefaulted_GetRef();
	Car.ModelIndex = Poi.Variant;
	Car.PaintIndex = UAITrafficSubsystem::PickPaintPaletteIndex(PositionHash(Poi.Position + FVector3f(float(TileOriginM.X), float(TileOriginM.Y), 0.f)));
	Car.Pose = FTransform(FRotator(Poi.Param1, Poi.YawDegrees, Poi.Param0), FVector(Poi.Position) * ParkedMetresToCm);
}

FTransform MeshTransform(const FTrafficVehicleModel& Model, const FTransform& Pose)
{
	const FVector BodyCentre = Model.BodyBoundsCm.GetCenter();
	const FVector Shift(BodyCentre.X, BodyCentre.Y, Model.GroundZCm());
	return FTransform(-Shift) * Pose;
}

FTransform ColliderTransform(const FTrafficVehicleModel& Model, const FTransform& Pose)
{
	const FVector BodyCentre = Model.BodyBoundsCm.GetCenter();
	const FVector Shift(BodyCentre.X, BodyCentre.Y, Model.GroundZCm());
	const FVector Size = Model.BodyBoundsCm.GetExtent() * 2.0 * ColliderShrink / 100.0;
	return FTransform(FQuat::Identity, BodyCentre - Shift, Size) * Pose;
}
}
