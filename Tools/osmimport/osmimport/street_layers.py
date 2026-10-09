"""The street features we derive from OSM, grouped into map layers and converted to GeoJSON for the street viewer.

Geometry is built in world metres (x east, y south, see geo.py) and only converted to longitude and latitude at the end.
Every feature carries a `style` the viewer page colours by and the OSM tags shown in its pop-up.
"""
import shapely
from pyproj import Transformer

from . import roads
from .geo import Area
from .osm import OsmData

CYCLE_LANE_INSET = 0.8
CYCLE_TRACK_OFFSET = 1.5
BRIDGE_MARGIN = 1.0
RAIL_TRACK_WIDTH = 3.0
PATH_BRIDGE_WIDTH = 2.0
POINT_KINDS = {"traffic_signals", "stop", "give_way", "crossing", "street_lamp", "bus_stop", "mini_roundabout"}

_utm_to_wgs = Transformer.from_crs(25832, 4326, always_xy=True)


class Layers:
    """Features by layer name, each a (world geometry, style, properties) tuple."""

    def __init__(self):
        self.by_name = {}

    def add(self, layer: str, geometry, style: str, properties: dict):
        """Adds one feature to a layer; empty geometry is skipped."""
        if geometry is None or geometry.is_empty:
            return
        self.by_name.setdefault(layer, []).append((geometry, style, properties))


def side_offset(line: shapely.LineString, distance: float, side: str) -> shapely.LineString:
    """The line moved sideways by distance metres to the physical left or right of its direction.

    World y points south, so the frame is mirrored and shapely's positive (left) offset lands on the physical right.
    """
    if side == "left":
        return shapely.offset_curve(line, -distance)
    return shapely.offset_curve(line, distance)


def is_bridge(tags: dict) -> bool:
    """True for ways that cross on a bridge (any bridge= value except no)."""
    return tags.get("bridge", "no") != "no"


def cycleway_sides(tags: dict) -> dict:
    """The cycleway tagged on a road, as {"left": value, "right": value} relative to the way's direction."""
    sides = {}
    both = tags.get("cycleway:both") or tags.get("cycleway")
    if both:
        sides = {"left": both, "right": both}
        if tags.get("cycleway") and roads.is_oneway(tags) and "cycleway:both" not in tags:
            sides = {"right": both}  # a plain cycleway= on a one-way street is on the right
    for side in ("left", "right"):
        value = tags.get(f"cycleway:{side}")
        if value:
            sides[side] = value
    return sides


def lane_divider_offsets(tags: dict, width: float) -> list:
    """Sideways offsets (positive = physical right) of the lines between lanes, from the lane tags."""
    lane_count = roads.road_lanes(tags)
    if lane_count < 2:
        return []
    lane_width = width / lane_count
    return [-width / 2 + lane_width * index for index in range(1, lane_count)]


def _properties(kind: str, osm_id: int, tags: dict, **extra) -> dict:
    """Pop-up properties: what the feature is, its OSM id and tags, and values we derived."""
    return {"kind": kind, "osm_id": osm_id, "tags": tags, "derived": extra}


def _bridge_band(layers: Layers, line, width: float, properties: dict):
    """The deck outline of a way crossing on a bridge."""
    layers.add("Bridges", shapely.buffer(line, width / 2 + BRIDGE_MARGIN, cap_style="flat"), "bridge", properties)


def add_road(layers: Layers, way):
    """Carriageway edges, centre line, lane dividers and the cycleways tagged on one road."""
    centre = shapely.LineString(way.xy)
    width = roads.road_width(way.tags)
    lane_count = roads.road_lanes(way.tags)
    properties = _properties("road", way.id, way.tags, width_m=round(width, 2), lanes=lane_count,
                             oneway=roads.is_oneway(way.tags))
    layers.add("Roads", centre, "centre", properties)
    for side in ("left", "right"):
        layers.add("Roads", side_offset(centre, width / 2, side), "carriageway_edge", properties)
    for offset in lane_divider_offsets(way.tags, width):
        side = "right" if offset > 0 else "left"
        layers.add("Lanes", side_offset(centre, abs(offset), side), "lane_divider", properties)
    if is_bridge(way.tags):
        _bridge_band(layers, centre, width, properties)
    for side, value in cycleway_sides(way.tags).items():
        _add_road_cycleway(layers, centre, width, side, value, properties)


