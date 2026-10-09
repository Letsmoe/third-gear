#pragma once

/**
 * World coordinates, as in Tools/osmimport/osmimport/geo.py: ETRS89 / UTM zone 32N (EPSG:25832, the CRS of the
 * Hamburg open data) relative to a fixed origin in Bergedorf, x east and y south, in metres.
 */
namespace WorldBuilder
{
/** The project origin in UTM metres, geo.py's ORIGIN_E and ORIGIN_N. */
constexpr double OriginEast = 580416.0;
constexpr double OriginNorth = 5927242.0;

struct FWorldPoint
{
	double X = 0.0;
	double Y = 0.0;
};

struct FUtmPoint
{
	double East = 0.0;
	double North = 0.0;
};

struct FLonLat
{
	double Longitude = 0.0;
	double Latitude = 0.0;
};

/** UTM zone 32N easting and northing of a WGS84 longitude and latitude in degrees (Krüger series to n^6). */
FUtmPoint LonLatToUtm(double Longitude, double Latitude);

/** WGS84 longitude and latitude in degrees of a UTM zone 32N easting and northing (Krüger series to n^6). */
FLonLat UtmToLonLat(double East, double North);

/** World x (east) and y (south) of a WGS84 longitude and latitude in degrees. */
FWorldPoint LonLatToWorld(double Longitude, double Latitude);
}
