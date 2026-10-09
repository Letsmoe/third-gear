#include "Raster.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace WorldBuilder
{
namespace
{
/** A polygon edge that crosses some row centres: its lower end, its slope, and the rows whose centres it crosses. */
struct FScanEdge
{
	double StartX;
	double StartY;
	double InverseSlope;
	int FirstRow;
	int LastRow;
};

/** The edges of the polygons bucketed by the first row centre they cross (half-open in y, as even-odd needs). */
std::vector<std::vector<FScanEdge>> EdgeTable(const FGridFrame& Frame, const FPolygons& Polygons)
{
	std::vector<std::vector<FScanEdge>> Table(Frame.Rows);
	for (const Clipper2Lib::PathD& Ring : Polygons)
	{
		const size_t Count = Ring.size();
		for (size_t Index = 0; Index < Count; ++Index)
		{
			Clipper2Lib::PointD Low = Ring[Index];
			Clipper2Lib::PointD High = Ring[(Index + 1) % Count];
			if (Low.y == High.y)
			{
				continue;
			}
			if (Low.y > High.y)
			{
				std::swap(Low, High);
			}
			// Rows whose centre y lies in [Low.y, High.y).
			const int FirstRow = std::max(0, static_cast<int>(std::ceil((Low.y - Frame.Y0) / Frame.CellSize - 0.5)));
			const int LastRow = std::min(Frame.Rows - 1, static_cast<int>(std::ceil((High.y - Frame.Y0) / Frame.CellSize - 0.5)) - 1);
			if (FirstRow > LastRow)
			{
				continue;
			}
			Table[FirstRow].push_back({Low.x, Low.y, (High.x - Low.x) / (High.y - Low.y), FirstRow, LastRow});
		}
	}
	return Table;
}

/** Marks the cells whose centres lie inside the polygons, row by row over the edges crossing each row. */
void FillCellCentres(const FGridFrame& Frame, const FPolygons& Polygons, FMask& Mask)
{
	const std::vector<std::vector<FScanEdge>> Table = EdgeTable(Frame, Polygons);
	std::vector<FScanEdge> Active;
	std::vector<double> Crossings;
	for (int Row = 0; Row < Frame.Rows; ++Row)
	{
		Active.erase(std::remove_if(Active.begin(), Active.end(), [Row](const FScanEdge& Edge) { return Edge.LastRow < Row; }),
					 Active.end());
		Active.insert(Active.end(), Table[Row].begin(), Table[Row].end());
		if (Active.empty())
		{
			continue;
		}
		const double Y = Frame.Y0 + (Row + 0.5) * Frame.CellSize;
		Crossings.clear();
		for (const FScanEdge& Edge : Active)
		{
			Crossings.push_back(Edge.StartX + (Y - Edge.StartY) * Edge.InverseSlope);
		}
		std::sort(Crossings.begin(), Crossings.end());
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

/** Convolves one line of values (stride apart) with the kernel, in place, mirroring it past both ends. */
void ConvolveLine(double* Values, int Count, size_t Stride, const std::vector<double>& Kernel, std::vector<double>& Padded)
{
	const int Radius = static_cast<int>(Kernel.size() / 2);
	Padded.resize(Count + 2 * Radius);
	for (int Index = -Radius; Index < Count + Radius; ++Index)
	{
		Padded[Index + Radius] = Values[ReflectIndex(Index, Count) * Stride];
	}
	const double* Weights = Kernel.data();
	const int Taps = static_cast<int>(Kernel.size());
	for (int Index = 0; Index < Count; ++Index)
	{
		const double* Window = Padded.data() + Index;
		double Sum = 0.0;
		for (int Tap = 0; Tap < Taps; ++Tap)
		{
			Sum += Weights[Tap] * Window[Tap];
		}
		Values[Index * Stride] = Sum;
	}
}

/**
 * One line of the Felzenszwalb and Huttenlocher squared distance transform: for every position, the smallest
 * (position - source)^2 + Cost[source] and the source it comes from.
 */
void LowerEnvelope(const std::vector<double>& Cost, std::vector<double>& Result, std::vector<int>& Source,
				   std::vector<int>& Parabolas, std::vector<double>& Boundaries)
{
	const int Count = static_cast<int>(Cost.size());
	Parabolas.resize(Count);
	Boundaries.resize(Count + 1);
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
	Result.resize(Count);
	Source.resize(Count);
	if (Top < 0)
	{
		std::fill(Result.begin(), Result.end(), std::numeric_limits<double>::infinity());
		std::fill(Source.begin(), Source.end(), -1);
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
	const int Radius = static_cast<int>(Kernel.size() / 2);
	std::vector<double> Scratch;
	for (int Row = 0; Row < Rows; ++Row)
	{
		ConvolveLine(Values.data() + static_cast<size_t>(Row) * Columns, Columns, 1, Kernel, Scratch);
	}
	// Down the columns, whole rows at a time, so memory is read in order.
	std::vector<double> Result(Values.size(), 0.0);
	for (int Row = 0; Row < Rows; ++Row)
	{
		double* Output = Result.data() + static_cast<size_t>(Row) * Columns;
		for (int Tap = -Radius; Tap <= Radius; ++Tap)
		{
			const double Weight = Kernel[Tap + Radius];
			const double* Input = Values.data() + static_cast<size_t>(ReflectIndex(Row + Tap, Rows)) * Columns;
			for (int Column = 0; Column < Columns; ++Column)
			{
				Output[Column] += Weight * Input[Column];
			}
		}
	}
	Values.swap(Result);
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
	std::vector<int> Parabolas;
	std::vector<double> Boundaries;
	for (int Column = 0; Column < Columns; ++Column)
	{
		for (int Row = 0; Row < Rows; ++Row)
		{
			Cost[Row] = IsFeature[static_cast<size_t>(Row) * Columns + Column] ? 0.0 : Infinity;
		}
		LowerEnvelope(Cost, Result, Source, Parabolas, Boundaries);
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
		LowerEnvelope(Cost, Result, Source, Parabolas, Boundaries);
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
