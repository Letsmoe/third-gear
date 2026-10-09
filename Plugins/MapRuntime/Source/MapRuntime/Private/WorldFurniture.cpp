#include "WorldFurniture.h"

#include "WorldTileData.h"

namespace
{
constexpr float FurnitureMetresToCm = 100.f;
/** Lowest edge of the lowest sign above the foot of the pole, m (clear height above a pavement). */
constexpr float SignBottomHeightM = 2.2f;
constexpr float SignGapM = 0.01f;
constexpr float SignStandoffM = 0.064f;
constexpr float SignTopMarginM = 0.07f;
constexpr float ClampInsetM = 0.07f;

constexpr float LampArmReachM = 1.82f;
constexpr float LampLightHeightM = 6.82f;
constexpr float SignalLensX = -0.40f;
constexpr float SignalCenterHeightM = 3.0f;
constexpr float SignalLensPitchM = 0.305f;

/** Aspect ratio (width / height) of the sign graphics, from the PNG sizes. Anything else is square. */
const TMap<FString, float>& SignAspects()
{
	static const TMap<FString, float> Aspects = {
		{TEXT("Zeichen_1010-51"), 1.8184f}, {TEXT("Zeichen_101"), 1.1396f}, {TEXT("Zeichen_102"), 1.1396f},
		{TEXT("Zeichen_103-10"), 1.1396f}, {TEXT("Zeichen_103-20"), 1.1396f}, {TEXT("Zeichen_108-10"), 1.1396f},
		{TEXT("Zeichen_108"), 1.1396f}, {TEXT("Zeichen_110-10"), 1.1396f}, {TEXT("Zeichen_120"), 1.1396f},
		{TEXT("Zeichen_123"), 1.1396f}, {TEXT("Zeichen_131"), 1.1396f}, {TEXT("Zeichen_133"), 1.1396f},
		{TEXT("Zeichen_136"), 0.9355f}, {TEXT("Zeichen_138"), 1.1396f}, {TEXT("Zeichen_205"), 1.1396f},
		{TEXT("Zeichen_220"), 2.6963f}, {TEXT("Zeichen_237"), 0.9355f}, {TEXT("Zeichen_301"), 1.1396f},
		{TEXT("Zeichen_310"), 1.5f}, {TEXT("Zeichen_311"), 1.5f}, {TEXT("Zeichen_325.1"), 1.498f},
		{TEXT("Zeichen_325.2"), 1.498f}, {TEXT("Zusatzzeichen_1000-10"), 1.8184f}, {TEXT("Zusatzzeichen_1000-20"), 1.8184f},
		{TEXT("Zusatzzeichen_1001-30"), 1.8184f}, {TEXT("Zusatzzeichen_1020-30"), 1.8203f},
		{TEXT("Zusatzzeichen_1022-10"), 1.334f}, {TEXT("Zusatzzeichen_1040-30"), 1.8164f},
		{TEXT("Zusatzzeichen_1052-30"), 1.334f},
	};
	return Aspects;
}

/** Plate width in metres by sign type: triangles and stop signs large, round and square signs medium. */
float SignWidthMetres(const FString& Name)
{
	if (Name.StartsWith(TEXT("Zusatzzeichen")) || Name == TEXT("Zeichen_1010-51"))
	{
		return 0.6f;
	}
	if (Name == TEXT("Zeichen_310") || Name == TEXT("Zeichen_311"))
	{
		return 1.1f;
	}
	if (Name == TEXT("Zeichen_220"))
	{
		return 0.84f;
	}
	if (Name == TEXT("Zeichen_205") || Name == TEXT("Zeichen_206") || Name.StartsWith(TEXT("Zeichen_1")))
	{
		return 0.9f;
	}
	return 0.6f;
}

float SignAspect(const FString& Name)
{
	const float* Found = SignAspects().Find(Name);
	return Found ? *Found : 1.f;
}

FVector TileLocationCm(const FWorldPoi& Poi)
{
	return FVector(Poi.Position.X * FurnitureMetresToCm, Poi.Position.Y * FurnitureMetresToCm, Poi.Position.Z * FurnitureMetresToCm);
}

void AddLamp(const FWorldPoi& Poi, FWorldFurnitureInstances& Out)
{
	const FRotator Yaw(0.f, Poi.YawDegrees, 0.f);
	const FVector Base = TileLocationCm(Poi);
	Out.Lamps.Emplace(Yaw, Base, FVector::OneVector);
	FFurnitureLight Light;
	Light.LocationCm = Base + Yaw.RotateVector(FVector(LampArmReachM, 0.f, LampLightHeightM) * FurnitureMetresToCm);
	Light.Direction = FVector::DownVector;
	Out.Lights.Add(Light);
}

void AddSignalHead(const FWorldPoi& Poi, FWorldFurnitureInstances& Out)
{
	const FRotator Yaw(0.f, Poi.YawDegrees, 0.f);
	const FVector Base = TileLocationCm(Poi);
	Out.SignalPoles.Emplace(Yaw, Base, FVector::OneVector);
	Out.SignalHeads.Emplace(Yaw, Base, FVector::OneVector);
	Out.HeadApproaches.Add(int32(Poi.Link));
	FFurnitureLight Light;
	Light.LocationCm = Base + Yaw.RotateVector(FVector(SignalLensX * FurnitureMetresToCm, 0.f, SignalCenterHeightM * FurnitureMetresToCm));
	Light.Direction = Yaw.RotateVector(FVector(-1.f, 0.f, 0.f));
	Light.ApproachId = int32(Poi.Link);
	Light.LensOffsetsCm[0] = SignalLensPitchM * FurnitureMetresToCm;
	Light.LensOffsetsCm[1] = 0.f;
	Light.LensOffsetsCm[2] = -SignalLensPitchM * FurnitureMetresToCm;
	Out.Lights.Add(Light);
}

/** Picks the shortest pole mesh that carries the stack of plates, in cm. */
int32 PoleHeightFor(float StackTopM)
{
	const float NeededCm = (StackTopM + SignTopMarginM) * FurnitureMetresToCm;
	for (const int32 Height : GetSignPoleHeightsCm())
	{
		if (Height >= NeededCm)
		{
			return Height;
		}
	}
	return GetSignPoleHeightsCm().Last();
}

void AddSign(const FWorldTileData& Tile, const FWorldPoi& Poi, FWorldFurnitureInstances& Out)
{
	if (!Tile.Names.IsValidIndex(Poi.Variant))
	{
		return;
	}
	TArray<FString> Graphics;
	if (Poi.Variant2 != FWorldPoi::NoVariant && Tile.Names.IsValidIndex(Poi.Variant2))
	{
		Graphics.Add(Tile.Names[Poi.Variant2]); // additional sign sits below the main one
	}
	Graphics.Add(Tile.Names[Poi.Variant]);
	const FRotator Yaw(0.f, Poi.YawDegrees, 0.f);
	const FVector Base = TileLocationCm(Poi);
	float BottomM = SignBottomHeightM;
	for (const FString& Graphic : Graphics)
	{
		const float Width = SignWidthMetres(Graphic);
		const float Height = Width / SignAspect(Graphic);
		const FVector Center = Base + Yaw.RotateVector(FVector(-SignStandoffM, 0.f, BottomM + Height * 0.5f) * FurnitureMetresToCm);
		Out.SignPlates.FindOrAdd(Graphic).Emplace(Yaw, Center, FVector(1.f, Width, Height));
		for (const float Sign : {-1.f, 1.f})
		{
			const float ClampHeight = BottomM + Height * 0.5f + Sign * (Height * 0.5f - ClampInsetM);
			Out.SignClamps.Emplace(Yaw, Base + Yaw.RotateVector(FVector(0.f, 0.f, ClampHeight * FurnitureMetresToCm)), FVector::OneVector);
		}
		BottomM += Height + SignGapM;
	}
	Out.SignPoles.FindOrAdd(PoleHeightFor(BottomM - SignGapM)).Emplace(Yaw, Base, FVector::OneVector);
}
}

