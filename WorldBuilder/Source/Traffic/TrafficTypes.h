#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Polyline.h"

/** The street furniture and signal data the world needs, as plain structs (Python: furniture.py's dictionaries). */
namespace WorldBuilder
{
/** One direction of travel into a signalised junction, as traffic.json records it (values rounded like the file). */
struct FApproachRecord
{
	int Id = 0;
	int Phase = 0;
	int64_t Way = 0;
	/** x0, y0, x1, y1 of the stop line: from the lane centre line (two-way) or left edge (one-way) to the right edge. */
	double StopLine[4] = {0.0, 0.0, 0.0, 0.0};
	double Direction[2] = {1.0, 0.0};
	int Lanes = 1;
	double SpeedKmh = 50.0;
};

/** One signal phase in seconds. */
struct FPhaseRecord
{
	double Green = 0.0;
	double Amber = 0.0;
	double Clearance = 0.0;
	bool bPedestrian = false;
};

struct FJunctionRecord
{
	int Id = 0;
	double X = 0.0;
	double Y = 0.0;
	bool bCrossingOnly = false;
	std::vector<FPhaseRecord> Phases;
	std::vector<FApproachRecord> Approaches;
};

/** A drivable way with its speed limit, for the runtime speed limit query. */
struct FSpeedWay
{
	int64_t Id = 0;
	double LimitKmh = 0.0;
	double Width = 0.0;
	bool bOneway = false;
	/** The way's points rounded to centimetres. */
	std::vector<FStreetPoint> Points;
};

/** The contents of traffic.json. */
struct FTrafficNetwork
{
	std::vector<FJunctionRecord> Junctions;
	std::vector<FSpeedWay> SpeedWays;
};

struct FLamp
{
	double X = 0.0;
	double Y = 0.0;
	double YawDegrees = 0.0;
	/** True for a lamp OSM maps, false for one placed along a lit road. */
	bool bFromOsm = false;
};

/** A pole carrying the signal heads of an approach. */
struct FSignalHead
{
	double X = 0.0;
	double Y = 0.0;
	double YawDegrees = 0.0;
	int Approach = 0;
	int Junction = 0;
	/** True for the pole on the left of a wide one-way road. */
	bool bLeftSide = false;
};

/** A sign pole; Names are the sign graphics stacked top to bottom. */
struct FSign
{
	double X = 0.0;
	double Y = 0.0;
	double YawDegrees = 0.0;
	std::vector<std::string> Names;
};

/** A zebra crossing on a road: its centre on the road's line, the road direction there and the carriageway width. */
struct FZebra
{
	double X = 0.0;
	double Y = 0.0;
	FStreetPoint Direction;
	double Width = 0.0;
};

/** Everything FurnitureBuilder.build derives. */
struct FFurniture
{
	FTrafficNetwork Network;
	std::vector<FLamp> Lamps;
	std::vector<FSignalHead> Heads;
	std::vector<FSign> Signs;
	std::vector<FZebra> Zebras;
};
}
