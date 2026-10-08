"""Area definition and coordinate conversion.

World frame (matches Unreal, metres here, centimetres in exported meshes):
  x = East  - origin East
  y = -(North - origin North)   (Unreal is left-handed: +Y points south when +X points east)
  z = height above NHN (DHHN2016), absolute — values are small (−3…95 m) so no z origin is needed.
Projected CRS for everything: ETRS89 / UTM 32N (EPSG:25832), same as the Hamburg open data.
"""
from dataclasses import dataclass

from pyproj import Transformer

_to_utm = Transformer.from_crs(4326, 25832, always_xy=True)
_to_wgs = Transformer.from_crs(25832, 4326, always_xy=True)


@dataclass(frozen=True)
class Area:
    name: str
    # World origin in UTM metres. Fixed for the whole project so tiles from different runs line up.
    origin_e: float
    origin_n: float
    # Extent to build, in world metres (x east, y south), half-open [min, max).
    x_min: float
    x_max: float
    y_min: float
    y_max: float
    tile_size: float = 250.0

    def utm_bounds(self, margin: float = 0.0):
        """(e_min, n_min, e_max, n_max) of the build extent plus margin."""
        return (
            self.origin_e + self.x_min - margin,
            self.origin_n - self.y_max - margin,
            self.origin_e + self.x_max + margin,
            self.origin_n - self.y_min + margin,
        )

    def wgs_bounds(self, margin: float = 0.0):
        e0, n0, e1, n1 = self.utm_bounds(margin)
        lon0, lat0 = _to_wgs.transform(e0, n0)
        lon1, lat1 = _to_wgs.transform(e1, n1)
        return lon0, lat0, lon1, lat1

    def tiles(self):
        """Yields (ix, iy, x0, y0, x1, y1) for every tile of the build extent."""
        iy = 0
        y = self.y_min
        while y < self.y_max:
            ix = 0
            x = self.x_min
            while x < self.x_max:
                yield ix, iy, x, y, min(x + self.tile_size, self.x_max), min(y + self.tile_size, self.y_max)
                x += self.tile_size
                ix += 1
            y += self.tile_size
            iy += 1


# Project-wide origin: Bergedorf, near the Schloss / town centre.
ORIGIN_LON, ORIGIN_LAT = 10.2120, 53.4880
ORIGIN_E, ORIGIN_N = (round(v) for v in _to_utm.transform(ORIGIN_LON, ORIGIN_LAT))

AREAS = {
    # First playable area: 2 x 2 km around the origin.
    "bergedorf_core": Area("bergedorf_core", ORIGIN_E, ORIGIN_N, -1000, 1000, -1000, 1000),
    # Small area for fast iteration.
    "bergedorf_test": Area("bergedorf_test", ORIGIN_E, ORIGIN_N, -250, 250, -250, 250),
}


def lonlat_to_world(lon, lat, area: Area):
    e, n = _to_utm.transform(lon, lat)
    return e - area.origin_e, -(n - area.origin_n)


def utm_to_world(e, n, area: Area):
    return e - area.origin_e, -(n - area.origin_n)
