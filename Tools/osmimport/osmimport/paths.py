"""Water bodies and non-road paved/unpaved surfaces (footpaths, pedestrian zones, squares, car parks)."""
from dataclasses import dataclass

import numpy as np
import shapely

from .dem import HeightGrid
from .osm import OsmData

PAVED = {"paved", "asphalt", "paving_stones", "sett", "concrete", "concrete:plates", "cobblestone", "bricks",
         "paving_stones:30", "metal", "wood"}
FOOT_WIDTHS = {"pedestrian": 8.0, "footway": 2.0, "cycleway": 2.0, "path": 1.5, "bridleway": 2.5, "track": 3.0}
RIVER_WIDTHS = {"river": 12.0, "canal": 8.0, "stream": 2.0}


def _float(value):
    try:
        return float(str(value).replace(",", ".").replace("m", "").strip())
    except (TypeError, ValueError):
        return None


@dataclass
class WaterBody:
    polygon: shapely.Geometry
    level: float


@dataclass
class Surfaces:
    paved: shapely.Geometry
    unpaved: shapely.Geometry
    water: list  # WaterBody


def build(osm: OsmData, dem: HeightGrid, road_ground, pavement, buildings_union) -> Surfaces:
    blocked = shapely.union_all([g for g in (road_ground, pavement, buildings_union) if g is not None])

    paved, unpaved = [], []
    for w in osm.footways:
        t = w.tags
        hw = t.get("highway")
        if hw == "steps" or t.get("footway") in {"sidewalk", "crossing"} or t.get("tunnel") not in (None, "no"):
            continue
        if t.get("access") in {"private", "no"} and hw == "track":
            continue
        width = _float(t.get("width")) or FOOT_WIDTHS.get(hw, 1.5)
        geom = shapely.buffer(shapely.LineString(w.xy), width / 2, cap_style="round", quad_segs=3)
        surface = t.get("surface")
        is_paved = surface in PAVED if surface else hw in {"pedestrian", "footway", "cycleway"}
        (paved if is_paved else unpaved).append(geom)
    for _, t, geom in osm.areas:
        if t.get("highway") in {"pedestrian", "footway", "service"} or "area:highway" in t \
                or t.get("place") == "square" or t.get("amenity") == "parking":
            if t.get("parking") in {"underground", "multi-storey", "rooftop"}:
                continue
            surface = t.get("surface")
            (unpaved if surface and surface not in PAVED else paved).append(geom)

    paved_u = shapely.union_all(paved).difference(blocked) if paved else shapely.Polygon()
    unpaved_u = shapely.union_all(unpaved).difference(blocked).difference(paved_u) if unpaved else shapely.Polygon()

    water = _water_bodies(osm, dem, blocked)
    water_union = shapely.union_all([wb.polygon for wb in water]) if water else shapely.Polygon()
    return Surfaces(paved=paved_u.difference(water_union), unpaved=unpaved_u.difference(water_union), water=water)


def _water_bodies(osm: OsmData, dem: HeightGrid, blocked):
    polys = []
    for _, t, geom in osm.areas:
        if t.get("natural") == "water" or t.get("waterway") in {"riverbank", "dock"} or "water" in t:
            polys.append(geom)
    for w in osm.waterways:
        kind = w.tags.get("waterway")
        if kind in RIVER_WIDTHS and w.tags.get("tunnel") in (None, "no"):
            width = _float(w.tags.get("width")) or RIVER_WIDTHS[kind]
            if kind == "stream" and width < 3:
                continue  # ditches/streams: leave as terrain dips for now
            polys.append(shapely.buffer(shapely.LineString(w.xy), width / 2, cap_style="flat"))
    if not polys:
        return []
    h, w = dem.z.shape
    extent = shapely.box(dem.x0, dem.y0, dem.x0 + w * dem.res, dem.y0 + h * dem.res)
    merged = shapely.union_all(polys).intersection(extent)  # e.g. the Elbe polygon is tens of km long
    bodies = []
    for poly in getattr(merged, "geoms", [merged]):
        if not isinstance(poly, shapely.Polygon) or poly.area < 20:
            continue
        bodies.append(WaterBody(polygon=poly, level=_water_level(poly, dem)))
    return bodies


def _water_level(poly, dem: HeightGrid):
    """Laser scans return few, low points over water; the DGM interpolates a fairly flat surface there.
    Use a low percentile of interior samples (banks excluded)."""
    inner = poly.buffer(-2.0)
    if inner.is_empty:
        inner = poly
    minx, miny, maxx, maxy = inner.bounds
    xs = np.arange(minx, maxx, 2.0)
    ys = np.arange(miny, maxy, 2.0)
    if len(xs) == 0 or len(ys) == 0:
        c = inner.representative_point()
        return float(dem.sample(c.x, c.y))
    gx, gy = np.meshgrid(xs, ys)
    pts = shapely.points(gx.ravel(), gy.ravel())
    inside = shapely.contains(inner, pts)
    if not inside.any():
        c = inner.representative_point()
        return float(dem.sample(c.x, c.y))
    z = dem.sample(gx.ravel()[inside], gy.ravel()[inside])
    return float(np.percentile(z, 25))
