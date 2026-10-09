#include "Assumptions.h"

#include <array>

namespace WorldBuilder::Assumptions
{
int RoadClassRank(const std::string& RoadClass)
{
	static const std::array<const char*, 15> ClassesLowestFirst = {
		"service", "living_street", "road", "residential", "unclassified", "tertiary_link", "tertiary",
		"secondary_link", "secondary", "primary_link", "primary", "trunk_link", "trunk", "motorway_link", "motorway"};
	for (size_t Rank = 0; Rank < ClassesLowestFirst.size(); ++Rank)
	{
		if (RoadClass == ClassesLowestFirst[Rank])
		{
			return static_cast<int>(Rank);
		}
	}
	return 0;
}
}
