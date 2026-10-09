#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace WorldBuilder
{
/** The number of worker threads to use for ThreadCount: all cores when it is 0. */
inline unsigned ResolveThreadCount(unsigned ThreadCount)
{
	if (ThreadCount != 0)
	{
		return ThreadCount;
	}
	return std::max(1u, std::thread::hardware_concurrency());
}

/** Calls Body(Index) for every index below Count on ThreadCount threads, handing out the indices in small batches. */
template <typename FBody>
void ParallelFor(size_t Count, unsigned ThreadCount, const FBody& Body)
{
	constexpr size_t BatchSize = 64;
	std::atomic<size_t> NextIndex{0};
	auto Work = [&]() {
		while (true)
		{
			const size_t Begin = NextIndex.fetch_add(BatchSize);
			if (Begin >= Count)
			{
				return;
			}
			const size_t End = std::min(Begin + BatchSize, Count);
			for (size_t Index = Begin; Index < End; ++Index)
			{
				Body(Index);
			}
		}
	};
	std::vector<std::thread> Threads;
	for (unsigned Index = 1; Index < ThreadCount; ++Index)
	{
		Threads.emplace_back(Work);
	}
	Work();
	for (std::thread& Thread : Threads)
	{
		Thread.join();
	}
}
}
