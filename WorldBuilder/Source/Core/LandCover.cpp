#include "LandCover.h"

#include <algorithm>
#include <cmath>
#include <cstring>


namespace WorldBuilder
{
namespace
{
/** Early summer: most fields are green (grain, maize, grassland), the rest bare soil. */
constexpr double GreenCropShare = 0.6;

struct FCoverRule
{
	const char* Key;
	const char* Value;
	ECoverClass Class;
};

/** First match wins, so more specific entries come first (landcover.py's RULES). */
constexpr FCoverRule Rules[] = {
	{"natural", "wood", ECoverClass::Forest},
	{"landuse", "forest", ECoverClass::Forest},
	{"natural", "scrub", ECoverClass::Forest},
	{"landuse", "farmland", ECoverClass::Field},
	{"landuse", "greenhouse_horticulture", ECoverClass::Field},
	{"landuse", "plant_nursery", ECoverClass::Field},
	{"landuse", "construction", ECoverClass::Field},
	{"landuse", "brownfield", ECoverClass::Field},
	{"landuse", "meadow", ECoverClass::Meadow},
	{"landuse", "grass", ECoverClass::Meadow},
	{"natural", "grassland", ECoverClass::Meadow},
	{"natural", "wetland", ECoverClass::Meadow},
	{"natural", "heath", ECoverClass::Meadow},
	{"landuse", "orchard", ECoverClass::Meadow},
	{"landuse", "farmyard", ECoverClass::Meadow},
	{"landuse", "railway", ECoverClass::Meadow},
};

/** landcover.py's farmland hash: (id * 2654435761 % 1000) / 1000, with Python's unbounded integers. */
bool IsGreenCrop(int64_t Id)
{
	const unsigned __int128 Product = static_cast<unsigned __int128>(static_cast<uint64_t>(Id)) * 2654435761u;
	return static_cast<double>(static_cast<uint64_t>(Product % 1000)) / 1000.0 < GreenCropShare;
}
}

std::optional<ECoverClass> ClassifyCover(const FOsmArea& Area)
{
	for (const FCoverRule& Rule : Rules)
	{
		const std::string* Value = FindTag(Area.Tags, Rule.Key);
		if (Value == nullptr || *Value != Rule.Value)
		{
			continue;
		}
		const bool bFarmland = std::strcmp(Rule.Key, "landuse") == 0 && std::strcmp(Rule.Value, "farmland") == 0;
		if (bFarmland && IsGreenCrop(Area.Id))
		{
			return ECoverClass::Meadow;
		}
		return Rule.Class;
	}
	return std::nullopt;
}

FLandCover::FLandCover(const std::vector<FOsmArea>& Areas)
{
	for (const FOsmArea& Area : Areas)
	{
		const std::optional<ECoverClass> Class = ClassifyCover(Area);
		if (!Class)
		{
			continue;
		}
		std::vector<FPolygonWithHoles> Polygons;
		for (const FOsmPolygon& Polygon : Area.Polygons)
		{
			Polygons.push_back({Polygon.Rings});
		}
		// Even-odd union orients outlines and holes consistently, which the non-zero union per window relies on.
		const int ClassIndex = static_cast<int>(*Class);
		Classes[ClassIndex].Add(ClassIndex, Clipper2Lib::Union(ToPaths(Polygons), Clipper2Lib::FillRule::EvenOdd, 4));
	}
}

std::vector<float> FLandCover::Weights(double X0, double Y0, int Columns, int Rows, double CellSize,
									   double BlurMetres) const
{
	// Rasterise with a margin of the blur's reach, so tiles agree along their shared edges.
	const double Sigma = BlurMetres / CellSize;
	const int Margin = static_cast<int>(4.0 * Sigma + 0.5) + 1;
	FGridFrame Frame;
	Frame.Columns = Columns + 2 * Margin;
	Frame.Rows = Rows + 2 * Margin;
	Frame.CellSize = CellSize;
	// Cells are centred on the vertices.
	Frame.X0 = X0 - CellSize / 2 - Margin * CellSize;
	Frame.Y0 = Y0 - CellSize / 2 - Margin * CellSize;
	const FBox Window{Frame.X0, Frame.Y0, Frame.X0 + Frame.Columns * CellSize, Frame.Y0 + Frame.Rows * CellSize};

	std::vector<float> Result(static_cast<size_t>(Columns) * Rows * 3, 0.0f);
	for (int ClassIndex = 0; ClassIndex < 3; ++ClassIndex)
	{
		const FPolygons Near = Classes[ClassIndex].UnionInWindow(Window);
		if (Near.empty())
		{
			continue;
		}
		const FMask Mask = RasterizePolygons(Frame, Near, false);
		std::vector<double> Values(Mask.begin(), Mask.end());
		if (Sigma > 0.3)
		{
			GaussianFilter(Values, Frame.Columns, Frame.Rows, Sigma);
		}
		for (int Row = 0; Row < Rows; ++Row)
		{
			for (int Column = 0; Column < Columns; ++Column)
			{
				const double Weight = Values[static_cast<size_t>(Row + Margin) * Frame.Columns + Column + Margin];
				Result[(static_cast<size_t>(Row) * Columns + Column) * 3 + ClassIndex] =
					static_cast<float>(std::clamp(Weight, 0.0, 1.0));
			}
		}
	}
	return Result;
}
}