def _add_road_cycleway(layers: Layers, centre, width: float, side: str, value: str, properties: dict):
    """A cycle lane inside the carriageway edge, or a track outside it, on one side of a road."""
    if value in {"lane", "opposite_lane"}:
        layers.add("Cycling", side_offset(centre, width / 2 - CYCLE_LANE_INSET, side), "cycle_lane", properties)
    elif value in {"track", "opposite_track"}:
        layers.add("Cycling", side_offset(centre, width / 2 + CYCLE_TRACK_OFFSET, side), "cycle_track", properties)
    elif value in {"shared_lane", "share_busway"}:
        layers.add("Cycling", side_offset(centre, width / 2 - CYCLE_LANE_INSET, side), "shared_lane", properties)


def add_path(layers: Layers, way):
    """A separately mapped cycleway, footway, crossing or path."""
    line = shapely.LineString(way.xy)
    tags = way.tags
    properties = _properties("path", way.id, tags)
    if is_bridge(tags):
        _bridge_band(layers, line, PATH_BRIDGE_WIDTH, properties)
    if tags.get("footway") == "crossing" or tags.get("cycleway") == "crossing":
        layers.add("Crossings", line, "crossing_way", properties)
    elif tags.get("highway") == "cycleway" or tags.get("bicycle") == "designated":
        layers.add("Cycling", line, "cycleway", properties)
    else:
        layers.add("Footways", line, "footway", properties)


def add_railway(layers: Layers, way):
    """A rail line, with its bridge deck where it crosses one."""
    line = shapely.LineString(way.xy)
    properties = _properties("railway", way.id, way.tags)
    if is_bridge(way.tags):
        _bridge_band(layers, line, RAIL_TRACK_WIDTH, properties)
    layers.add("Railways", line, "railway", properties)


def add_point(layers: Layers, point):
    """A signal, crossing, stop or give way sign, lamp, bus stop or traffic sign node."""
    tags = point.tags
    geometry = shapely.Point(point.x, point.y)
    kind = tags.get("highway")
    properties = _properties("node", point.id, tags)
    if "traffic_sign" in tags:
        properties["label"] = tags["traffic_sign"].replace("DE:", "")
        layers.add("Signals and signs", geometry, "traffic_sign", properties)
    elif kind in {"traffic_signals", "stop", "give_way", "mini_roundabout"}:
        layers.add("Signals and signs", geometry, kind, properties)
    elif kind == "crossing":
        layers.add("Crossings", geometry, "crossing", properties)
    elif kind in {"street_lamp", "bus_stop"}:
        layers.add("Lamps and bus stops", geometry, kind, properties)


def add_road_area(layers: Layers, osm_id: int, tags: dict, geometry):
    """A mapped road area (area:highway), the exact carriageway or path outline where mappers drew it."""
    layers.add("Road areas", geometry, "road_area", _properties("area", osm_id, tags))


def build(data: OsmData) -> Layers:
    """All street layers for the loaded OSM data, as OSM tags them."""
    layers = Layers()
    for osm_id, tags, geometry in data.areas:
        if "area:highway" in tags:
            add_road_area(layers, osm_id, tags, geometry)
    for way in data.footways:
        add_path(layers, way)
    for way in data.railways:
        add_railway(layers, way)
    for way in data.roads:
        add_road(layers, way)
    for point in data.points:
        add_point(layers, point)
    return layers


def _world_to_lonlat(area: Area, geometry):
    """The geometry converted from world metres to WGS84 longitude and latitude."""
    def convert(coordinates):
        east = coordinates[:, 0] + area.origin_e
        north = area.origin_n - coordinates[:, 1]
        longitude, latitude = _utm_to_wgs.transform(east, north)
        coordinates[:, 0] = longitude
        coordinates[:, 1] = latitude
        return coordinates
    return shapely.transform(geometry, convert)


def to_geojson(layers: Layers, area: Area) -> dict:
    """{layer name: GeoJSON FeatureCollection} in longitude and latitude."""
    result = {}
    for name, features in layers.by_name.items():
        collection = []
        for geometry, style, properties in features:
            lonlat = _world_to_lonlat(area, geometry)
            collection.append({"type": "Feature", "geometry": shapely.geometry.mapping(lonlat),
                               "properties": properties | {"style": style}})
        result[name] = {"type": "FeatureCollection", "features": collection}
    return result


def region_outline(area: Area) -> dict:
    """The region's build extent as a GeoJSON polygon in longitude and latitude."""
    box = shapely.box(area.x_min, area.y_min, area.x_max, area.y_max)
    return shapely.geometry.mapping(_world_to_lonlat(area, box))
