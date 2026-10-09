#include "ChunkedPolygons.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace WorldBuilder
{
void FChunkedPolygons::Add(int Owner, const FPolygons& Polygon)
{
	if (Polygon.empty())
	{
		return;
	}
	const FBox Bounds = BoundsOf(Polygon);
	const int64_t FirstColumn = static_cast<int64_t>(std::floor(Bounds.X0 / ChunkSize));
	const int64_t FirstRow = static_cast<int64_t>(std::floor(Bounds.Y0 / ChunkSize));
	const int64_t LastColumn = static_cast<int64_t>(std::floor(Bounds.X1 / ChunkSize));
	const int64_t LastRow = static_cast<int64_t>(std::floor(Bounds.Y1 / ChunkSize));
	Split(Owner, Polygon, FirstColumn, FirstRow, LastColumn - FirstColumn + 1, LastRow - FirstRow + 1);
}

void FChunkedPolygons::Split(int Owner, const FPolygons& Polygon, int64_t FirstColumn, int64_t FirstRow,
							 int64_t Columns, int64_t Rows)
{
	if (Polygon.empty())
	{
		return;
	}
	if (Columns == 1 && Rows == 1)
	{
		Chunks[Key(FirstColumn, FirstRow)].push_back({Owner, Polygon});
		return;
	}
	const double X0 = FirstColumn * ChunkSize;
	const double Y0 = FirstRow * ChunkSize;
	if (Columns >= Rows)
	{
		const int64_t Half = Columns / 2;
		const double Middle = X0 + Half * ChunkSize;
		const double Bottom = Y0 + Rows * ChunkSize;
		Split(Owner, ClipToBox(Polygon, {X0, Y0, Middle, Bottom}), FirstColumn, FirstRow, Half, Rows);
		Split(Owner, ClipToBox(Polygon, {Middle, Y0, X0 + Columns * ChunkSize, Bottom}), FirstColumn + Half, FirstRow,
			  Columns - Half, Rows);
		return;
	}
	const int64_t Half = Rows / 2;
	const double Middle = Y0 + Half * ChunkSize;
	const double Right = X0 + Columns * ChunkSize;
	Split(Owner, ClipToBox(Polygon, {X0, Y0, Right, Middle}), FirstColumn, FirstRow, Columns, Half);
	Split(Owner, ClipToBox(Polygon, {X0, Middle, Right, Y0 + Rows * ChunkSize}), FirstColumn, FirstRow + Half, Columns,
		  Rows - Half);
}

std::vector<std::pair<int, FPolygons>> FChunkedPolygons::InWindow(const FBox& Window) const
{
	std::map<int, FPolygons> ByOwner;
	const int64_t FirstColumn = static_cast<int64_t>(std::floor(Window.X0 / ChunkSize));
	const int64_t LastColumn = static_cast<int64_t>(std::floor(Window.X1 / ChunkSize));
	const int64_t FirstRow = static_cast<int64_t>(std::floor(Window.Y0 / ChunkSize));
	const int64_t LastRow = static_cast<int64_t>(std::floor(Window.Y1 / ChunkSize));
	for (int64_t Row = FirstRow; Row <= LastRow; ++Row)
	{
		for (int64_t Column = FirstColumn; Column <= LastColumn; ++Column)
		{
			const auto Chunk = Chunks.find(Key(Column, Row));
			if (Chunk == Chunks.end())
			{
				continue;
			}
			for (const FPiece& Piece : Chunk->second)
			{
				const FPolygons Clipped = ClipToBox(Piece.Polygon, Window);
				FPolygons& Target = ByOwner[Piece.Owner];
				Target.insert(Target.end(), Clipped.begin(), Clipped.end());
			}
		}
	}
	std::vector<std::pair<int, FPolygons>> Result;
	for (auto& [Owner, Pieces] : ByOwner)
	{
		// Pieces of neighbouring chunks share edges; merging them gives one clean outline per owner.
		Result.emplace_back(Owner, UnionOf(Pieces));
	}
	return Result;
}

FPolygons FChunkedPolygons::UnionInWindow(const FBox& Window) const
{
	FPolygons All;
	for (const auto& [Owner, Pieces] : InWindow(Window))
	{
		All.insert(All.end(), Pieces.begin(), Pieces.end());
	}
	return UnionOf(All);
}
}
