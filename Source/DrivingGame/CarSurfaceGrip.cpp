#include "CarSurfaceGrip.h"

namespace
{
/** Grip on dry ground relative to dry asphalt. */
float CarGripDryScale(ECarGripSurface Surface)
{
	switch (Surface)
	{
	case ECarGripSurface::Cobble: return 0.85f;
	case ECarGripSurface::Pavers: return 0.9f;
	case ECarGripSurface::Rough: return 0.65f;
	default: return 1.f;
	}
}

/** Grip on soaked ground relative to dry asphalt; polished stone and grass lose more than asphalt. */
float CarGripWetScale(ECarGripSurface Surface)
{
	switch (Surface)
	{
	case ECarGripSurface::Cobble: return 0.6f;
	case ECarGripSurface::Pavers: return 0.65f;
	case ECarGripSurface::Rough: return 0.45f;
	default: return 0.8f;
	}
}

constexpr float CarGripIceScale = 0.12f;
constexpr float CarGripPackedSnowScale = 0.34f;
constexpr float CarGripSlushScale = 0.45f;
/** Depth of loose snow at full cover: trafficked roads are packed and cleared, open ground is not. */
constexpr float CarSnowDepthRoadM = 0.03f;
constexpr float CarSnowDepthOffRoadM = 0.12f;
/** Added rolling resistance coefficient per metre of loose snow. */
constexpr float CarSnowRollingPerMetre = 0.35f;
}

float CarSurfaceGrip::Ramp(float Edge0, float Edge1, float Value)
{
	const float Alpha = FMath::Clamp((Value - Edge0) / (Edge1 - Edge0), 0.f, 1.f);
	return Alpha * Alpha * (3.f - 2.f * Alpha);
}

ECarGripSurface CarSurfaceGrip::Classify(const FName& GroundMaterialName)
{
	if (GroundMaterialName.IsNone())
	{
		return ECarGripSurface::Asphalt; // flat test maps and anything without generated ground
	}
	const FString Text = GroundMaterialName.ToString();
	if (Text.Contains(TEXT("Cobble")))
	{
		return ECarGripSurface::Cobble;
	}
	if (Text.Contains(TEXT("Pavers")) || Text.Contains(TEXT("Pavement")) || Text.Contains(TEXT("Path_Paved")) || Text.Contains(TEXT("Kerb")))
	{
		return ECarGripSurface::Pavers;
	}
	if (Text.StartsWith(TEXT("Road_")) || Text.Contains(TEXT("Bridge")))
	{
		return ECarGripSurface::Asphalt;
	}
	return ECarGripSurface::Rough;
}

FCarWheelSurface CarSurfaceGrip::Evaluate(ECarGripSurface Surface, const FCarSurfaceConditions& Conditions, float Variation)
{
	const float WetBlend = Ramp(0.05f, 0.6f, Conditions.Wetness);
	// Black ice: a wet road below freezing. A dry frozen road keeps its grip.
	const float Freezing = Ramp(0.5f, -1.5f, Conditions.TemperatureCelsius);
	const float IceBlend = Freezing * Ramp(0.1f, 0.5f, Conditions.Wetness);
	const float SnowBlend = Ramp(0.15f, 0.7f, Conditions.SnowCover);
	const bool bPaved = Surface != ECarGripSurface::Rough;

	float GripScale = FMath::Lerp(CarGripDryScale(Surface), CarGripWetScale(Surface), WetBlend);
	GripScale = FMath::Lerp(GripScale, CarGripIceScale, IceBlend);
	const float SnowScale = FMath::Lerp(CarGripSlushScale, CarGripPackedSnowScale, Freezing);
	GripScale = FMath::Lerp(GripScale, SnowScale * (1.f + 0.12f * Variation), SnowBlend);

	// Snow and ice: the peak moves to more slip and the curve falls off less after it.
	const float LooseBlend = FMath::Max(SnowBlend, IceBlend);
	FCarWheelSurface Out;
	Out.GripScale = GripScale;
	Out.PeakSlipScale = 1.f + 0.15f * WetBlend + 0.5f * LooseBlend;
	Out.ShapeCScale = 1.f - 0.15f * LooseBlend;

	const float DepthM = Conditions.SnowCover * (bPaved ? CarSnowDepthRoadM : CarSnowDepthOffRoadM) * (1.f + 0.4f * Variation);
	Out.ExtraRollingResistance = SnowBlend * CarSnowRollingPerMetre * FMath::Max(0.f, DepthM);

	// Standing water on a mild day; the physics thread turns this into grip loss above about 70 km/h.
	Out.Aquaplaning = Ramp(0.85f, 1.f, Conditions.Wetness) * Ramp(0.5f, 2.f, Conditions.TemperatureCelsius);
	return Out;
}

bool CarSurfaceGrip::TestPreset(const FString& Name, FCarSurfaceConditions& OutConditions)
{
	OutConditions = FCarSurfaceConditions();
	if (Name.Equals(TEXT("dry"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (Name.Equals(TEXT("wet"), ESearchCase::IgnoreCase))
	{
		OutConditions.Wetness = 0.7f;
		return true;
	}
	if (Name.Equals(TEXT("flood"), ESearchCase::IgnoreCase))
	{
		OutConditions.Wetness = 1.f;
		return true;
	}
	if (Name.Equals(TEXT("snow"), ESearchCase::IgnoreCase))
	{
		OutConditions.Wetness = 0.3f;
		OutConditions.SnowCover = 1.f;
		OutConditions.TemperatureCelsius = -4.f;
		return true;
	}
	if (Name.Equals(TEXT("ice"), ESearchCase::IgnoreCase))
	{
		OutConditions.Wetness = 0.8f;
		OutConditions.TemperatureCelsius = -2.f;
		return true;
	}
	return false;
}
