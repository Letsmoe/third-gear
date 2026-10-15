#pragma once

#include <vector>

#include "Network.h"

/**
 * The ground ways redrawn along the road model's smooth centrelines, before the street model builds anything from
 * them, so the surface, kerbs, pavements, markings and lanes follow the same line as the road's heights.
 */
namespace WorldBuilder
{
/**
 * The ways with every chain of their segments redrawn along its fitted centreline (FitCentreline with the chain's
 * road design). Nodes keep their ids and move to the same share of the redrawn line between their neighbours; the
 * chains' ends and junction nodes stay where they are, and shape points are added about every two metres.
 */
std::vector<FStreetWay> SmoothWays(const std::vector<FStreetWay>& Ways, const FSectionsByWay& Sections);
}
