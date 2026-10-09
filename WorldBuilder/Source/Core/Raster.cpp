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

namespace
{
/** Below this sigma the recursive filter is less accurate than the direct kernel, which is cheap there anyway. */
constexpr double RecursiveGaussianMinimumSigma = 1.5;

/** The coefficients of Young and van Vliet's recursive Gaussian (1995): B and b1..b3 divided by b0. */
struct FRecursiveGaussian
{
	double Gain;
	double Feedback[3];
	/** Samples mirrored past each end before filtering, so the edges behave like scipy's "reflect". */
	int Padding;
};

FRecursiveGaussian RecursiveCoefficients(double Sigma)
{
	const double Q = Sigma >= 2.5 ? 0.98711 * Sigma - 0.96330 : 3.97156 - 4.14554 * std::sqrt(1.0 - 0.26891 * Sigma);
	const double B0 = 1.57825 + 2.44413 * Q + 1.4281 * Q * Q + 0.422205 * Q * Q * Q;
	const double B1 = 2.44413 * Q + 2.85619 * Q * Q + 1.26661 * Q * Q * Q;
	const double B2 = -(1.4281 * Q * Q + 1.26661 * Q * Q * Q);
	const double B3 = 0.422205 * Q * Q * Q;
	return {1.0 - (B1 + B2 + B3) / B0, {B1 / B0, B2 / B0, B3 / B0}, static_cast<int>(std::ceil(4.0 * Sigma))};
}

/**
 * Filters rows of values (each Count long, Stride apart in memory, Lanes of them side by side and contiguous) along
 * their length: a forward and a backward pass over the mirrored-padded rows, all lanes at once so the loops
 * vectorise.
 */
void RecursiveFilterLines(double* Values, int Count, size_t Stride, int Lanes, const FRecursiveGaussian& Filter,
						  std::vector<double>& Work)
{
	const int Padded = Count + 2 * Filter.Padding;
	Work.assign(static_cast<size_t>(Padded) * Lanes, 0.0);
	for (int Index = 0; Index < Padded; ++Index)
	{
		const double* Source = Values + ReflectIndex(Index - Filter.Padding, Count) * Stride;
		std::copy(Source, Source + Lanes, Work.data() + static_cast<size_t>(Index) * Lanes);
	}
	const double Gain = Filter.Gain;
	const double F1 = Filter.Feedback[0];
	const double F2 = Filter.Feedback[1];
	const double F3 = Filter.Feedback[2];
	auto Line = [&](int Index) { return Work.data() + static_cast<size_t>(Index) * Lanes; };
	// The first three samples start from the steady state of a constant input (their own value).
	for (int Index = 3; Index < Padded; ++Index)
	{
		double* Out = Line(Index);
		const double* Previous1 = Line(Index - 1);
		const double* Previous2 = Line(Index - 2);
		const double* Previous3 = Line(Index - 3);
		for (int Lane = 0; Lane < Lanes; ++Lane)
		{
			Out[Lane] = Gain * Out[Lane] + F1 * Previous1[Lane] + F2 * Previous2[Lane] + F3 * Previous3[Lane];
		}
	}
	for (int Index = Padded - 4; Index >= 0; --Index)
	{
		double* Out = Line(Index);
		const double* Next1 = Line(Index + 1);
		const double* Next2 = Line(Index + 2);
		const double* Next3 = Line(Index + 3);
		for (int Lane = 0; Lane < Lanes; ++Lane)
		{
			Out[Lane] = Gain * Out[Lane] + F1 * Next1[Lane] + F2 * Next2[Lane] + F3 * Next3[Lane];
		}
	}
	for (int Index = 0; Index < Count; ++Index)
	{
		const double* Source = Line(Index + Filter.Padding);
		std::copy(Source, Source + Lanes, Values + Index * Stride);
	}
}

/** The direct, truncated kernel of scipy, for small sigmas. */
void DirectGaussianFilter(std::vector<double>& Values, int Columns, int Rows, double Sigma)
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

/** Transposes a row-major grid. */
std::vector<double> Transposed(const std::vector<double>& Values, int Columns, int Rows)
{
	std::vector<double> Result(Values.size());
	for (int Row = 0; Row < Rows; ++Row)
	{
		for (int Column = 0; Column < Columns; ++Column)
		{
			Result[static_cast<size_t>(Column) * Rows + Row] = Values[static_cast<size_t>(Row) * Columns + Column];
		}
	}
	return Result;
}
}

void GaussianFilter(std::vector<double>& Values, int Columns, int Rows, double Sigma)
{
	if (Sigma < RecursiveGaussianMinimumSigma)
	{
		DirectGaussianFilter(Values, Columns, Rows, Sigma);
		return;
	}
	const FRecursiveGaussian Filter = RecursiveCoefficients(Sigma);
	std::vector<double> Work;
	// Down the columns: rows are the steps, all columns filtered side by side.
	RecursiveFilterLines(Values.data(), Rows, static_cast<size_t>(Columns), Columns, Filter, Work);
	// Along the rows the same way, on the transposed grid.
	std::vector<double> Flipped = Transposed(Values, Columns, Rows);
	RecursiveFilterLines(Flipped.data(), Columns, static_cast<size_t>(Rows), Rows, Filter, Work);
	Values = Transposed(Flipped, Rows, Columns);
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
