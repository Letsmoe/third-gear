#include "MapPalette.h"

namespace
{
FColor Hex(uint32 Rgb)
{
	return FColor((Rgb >> 16) & 0xFF, (Rgb >> 8) & 0xFF, Rgb & 0xFF, 0xFF);
}

FMapPalette MakeLight()
{
	FMapPalette Palette;
	Palette.bDark = false;
	Palette.Land = Hex(0xF8F7F7);
	Palette.Meadow = Hex(0xE3F1DC);
	Palette.Field = Hex(0xF3F1E7);
	Palette.Forest = Hex(0xD3E9CC);
	Palette.Water = Hex(0x90DAEE);
	Palette.Footpath = Hex(0x8FD4B0);
	Palette.Residential = Hex(0xE8E9ED);
	Palette.Commercial = Hex(0xFDF9EF);
	Palette.Industrial = Hex(0xDBE0E8);
	for (FColor& RoadColor : Palette.Road)
	{
		RoadColor = Hex(0xC2D0DE);
	}
	Palette.RoadEdge = Hex(0xB1C1D2);
	Palette.OneWayArrow = Hex(0x8E9CAD);
	Palette.Route = Hex(0x4285F4);
	Palette.RouteEdge = Hex(0x1A5BC8);
	Palette.CarArrow = Hex(0x4285F4);
	Palette.CarArrowBorder = Hex(0xFFFFFF);
	Palette.Waypoint = Hex(0xEA4335);
	Palette.Label = Hex(0x5F6B7A);
	Palette.LabelHalo = Hex(0xFFFFFF);
	Palette.LabelBackground = Hex(0xFFFFFF);
	return Palette;
}

FMapPalette MakeDark()
{
	FMapPalette Palette;
	Palette.bDark = true;
	Palette.Land = Hex(0x2F3F67);
	Palette.Meadow = Hex(0x0F4352);
	Palette.Field = Hex(0x364670);
	Palette.Forest = Hex(0x0B3A48);
	Palette.Water = Hex(0x073F4B);
	Palette.Footpath = Hex(0x3C6E9C);
	Palette.Residential = Hex(0x485276);
	Palette.Commercial = Hex(0x56608A);
	Palette.Industrial = Hex(0x3E4A70);
	const uint32 RoadColors[5] = {0x6387C6, 0x6A8CC6, 0x7190C7, 0x7894C7, 0x7F97C7};
	for (int32 Tier = 0; Tier < 5; ++Tier)
	{
		Palette.Road[Tier] = Hex(RoadColors[Tier]);
	}
	Palette.RoadEdge = Hex(0x587AB4);
	Palette.OneWayArrow = Hex(0x9DB3DD);
	Palette.Route = Hex(0x3FD8E8);
	Palette.RouteEdge = Hex(0x1B9DB0);
	Palette.CarArrow = Hex(0x8AB4F8);
	Palette.CarArrowBorder = Hex(0xFFFFFF);
	Palette.Waypoint = Hex(0xF28B82);
	Palette.Label = Hex(0xFFFFFF);
	Palette.LabelHalo = Hex(0x1A2440);
	Palette.LabelBackground = Hex(0x1E2A4A);
	return Palette;
}
}

const FMapPalette& FMapPalette::Light()
{
	static const FMapPalette Palette = MakeLight();
	return Palette;
}

const FMapPalette& FMapPalette::Dark()
{
	static const FMapPalette Palette = MakeDark();
	return Palette;
}
