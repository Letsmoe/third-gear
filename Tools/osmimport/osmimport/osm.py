"""Loads the OSM features we need for an area into plain Python structures in world coordinates."""
from dataclasses import dataclass, field

import numpy as np
import osmium
import shapely
from pyproj import Transformer

from .geo import Area

_to_utm = Transformer.from_crs(4326, 25832, always_xy=True)

DRIVABLE = {
    "motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary", "secondary_link",
    "tertiary", "tertiary_link", "unclassified", "residential", "living_street", "service", "road",
}
POINT_KEYS = {
    "highway": {"traffic_signals", "stop", "give_way", "crossing", "street_lamp", "mini_roundabout", "bus_stop"},
    "natural": {"tree"},
    "railway": {"level_crossing"},
    "traffic_sign": None,  # any value
}
AREA_KEYS = {
    "landuse": None, "natural": {"water", "wood", "scrub", "wetland", "grassland", "heath"},
    "leisure": {"park", "pitch", "playground", "garden"}, "amenity": {"parking"},
    "highway": {"pedestrian", "footway", "service"}, "area:highway": None, "place": {"square"},
    "waterway": {"riverbank", "dock"}, "water": None,
}


@dataclass
class Way:
    id: int
    tags: dict
    node_ids: list
    xy: np.ndarray  # (n, 2) world metres


@dataclass
class PointFeature:
    id: int
    tags: dict
    x: float
    y: float


@dataclass
class OsmData:
    roads: list = field(default_factory=list)
    footways: list = field(default_factory=list)
    railways: list = field(default_factory=list)
    waterways: list = field(default_factory=list)
    buildings: list = field(default_factory=list)  # (id, tags, shapely Polygon)
    areas: list = field(default_factory=list)  # (id, tags, shapely (Multi)Polygon)
    points: list = field(default_factory=list)
    tree_rows: list = field(default_factory=list)
    hedges: list = field(default_factory=list)


def _world_xy(lons, lats, area: Area):
    e, n = _to_utm.transform(np.asarray(lons), np.asarray(lats))
    return np.column_stack([e - area.origin_e, area.origin_n - n])


def _wanted_point(tags) -> bool:
    for key, values in POINT_KEYS.items():
        value = tags.get(key)
        if value is not None and (values is None or value in values):
            return True
    return False


def _wanted_area(tags) -> bool:
    if "building" in tags:
        return True
    for key, values in AREA_KEYS.items():
        if key == "highway" and tags.get("area") != "yes":
            continue  # closed highway ways are lines (loops) unless explicitly area=yes
        value = tags.get(key)
        if value is not None and (values is None or value in values):
            return True
    return False


def load(pbf_path: str, area: Area, margin: float = 150.0) -> OsmData:
    lon0, lat0, lon1, lat1 = area.wgs_bounds(margin)
    box = osmium.osm.Box(osmium.osm.Location(lon0, lat0), osmium.osm.Location(lon1, lat1))
    data = OsmData()

    def in_box(lon, lat):
        return lon0 <= lon <= lon1 and lat0 <= lat <= lat1

    processor = osmium.FileProcessor(pbf_path).with_locations().with_areas()
    for obj in processor:
        if obj.is_node():
            tags = dict(obj.tags)
            if tags and _wanted_point(tags) and obj.location.valid() and box.contains(obj.location):
                x, y = _world_xy([obj.location.lon], [obj.location.lat], area)[0]
                data.points.append(PointFeature(obj.id, tags, float(x), float(y)))
        elif obj.is_way():
            tags = dict(obj.tags)
            try:
                lons = [n.lon for n in obj.nodes]
                lats = [n.lat for n in obj.nodes]
            except osmium.InvalidLocationError:
                continue
            if len(lons) < 2 or not any(in_box(lo, la) for lo, la in zip(lons, lats)):
                continue
            way = Way(obj.id, tags, [n.ref for n in obj.nodes], _world_xy(lons, lats, area))
            highway = tags.get("highway")
            if highway in DRIVABLE and tags.get("area") != "yes":
                data.roads.append(way)
            elif highway in {"footway", "path", "cycleway", "pedestrian", "steps", "track", "bridleway"}:
                data.footways.append(way)
            elif "railway" in tags and tags["railway"] in {"rail", "light_rail", "subway", "tram"}:
                data.railways.append(way)
            elif "waterway" in tags:
                data.waterways.append(way)
            elif tags.get("natural") == "tree_row":
                data.tree_rows.append(way)
            elif tags.get("barrier") == "hedge":
                data.hedges.append(way)
        elif obj.is_area():
            tags = dict(obj.tags)
            if not _wanted_area(tags):
                continue
            polygons = []
            for outer in obj.outer_rings():
                shell = [(n.lon, n.lat) for n in outer]
                if not any(in_box(lo, la) for lo, la in shell):
                    continue
                holes = [[(n.lon, n.lat) for n in inner] for inner in obj.inner_rings(outer)]
                shell_xy = _world_xy(*zip(*shell), area)
                holes_xy = [_world_xy(*zip(*h), area) for h in holes if len(h) >= 4]
                poly = shapely.Polygon(shell_xy, holes_xy)
                if not poly.is_valid:
                    poly = shapely.make_valid(poly)
                if not poly.is_empty:
                    polygons.append(poly)
            if not polygons:
                continue
            geom = polygons[0] if len(polygons) == 1 else shapely.union_all(polygons)
            if "building" in tags:
                data.buildings.append((obj.orig_id(), tags, geom))
            else:
                data.areas.append((obj.orig_id(), tags, geom))
    return data