namespace FurnitureAssets
{
const TCHAR* const Lamp = TEXT("SM_StreetLamp");
const TCHAR* const SignalPole = TEXT("SM_SignalPole");
const TCHAR* const SignalHead = TEXT("SM_SignalHead");
const TCHAR* const SignPlate = TEXT("SM_SignPlate");
const TCHAR* const SignClamp = TEXT("SM_SignClamp");
}

const TArray<int32>& GetSignPoleHeightsCm()
{
	static const TArray<int32> Heights = {300, 350, 400};
	return Heights;
}

FString SignTexturePath(const FString& GraphicName)
{
	const FString AssetName = TEXT("T_") + GraphicName.Replace(TEXT("."), TEXT("_"));
	return FString::Printf(TEXT("/Game/World/Furniture/Signs/%s.%s"), *AssetName, *AssetName);
}

void BuildWorldFurniture(const FWorldTileData& Tile, FWorldFurnitureInstances& Out)
{
	for (const FWorldPoi& Poi : Tile.Pois)
	{
		switch (Poi.Kind)
		{
		case EWorldPoiKind::Lamp: AddLamp(Poi, Out); break;
		case EWorldPoiKind::SignalHead: AddSignalHead(Poi, Out); break;
		case EWorldPoiKind::Sign: AddSign(Tile, Poi, Out); break;
		}
	}
}
