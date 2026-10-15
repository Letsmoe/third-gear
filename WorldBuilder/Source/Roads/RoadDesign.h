#pragma once

#include <string>

/**
 * Design values of the road model by road class: how smooth a centreline and a height profile are, and how far they
 * may leave the OSM line and the ground. Tuned by eye against the aerial photo and the survey (issue #113).
 */
namespace WorldBuilder
{
struct FRoadDesign
{
	/**
	 * Length in metres over which the centreline's curvature is smoothed. Bends tighter than this are rounded, as far
	 * as MaxDeviation lets them.
	 */
	double PlanSmoothing = 8.0;
	/** Furthest the centreline may move away from the OSM line, metres. */
	double MaxDeviation = 1.0;
	/** Length in metres over which the change of grade is smoothed: humps shorter than about six times this are cut. */
	double ProfileSmoothing = 10.0;
	/** Steepest grade, rise over run. */
	double MaxGrade = 0.12;
};

/** The design values of a highway= class; unknown classes get those of a residential street. */
FRoadDesign RoadDesignFor(const std::string& RoadClass);
}
