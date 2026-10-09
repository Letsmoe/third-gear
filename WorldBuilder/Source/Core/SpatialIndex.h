#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Geometry.h"

namespace WorldBuilder
{
/** Items by bounding box in buckets of a fixed size, so a tile finds the features near it without a full scan. */
class FSpatialIndex
{
public:
	explicit FSpatialIndex(double InBucketSize = 250.0) : BucketSize(InBucketSize) {}

	void Insert(const FBox& Box, int Item)
	{
		Boxes.push_back(Box);
		Items.push_back(Item);
		const int Entry = static_cast<int>(Items.size()) - 1;
		ForEachBucket(Box, [&](int64_t Key) { Buckets[Key].push_back(Entry); });
	}

	/** The items whose boxes overlap the box, each once, in insertion order. Safe to call from many threads. */
	std::vector<int> Query(const FBox& Box) const
	{
		std::vector<int> Entries;
		ForEachBucket(Box, [&](int64_t Key) {
			const auto Bucket = Buckets.find(Key);
			if (Bucket == Buckets.end())
			{
				return;
			}
			for (const int Entry : Bucket->second)
			{
				if (Boxes[Entry].Overlaps(Box))
				{
					Entries.push_back(Entry);
				}
			}
		});
		std::sort(Entries.begin(), Entries.end());
		Entries.erase(std::unique(Entries.begin(), Entries.end()), Entries.end());
		std::vector<int> Found;
		Found.reserve(Entries.size());
		for (const int Entry : Entries)
		{
			Found.push_back(Items[Entry]);
		}
		return Found;
	}

private:
	double BucketSize;
	std::vector<FBox> Boxes;
	std::vector<int> Items;
	std::unordered_map<int64_t, std::vector<int>> Buckets;

	template <typename FVisit>
	void ForEachBucket(const FBox& Box, FVisit&& Visit) const
	{
		const int64_t ColumnStart = static_cast<int64_t>(std::floor(Box.X0 / BucketSize));
		const int64_t ColumnEnd = static_cast<int64_t>(std::floor(Box.X1 / BucketSize));
		const int64_t RowStart = static_cast<int64_t>(std::floor(Box.Y0 / BucketSize));
		const int64_t RowEnd = static_cast<int64_t>(std::floor(Box.Y1 / BucketSize));
		for (int64_t Row = RowStart; Row <= RowEnd; ++Row)
		{
			for (int64_t Column = ColumnStart; Column <= ColumnEnd; ++Column)
			{
				Visit(Row * 1000003 + Column);
			}
		}
	}
};
}
