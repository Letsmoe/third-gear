#include "Raster.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace WorldBuilder
{
namespace
{
/** The x coordinates where the polygons' edges cross a horizontal line, sorted, for even-odd filling. */
std::vector<double> RowCrossings(const FPolygons& Polygons, double Y)
{
	std::vector<double> Crossings;
	for (const Clipper2Lib::PathD& Ring : Polygons)
	{
		const size_t Count = Ring.size();
		for (size_t Index = 0; Index < Count; ++Index)
		{
			const Clipper2Lib::PointD& Start = Ring[Index];
			const Clipper2Lib::PointD& End = Ring[(Index + 1) % Count];
			// Half-open in y, so a vertex exactly on the line is counted once.
			if ((Start.y <= Y) == (End.y <= Y))
			{
				continue;
			}
			Crossings.push_back(Start.x + (Y - Start.y) / (End.y - Start.y) * (End.x - Start.x));
		}
	}
	std::sort(Crossings.begin(), Crossings.end());
	return Crossings;
}

/** Marks the cells whose centres lie inside the polygons. */
void FillCellCentres(const FGridFrame& Frame, const FPolygons& Polygons, FMask& Mask)
{
	for (int Row = 0; Row < Frame.Rows; ++Row)
	{
		const double Y = Frame.Y0 + (Row + 0.5) * Frame.CellSize;
		const std::vector<double> Crossings = RowCrossings(Polygons, Y);
		for (size_t Pair = 0; Pair + 1 < Crossings.size(); Pair += 2)
		{
			// Columns whose centre x lies in [enter, leave).
			const double Enter = (Crossings[Pair] - Frame.X0) / Frame.CellSize - 0.5;
			const double Leave = (Crossings[Pair + 1] - Frame.X0) / Frame.CellSize - 0.5;
			const int First = std::max(0, static_cast<int>(std::ceil(Enter)));
			const int Last = std::min(Frame.Columns - 1, static_cast<int>(std::ceil(Leave)) - 1);
			for (int Column = First; Column <= Last; ++Column)
			{
				Mask[static_cast<size_t>(Row) * Frame.Columns + Column] = 1;
			}
		}
	}
}

/** Marks every cell a segment passes through (a supercover walk in cell coordinates). */
void MarkSegmentCells(const FGridFrame& Frame, Clipper2Lib::PointD Start, Clipper2Lib::PointD End, FMask& Mask)
{
	const double StartX = (Start.x - Frame.X0) / Frame.CellSize;
	const double StartY = (Start.y - Frame.Y0) / Frame.CellSize;
	const double EndX = (End.x - Frame.X0) / Frame.CellSize;
	const double EndY = (End.y - Frame.Y0) / Frame.CellSize;
	const double Length = std::hypot(EndX - StartX, EndY - StartY);
	// Steps of a quarter cell can't skip a cell the segment crosses by more than a sliver.
	const int Steps = std::max(1, static_cast<int>(std::ceil(Length * 4.0)));
	for (int Step = 0; Step <= Steps; ++Step)
	{
		const double Along = static_cast<double>(Step) / Steps;
		const int Column = static_cast<int>(std::floor(StartX + (EndX - StartX) * Along));
		const int Row = static_cast<int>(std::floor(StartY + (EndY - StartY) * Along));
		if (Column < 0 || Row < 0 || Column >= Frame.Columns || Row >= Frame.Rows)
		{
			continue;
		}
		Mask[static_cast<size_t>(Row) * Frame.Columns + Column] = 1;
	}
}

/** The scipy Gaussian kernel: radius int(4 sigma + 0.5), normalised weights. */
std::vector<double> GaussianKernel(double Sigma)
{
	const int Radius = static_cast<int>(4.0 * Sigma + 0.5);
	std::vector<double> Kernel(2 * Radius + 1);
	double Sum = 0.0;
	for (int Offset = -Radius; Offset <= Radius; ++Offset)
	{
		Kernel[Offset + Radius] = std::exp(-0.5 * Offset * Offset / (Sigma * Sigma));
		Sum += Kernel[Offset + Radius];
	}
	for (double& Weight : Kernel)
	{
		Weight /= Sum;
	}
	return Kernel;
}

/** scipy's "reflect" mode: d c b a | a b c d | d c b a. */
int ReflectIndex(int Index, int Count)
{
	const int Period = 2 * Count;
	Index %= Period;
	if (Index < 0)
	{
		Index += Period;
	}
	if (Index >= Count)
	{
		Index = Period - 1 - Index;
	}
	return Index;
}

/** Convolves one line of values (stride apart) with the kernel, in place. */
void ConvolveLine(double* Values, int Count, size_t Stride, const std::vector<double>& Kernel, std::vector<double>& Scratch)
{
	const int Radius = static_cast<int>(Kernel.size() / 2);
	Scratch.assign(Count, 0.0);
	for (int Index = 0; Index < Count; ++Index)
	{
		double Sum = 0.0;
		for (int Offset = -Radius; Offset <= Radius; ++Offset)
		{
			Sum += Kernel[Offset + Radius] * Values[ReflectIndex(Index + Offset, Count) * Stride];
		}
		Scratch[Index] = Sum;
	}
	for (int Index = 0; Index < Count; ++Index)
	{
		Values[Index * Stride] = Scratch[Index];
	}
}

/**
 * One line of the Felzenszwalb and Huttenlocher squared distance transform: for every position, the smallest
 * (position - source)^2 + Cost[source] and the source it comes from.
 */
void LowerEnvelope(const std::vector<double>& Cost, std::vector<double>& Result, std::vector<int>& Source)
{
	const int Count = static_cast<int>(Cost.size());
	std::vector<int> Parabolas(Count);
	std::vector<double> Boundaries(Count + 1);
	int Top = -1;
	for (int Position = 0; Position < Count; ++Position)
	{
		if (!std::isfinite(Cost[Position]))
		{
			continue;
		}
		double Intersection = -std::numeric_limits<double>::infinity();
		while (Top >= 0)
		{
			const int Previous = Parabolas[Top];
			Intersection = ((Cost[Position] + Position * Position) - (Cost[Previous] + Previous * Previous))
				/ (2.0 * (Position - Previous));
			if (Intersection > Boundaries[Top])
			{
				break;
			}
			--Top;
		}
		++Top;
		Parabolas[Top] = Position;
		Boundaries[Top] = Top == 0 ? -std::numeric_limits<double>::infinity() : Intersection;
		Boundaries[Top + 1] = std::numeric_limits<double>::infinity();
	}
	Result.assign(Count, std::numeric_limits<double>::infinity());
	Source.assign(Count, -1);
	if (Top < 0)
	{
		return;
	}
	int Parabola = 0;
	for (int Position = 0; Position < Count; ++Position)
	{
		while (Boundaries[Parabola + 1] < Position)
		{
			++Parabola;
		}
		const int Origin = Parabolas[Parabola];
		Result[Position] = (Position - Origin) * static_cast<double>(Position - Origin) + Cost[Origin];
		Source[Position] = Origin;
	}
}
}

FMask RasterizePolygons(const FGridFrame& Frame, const FPolygons& Polygons, bool bAllTouched)
{
	FMask Mask(Frame.CellCount(), 0);
	if (Polygons.empty())
	{
		return Mask;
	}
	FillCellCentres(Frame, Polygons, Mask);
	if (!bAllTouched)
	{
		return Mask;
	}
	for (const Clipper2Lib::PathD& Ring : Polygons)
	{
		for (size_t Index = 0; Index < Ring.size(); ++Index)
		{
			MarkSegmentCells(Frame, Ring[Index], Ring[(Index + 1) % Ring.size()], Mask);
		}
	}
	return Mask;
}

void GaussianFilter(std::vector<double>& Values, int Columns, int Rows, double Sigma)
{
	const std::vector<double> Kernel = GaussianKernel(Sigma);
	std::vector<double> Scratch;
	for (int Row = 0; Row < Rows; ++Row)
	{
		ConvolveLine(Values.data() + static_cast<size_t>(Row) * Columns, Columns, 1, Kernel, Scratch);
	}
	for (int Column = 0; Column < Columns; ++Column)
	{
		ConvolveLine(Values.data() + Column, Rows, static_cast<size_t>(Columns), Kernel, Scratch);
	}
}

FDistanceField DistanceToFeatures(const FMask& IsFeature, int Columns, int Rows, double CellSize)
{
	const double Infinity = std::numeric_limits<double>::infinity();
	// First pass along columns: squared distance to the nearest feature in the same column, and its row.
	std::vector<double> ColumnDistance(IsFeature.size(), Infinity);
	std::vector<int> ColumnSourceRow(IsFeature.size(), -1);
	std::vector<double> Cost(Rows);
	std::vector<double> Result;
	std::vector<int> Source;
	for (int Column = 0; Column < Columns; ++Column)
	{
		for (int Row = 0; Row < Rows; ++Row)
		{
			Cost[Row] = IsFeature[static_cast<size_t>(Row) * Columns + Column] ? 0.0 : Infinity;
		}
		LowerEnvelope(Cost, Result, Source);
		for (int Row = 0; Row < Rows; ++Row)
		{
			ColumnDistance[static_cast<size_t>(Row) * Columns + Column] = Result[Row];
			ColumnSourceRow[static_cast<size_t>(Row) * Columns + Column] = Source[Row];
		}
	}
	// Second pass along rows over the column results.
	FDistanceField Field;
	Field.Distance.assign(IsFeature.size(), Infinity);
	Field.Nearest.assign(IsFeature.size(), -1);
	Cost.resize(Columns);
	for (int Row = 0; Row < Rows; ++Row)
	{
		const size_t RowStart = static_cast<size_t>(Row) * Columns;
		std::copy(ColumnDistance.begin() + RowStart, ColumnDistance.begin() + RowStart + Columns, Cost.begin());
		LowerEnvelope(Cost, Result, Source);
		for (int Column = 0; Column < Columns; ++Column)
		{
			if (Source[Column] < 0)
			{
				continue;
			}
			const int NearestRow = ColumnSourceRow[RowStart + Source[Column]];
			Field.Distance[RowStart + Column] = std::sqrt(Result[Column]) * CellSize;
			Field.Nearest[RowStart + Column] = static_cast<int64_t>(NearestRow) * Columns + Source[Column];
		}
	}
	return Field;
}
}
