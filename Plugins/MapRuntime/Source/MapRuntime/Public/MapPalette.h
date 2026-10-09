#pragma once

#include "CoreMinimal.h"

/** Which of the three building fills a footprint gets on the 2D maps (and the wall colours of the 3D map). */
enum class EMapBuildingStyle : uint8
{
	Residential = 0,
	/** Shops, offices, retail centres and public buildings: a warm tone. */
	Commercial = 1,
	/** Industrial halls: a darker grey. */
	Industrial = 2,
};

/**
 * The colours of the maps (minimap, full map and the 3D view), as sRGB. One light set for the day, modelled on Google
 * Maps, and one dark set for the night, so everything that draws a map reads its colours from here.
 */
struct MAPRUNTIME_API FMapPalette
{
	bool bDark = false;

	// Ground
	FColor Land;
	FColor Meadow;
	FColor Field;
	FColor Forest;
	FColor Water;
	/** Outlines of pavements and footpaths, thin lines. */
	FColor Footpath;

	// Buildings
	FColor Residential;
	FColor Commercial;
	FColor Industrial;

	// Roads by class 0 (service) to 4 (primary and trunk), and the thin edge drawn under every road.
	FColor Road[5];
	FColor RoadEdge;
	FColor OneWayArrow;

	// Navigation
	FColor Route;
	FColor RouteEdge;
	FColor CarArrow;
	FColor CarArrowBorder;
	FColor Waypoint;

	// Text
	FColor Label;
	FColor LabelHalo;
	FColor LabelBackground;

	/** The sRGB colour as the linear colour Slate and materials take. */
	static FLinearColor Linear(const FColor& SrgbColor) { return FLinearColor::FromSRGBColor(SrgbColor); }

	/** Fill of a building style. */
	FColor BuildingFill(EMapBuildingStyle Style) const
	{
		switch (Style)
		{
		case EMapBuildingStyle::Commercial: return Commercial;
		case EMapBuildingStyle::Industrial: return Industrial;
		default: return Residential;
		}
	}

	/** The day palette, after Google Maps' light style. */
	static const FMapPalette& Light();

	/** The night palette, after Google Maps' dark navigation style. */
	static const FMapPalette& Dark();
};
