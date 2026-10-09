#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "OsmData.h"
#include "PolygonSet.h"
#include "RoadGraph.h"
#include "TrafficTypes.h"

/**
 * Street parking: which sides of a way cars park on (from the OSM tags or a default by street width) and where the
 * parked cars stand (Python: parking.py).
 *
 * Tags: the new scheme (parking:left, parking:right, parking:both with the orientation in parking:<side>:orientation)
 * and the old one (parking:lane:<side>=parallel|diagonal|perpendicular|no). Only parallel parking on the carriageway
 * is placed; diagonal, perpendicular and separate parking (lay-bys, lots) is left to the area polygons. A way with any
 * parking tag is taken as tagged: untagged sides get no cars. Untagged residential streets and living streets get cars
 * on one side from ParkingMinWidth and on both from ParkingBothWidth.
 *
 * Right of a way means right of its node order, as everywhere else.
 */
namespace WorldBuilder
{
// Untagged streets only get parked cars where two moving cars still fit beside them (two cars of 1.8 m plus 0.2 m, and
// the 1.95 m strip of each parked row), so AI traffic keeps its lanes there.
inline constexpr double ParkingMinWidth = 5.75;
inline constexpr double ParkingBothWidth = 7.7;
// Tagged streets keep their cars even when only a single file passes; two rows need this much width to fit at all.
inline constexpr double TaggedBothMinWidth = 6.4;
// Width of the strip a parked car takes from the kerb, including the gap to the kerb and the mirror.
inline constexpr double ParkingStrip = 1.95;
inline constexpr double CarWidth = 1.85;

/** A car model of the parked car set (the City Sample models of the AI traffic), in the order of ParkedCars.cpp. */
struct FParkedCarModel
{
	const char* Name;
	double Length;
	double Weight;
};

/** The models a parked car can be: index into this list is the car's model. */
const std::vector<FParkedCarModel>& ParkedCarModels();

/** One side of the road cars park on, and how: lane, street_side, half_on_kerb or on_kerb. */
struct FParkingSide
{
	/** +1 right of the way's node order, -1 left. */
	int Sign = 0;
	std::string Position;
};

/** The sides cars park on along a way, right before left. */
std::vector<FParkingSide> ParkingSides(const FTags& Tags, int64_t WayId, double Width);

// Half the width of a moving car plus a hand's breadth.
inline constexpr double LaneClearance = 0.9;

/**
 * Offset of the lane of one direction of travel (positive towards its own kerb) after making room for parked cars: the
 * cars on the lane's own side push it towards the centre line, cars on the opposite side towards its own kerb. Without
 * parked cars the base offset is returned unchanged.
 */
double LaneOffsetWithParking(double BaseOffset, double Width, int Travel, const std::vector<FParkingSide>& Sides);

/** A parked car's pose on the plane, without the heights, which the height field gives (FinishParkedCar). */
struct FParkedCar
{
	/** The middle of the car. */
	double X = 0.0;
	double Y = 0.0;
	double YawDegrees = 0.0;
	int Model = 0;
	double Length = 0.0;
	/** The unit direction the car points in. */
	FStreetPoint Heading;
	/** Degrees the kerb side is lifted (3 when two wheels stand on the kerb, else 0). */
	double KerbTilt = 0.0;
	/** +1 when the car is on the right of the way, -1 on the left. */
	int Sign = 1;
	/** +1 when the car points along the way's node order. */
	double Facing = 1.0;
	/** The four corners of the car's footprint. */
	FStreetPoint Corners[4];
};

/** The height, pitch and roll of a parked car. */
struct FParkedCarPose
{
	double Z = 0.0;
	double PitchDegrees = 0.0;
	double RollDegrees = 0.0;
};

/**
 * Finishes a car's z, pitch and roll from the road height at points under its axles (_car_record): RoadHeight gives
 * the road surface height at a place.
 */
FParkedCarPose FinishParkedCar(const FParkedCar& Car, const std::function<double(double X, double Y)>& RoadHeight);

/**
 * Places parked cars along the ways of the road graph, keeping the legal gaps of section 12 StVO. Zebras: the zebra
 * crossings of the furniture; Carriageway: the paved driving surface; Region: only ways touching this box are filled.
 */
std::vector<FParkedCar> BuildParkedCars(const FOsmData& Data, const FRoadGraph& Graph, const std::vector<FZebra>& Zebras,
										const FPolygonSet& Carriageway, const FPolygonSet& Buildings,
										const std::optional<FBox>& Region);
}
