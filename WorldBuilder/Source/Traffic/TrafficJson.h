#pragma once

#include <string>

#include "TrafficTypes.h"

namespace WorldBuilder
{
/** A double written the way Python's repr writes it (the shortest text that reads back as the same number). */
std::string PythonDouble(double Value);

/** The traffic network as the text json.dump(traffic, separators=(",", ":")) writes. */
std::string TrafficJsonText(const FTrafficNetwork& Network);

/** Writes traffic.json; returns false when the file can't be written. */
bool WriteTrafficJson(const std::string& Path, const FTrafficNetwork& Network);
}
