#pragma once

#include <functional>
#include <vector>

#include "Geometry.h"
#include "KitSpecs.h"
#include "OsmReader.h"
#include "Structures.h"

/**
 * The tall structures of a region, placed once for all of it (structures.tgtile) so they show from far away: wind
 * turbines, pylons with the conductors between them, and the landmarks that replace their OSM building.
 *
 * - Turbines: the kit model from the OSM manufacturer, model or hub height; all turned into the prevailing
 *   west-south-west wind, the rotor spinning.
 * - Pylons: the kit design from the tower's design tag (or its line's), anchors where the line bends by more than
 *   AnchorAngleDegrees or ends, turned so the cross arms stand across the line. Conductors hang from socket to
 *   socket between neighbouring towers of a line.
 * - Landmarks: every landmark of the kit at its surveyed position.
 */
namespace WorldBuilder
{
/** Ground height at a world point. */
using FGroundSampler = std::function<double(double X, double Y)>;

std::vector<FStructurePlacement> BuildFarStructures(const FFarStructureSources& Sources, const FKitSpecs& Specs,
												   const FGroundSampler& Ground);

/** The footprints of the kit's landmarks, world metres: OSM buildings standing in them are left out. */
FPolygons LandmarkFootprints(const FKitSpecs& Specs);
}
