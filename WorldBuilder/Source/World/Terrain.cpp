#include "Terrain.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Geometry.h"

namespace WorldBuilder
{
namespace
{
/** Terrain sits this far below the road surface under roads. */
constexpr double UnderRoad = 0.10;
/** At the road and pavement edge, terrain meets the surface just below it. */
constexpr double EdgeDrop = 0.03;
/** Metres over which terrain returns to the natural ground. */
constexpr double BlendDistance = 5.0;
/** Smoothing of the road surface height, metres. */
constexpr double RoadSmoothing = 4.0;
/** Smoothing of the natural terrain, cells (takes the edge off the survey's noise). */
constexpr double NaturalSmoothing = 0.7;

FGridFrame FrameOf(const FHeightGrid& Grid)
{
	return {Grid.Columns, Grid.Rows, Grid.X0, Grid.Y0, Grid.CellSize};
}

/** A grid with the same placement as another and the given heights. */
FHeightGrid WithHeights(const FHeightGrid& Placement, const std::vector<double>& Heights)
{
	FHeightGrid Result = Placement;
	for (size_t Index = 0; Index < Heights.size(); ++Index)
	{
		Result.Heights[Index] = static_cast<float>(Heights[Index]);
	}
	return Result;
}

/** Lowers the natural ground inside a water body to a bed 0.3 m below the level at the bank, down to 2 m. */
void CarveWaterBed(const FHeightGrid& Terrain, const FWaterBody& Body, std::vector<double>& Natural)
{
	const FMask Inside = RasterizePolygons(FrameOf(Terrain), Body.Polygon, true);
	if (std::find(Inside.begin(), Inside.end(), 1) == Inside.end())
	{
		return;
	}
	FMask Outside(Inside.size());
	for (size_t Index = 0; Index < Inside.size(); ++Index)
	{
		Outside[Index] = Inside[Index] ? 0 : 1;
	}
	// Distance from the bank: from every inside cell to the nearest outside one.
	const FDistanceField FromBank = DistanceToFeatures(Outside, Terrain.Columns, Terrain.Rows, Terrain.CellSize);
	for (size_t Index = 0; Index < Inside.size(); ++Index)
	{
		if (!Inside[Index])
		{
			continue;
		}
		// A window without any outside cell is all water: the bed is as deep as it gets.
		const double Distance = std::isfinite(FromBank.Distance[Index]) ? FromBank.Distance[Index] : 1e9;
		const double Depth = std::clamp(0.3 + Distance * 0.4, 0.3, 2.0);
		Natural[Index] = std::min(Natural[Index], Body.Level - Depth);
	}
}
}

FHeightGrid RoadHeightField(const FHeightGrid& Terrain, const FPolygons& RoadGround)
{
	const FMask Mask = RasterizePolygons(FrameOf(Terrain), RoadGround, false);
	if (std::find(Mask.begin(), Mask.end(), 1) == Mask.end())
	{
		return Terrain;
	}
	const size_t CellCount = Mask.size();
	std::vector<double> Weight(Mask.begin(), Mask.end());
	std::vector<double> Smooth(CellCount);
	for (size_t Index = 0; Index < CellCount; ++Index)
	{
		Smooth[Index] = Mask[Index] ? Terrain.Heights[Index] : 0.0;
	}
	const double Sigma = RoadSmoothing / Terrain.CellSize;
	GaussianFilter(Weight, Terrain.Columns, Terrain.Rows, Sigma);
	GaussianFilter(Smooth, Terrain.Columns, Terrain.Rows, Sigma);

	// Valid where the road is, or near enough for the normalised smoothing to be meaningful.
	FMask Valid(CellCount, 0);
	bool bAnyValid = false;
	for (size_t Index = 0; Index < CellCount; ++Index)
	{
		const bool bWeighted = Weight[Index] > 1e-3;
		Smooth[Index] = bWeighted ? Smooth[Index] / Weight[Index] : std::numeric_limits<double>::quiet_NaN();
		Valid[Index] = (Mask[Index] || Weight[Index] > 0.05) && bWeighted ? 1 : 0;
		bAnyValid = bAnyValid || Valid[Index];
	}
	if (!bAnyValid)
	{
		return Terrain;
	}
	const FDistanceField Nearest = DistanceToFeatures(Valid, Terrain.Columns, Terrain.Rows, Terrain.CellSize);
	std::vector<double> Extended(CellCount);
	for (size_t Index = 0; Index < CellCount; ++Index)
	{
		Extended[Index] = Smooth[Nearest.Nearest[Index]];
	}
	return WithHeights(Terrain, Extended);
}

FHeightGrid ConformTerrain(const FHeightGrid& Terrain, const FHeightGrid& RoadHeight, const FPolygons& RoadGround,
						   const FPolygons& Pavement, const std::vector<FWaterBody>& Water)
{
	const size_t CellCount = Terrain.Heights.size();
	std::vector<double> Natural(Terrain.Heights.begin(), Terrain.Heights.end());
	GaussianFilter(Natural, Terrain.Columns, Terrain.Rows, NaturalSmoothing);
	for (const FWaterBody& Body : Water)
	{
		CarveWaterBed(Terrain, Body, Natural);
	}

	const FMask RoadMask = RasterizePolygons(FrameOf(Terrain), RoadGround, true);
	FMask PavementMask = RasterizePolygons(FrameOf(Terrain), Pavement, true);
	FMask Surfaced(CellCount, 0);
	bool bAnySurfaced = false;
	for (size_t Index = 0; Index < CellCount; ++Index)
	{
		PavementMask[Index] = PavementMask[Index] && !RoadMask[Index];
		Surfaced[Index] = RoadMask[Index] || PavementMask[Index];
		bAnySurfaced = bAnySurfaced || Surfaced[Index];
	}
	if (!bAnySurfaced)
	{
		return WithHeights(Terrain, Natural);
	}

	// Outside: blend from the nearest surfaced edge height to the natural ground.
	const FDistanceField Nearest = DistanceToFeatures(Surfaced, Terrain.Columns, Terrain.Rows, Terrain.CellSize);
	std::vector<double> Result(CellCount);
	for (size_t Index = 0; Index < CellCount; ++Index)
	{
		const double Road = RoadHeight.Heights[Index];
		if (RoadMask[Index])
		{
			Result[Index] = Road - UnderRoad;
			continue;
		}
		if (PavementMask[Index])
		{
			Result[Index] = Road + KerbHeight - UnderRoad;
			continue;
		}
		const int64_t Edge = Nearest.Nearest[Index];
		const double EdgeRoad = RoadHeight.Heights[Edge];
		const double EdgeHeight = RoadMask[Edge] ? EdgeRoad - EdgeDrop : EdgeRoad + KerbHeight - EdgeDrop;
		double Blend = std::clamp(Nearest.Distance[Index] / BlendDistance, 0.0, 1.0);
		Blend = Blend * Blend * (3.0 - 2.0 * Blend);
		Result[Index] = EdgeHeight * (1.0 - Blend) + Natural[Index] * Blend;
	}
	return WithHeights(Terrain, Result);
}
}
