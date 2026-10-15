#include "RoadDesign.h"

#include <unordered_map>

namespace WorldBuilder
{
namespace
{
// Grades follow the German RAA and RASt guidance: 4 % on motorways, 6 to 8 % on main roads, 12 % and more in towns.
const std::unordered_map<std::string, FRoadDesign> DesignByClass = {
	{"motorway", {30.0, 2.0, 25.0, 0.04}},
	{"motorway_link", {16.0, 1.5, 16.0, 0.06}},
	{"trunk", {24.0, 2.0, 20.0, 0.05}},
	{"trunk_link", {14.0, 1.5, 14.0, 0.07}},
	{"primary", {16.0, 1.5, 16.0, 0.06}},
	{"primary_link", {12.0, 1.2, 12.0, 0.08}},
	{"secondary", {14.0, 1.2, 14.0, 0.08}},
	{"secondary_link", {10.0, 1.0, 12.0, 0.08}},
	{"tertiary", {12.0, 1.0, 12.0, 0.10}},
	{"tertiary_link", {10.0, 1.0, 10.0, 0.10}},
	{"unclassified", {10.0, 1.0, 10.0, 0.12}},
	{"residential", {8.0, 1.0, 10.0, 0.12}},
	{"living_street", {6.0, 0.8, 8.0, 0.15}},
	{"service", {6.0, 0.8, 8.0, 0.15}},
	{"track", {6.0, 0.8, 8.0, 0.18}},
};
}

FRoadDesign RoadDesignFor(const std::string& RoadClass)
{
	const auto Found = DesignByClass.find(RoadClass);
	if (Found == DesignByClass.end())
	{
		return DesignByClass.at("residential");
	}
	return Found->second;
}
}
