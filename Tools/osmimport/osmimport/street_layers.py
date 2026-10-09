"""The street features we derive from OSM, grouped into map layers and converted to GeoJSON for the street viewer.

Geometry is built in world metres (x east, y south, see geo.py) and only converted to longitude and latitude at the end.
Every feature carries a `style` the viewer page colours by and the OSM tags shown in its pop-up.
"""
import shapely
from pyproj import Transformer

from . import building_types, roads
from .streets import tags as osm_tags
from .streets.cross_section import StripKind
from .geo import Area
from .osm import OsmData

CYCLE_LANE_INSET = 0.8
CYCLE_TRACK_OFFSET = 1.5
BRIDGE_MARGIN = 1.0
RAIL_TRACK_WIDTH = 3.0
PATH_BRIDGE_WIDTH = 2.0
POINT_KINDS = {"traffic_signals", "stop", "give_way", "crossing", "street_lamp", "bus_stop", "mini_roundabout"}

# Fill colour of each building class in the viewer, grouped by kind: old town warm, postwar grey and blue, houses
# green, non-residential violet and brown.
BUILDING_CLASS_COLOURS = {
    "gruenderzeit_clinker": "#c0392b", "brick_block_1920s": "#e67e22", "halftimbered_town": "#f1c40f",
    "postwar_plaster": "#95a5a6", "slab_block": "#34495e", "modern": "#3498db",
    "terraced": "#27ae60", "semidetached": "#2ecc71", "detached_postwar": "#a3e635", "villa": "#16a085",
    "vierlande_farmhouse": "#8d6e63", "commercial_groundfloor": "#e84393", "retail_centre": "#9b59b6",
    "public": "#6c5ce7", "industrial_hall": "#5d4037", "shed_garage": "#bdbdbd",
}
BUILDING_FLAGS = {
    building_types.FLAG_ATTIC_HABITABLE: "habitable attic", building_types.FLAG_SHOP_GROUND_FLOOR: "shop ground floor",
    building_types.FLAG_CLOSED_LEFT: "party wall left", building_types.FLAG_CLOSED_RIGHT: "party wall right",
    building_types.FLAG_CORNER: "street corner", building_types.FLAG_COMPLEX_FOOTPRINT: "complex footprint",
    building_types.FLAG_BARN: "barn", building_types.FLAG_TOWER: "tower block",
}
BUILDING_TAG_BITS = {
    building_types.TAG_CLASS: "class", building_types.TAG_LEVELS: "levels", building_types.TAG_ROOF_SHAPE: "roof shape",
    building_types.TAG_HEIGHT: "height", building_types.TAG_START_DATE: "start date",
    building_types.TAG_ROOF_LEVELS: "roof levels",
}

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
    if abs(distance) < 0.01:
        return line  # GEOS fails on offsets of about 1e-16, which the strip sums give for the centre line
    if side == "left":
        return shapely.offset_curve(line, -distance)
    return shapely.offset_curve(line, distance)


def is_bridge(tags: dict) -> bool:
    """True for ways that cross on a bridge (any bridge= value except no)."""
    return tags.get("bridge", "no") != "no"


def cycleway_sides(tags: dict) -> dict:
    """The cycleway tagged on a road, as {"left": value, "right": value} relative to the way's direction."""
    return osm_tags.cycleway_sides(tags)


def cycle_lane_offset(section, side: str) -> float:
    """Distance from the centre line to the middle of the painted cycle lane on one side, or to where one would run
    along the kerb when the cross-section has none (shared lanes)."""
    offset = section.side_strip_centre(StripKind.CYCLE_LANE, side)
    if offset is None:
        return section.width() / 2 - CYCLE_LANE_INSET
    return offset


def _properties(kind: str, osm_id: int, tags: dict, **extra) -> dict:
    """Pop-up properties: what the feature is, its OSM id and tags, and values we derived."""
    return {"kind": kind, "osm_id": osm_id, "tags": tags, "derived": extra}


def _bridge_band(layers: Layers, line, width: float, properties: dict):
    """The deck outline of a way crossing on a bridge."""
    layers.add("Bridges", shapely.buffer(line, width / 2 + BRIDGE_MARGIN, cap_style="flat"), "bridge", properties)


def add_road(layers: Layers, way):
    """Carriageway edges, centre line, lane dividers and the cycleways tagged on one road."""
    centre = shapely.LineString(way.xy)
    section = roads.road_section(way.tags)
    width = section.width()
    properties = _properties("road", way.id, way.tags, width_m=round(width, 2), lanes=section.lane_count(),
                             oneway=osm_tags.is_oneway(way.tags), width_from=section.width_source.value,
                             cross_section=section.describe())
    layers.add("Roads", centre, "centre", properties)
    for side in ("left", "right"):
        layers.add("Roads", side_offset(centre, width / 2, side), "carriageway_edge", properties)
    for offset in section.lane_dividers():
        side = "right" if offset > 0 else "left"
        layers.add("Lanes", side_offset(centre, abs(offset), side), "lane_divider", properties)
    if is_bridge(way.tags):
        _bridge_band(layers, centre, width, properties)
    for side, value in cycleway_sides(way.tags).items():
        _add_road_cycleway(layers, centre, section, side, value, properties)


def _add_road_cycleway(layers: Layers, centre, section, side: str, value: str, properties: dict):
    """A cycle lane inside the carriageway edge, or a track outside it, on one side of a road."""
    if value in {"lane", "opposite_lane"}:
        layers.add("Cycling", side_offset(centre, cycle_lane_offset(section, side), side), "cycle_lane", properties)
    elif value in {"track", "opposite_track"}:
        layers.add("Cycling", side_offset(centre, section.width() / 2 + CYCLE_TRACK_OFFSET, side), "cycle_track",
                   properties)
    elif value in {"shared_lane", "share_busway"}:
        layers.add("Cycling", side_offset(centre, cycle_lane_offset(section, side), side), "shared_lane", properties)


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


WATER_LANDUSE = {"reservoir", "basin"}
# Typical waterway widths in metres when OSM has no width tag.
WATERWAY_WIDTHS = {"river": 15.0, "canal": 8.0, "stream": 2.0, "ditch": 1.2, "drain": 1.2}


def is_water_area(tags: dict) -> bool:
    """True for mapped water surfaces: lakes, ponds, river and harbour areas, reservoirs."""
    return (tags.get("natural") == "water" or "water" in tags or tags.get("waterway") in {"riverbank", "dock"}
            or tags.get("landuse") in WATER_LANDUSE)


def waterway_width(tags: dict) -> float:
    """Width of a waterway line in metres: the width tag, else a typical width for its kind."""
    width = osm_tags.number(tags.get("width"))
    if width and 0.3 <= width <= 200.0:
        return width
    return WATERWAY_WIDTHS.get(tags.get("waterway"), 1.0)


def add_water_area(layers: Layers, osm_id: int, tags: dict, geometry):
    """A mapped water surface."""
    layers.add("Water", geometry, "water_area", _properties("water", osm_id, tags))


def add_waterway(layers: Layers, way):
    """A river, canal, stream or ditch centre line, with the width we assume for it."""
    properties = _properties("waterway", way.id, way.tags, width_m=waterway_width(way.tags))
    layers.add("Water", shapely.LineString(way.xy), "waterway", properties)


def add_road_area(layers: Layers, osm_id: int, tags: dict, geometry):
    """A mapped road area (area:highway), the exact carriageway or path outline where mappers drew it."""
    layers.add("Road areas", geometry, "road_area", _properties("area", osm_id, tags))


SURVEY_AREA_STYLES = {"carriageway": "survey_carriageway", "parking": "survey_parking", "cycle": "survey_cycle",
                      "footway": "survey_footway", "green": "survey_green", "shared": "survey_shared"}


def add_survey(layers: Layers, survey, box):
    """Hamburg's surveyed street space in the box (hh_survey.Survey): areas by use, kerbs, lamps, signal poles and
    signs, as reference layers to compare our streets with."""
    for group, usage, surface, polygon in survey.areas_in(box):
        properties = {"kind": f"survey: {usage}", "osm_id": "", "tags": {}, "derived": {"surface": surface}}
        layers.add("Survey: street areas", polygon, SURVEY_AREA_STYLES[group], properties)
    for kerb in survey.kerbs_in(box):
        layers.add("Survey: kerbs", kerb, "survey_kerb", {"kind": "survey: kerb", "osm_id": "", "tags": {},
                                                          "derived": {}})
    for kind, x, y in survey.points_in(box, {"lamp", "signal"}):
        layers.add("Survey: lamps and signals", shapely.Point(x, y), f"survey_{kind}",
                   {"kind": f"survey: {kind}", "osm_id": "", "tags": {}, "derived": {}})
    for number, x, y, azimuth, pole in survey.signs_in(box):
        layers.add("Survey: signs", shapely.Point(x, y), "survey_sign",
                   {"kind": f"survey: sign {number}", "osm_id": "", "tags": {},
                    "derived": {"facing_azimuth_deg": azimuth, "pole": pole}, "label": number})


def _bit_names(value: int, names: dict) -> str:
    """The names of the bits set in value, comma separated, or "none"."""
    set_names = [name for bit, name in names.items() if value & bit]
    if not set_names:
        return "none"
    return ", ".join(set_names)


def add_building(layers: Layers, osm_id: int, tags: dict, footprint, building_type):
    """A building footprint, coloured by the class building_types.py gives it, with the whole typing in its pop-up."""
    class_name = building_types.CLASS_NAMES[building_type.class_id]
    properties = _properties(
        "building", osm_id, tags, building_class=class_name,
        roof=f"{building_types.ROOF_NAMES[building_type.roof_shape]}, {building_type.pitch_degrees:.0f}°",
        storeys=building_type.storeys, attic_levels=building_type.attic_levels,
        eave_height_m=round(building_type.eave_height, 1), storey_height_m=round(building_type.storey_height, 2),
        footprint_m2=round(footprint.area), features=_bit_names(building_type.flags, BUILDING_FLAGS),
        from_tags=_bit_names(building_type.tag_bits, BUILDING_TAG_BITS))
    properties["fill"] = BUILDING_CLASS_COLOURS[class_name]
    layers.add("Buildings", footprint, "building", properties)


def build(data: OsmData, buildings=()) -> Layers:
    """All street layers for the loaded OSM data, as OSM tags them, plus the given typed buildings
    ((osm_id, tags, footprint, BuildingType) tuples)."""
    layers = Layers()
    for osm_id, tags, footprint, building_type in buildings:
        add_building(layers, osm_id, tags, footprint, building_type)
    for osm_id, tags, geometry in data.areas:
        if "area:highway" in tags:
            add_road_area(layers, osm_id, tags, geometry)
        elif is_water_area(tags):
            add_water_area(layers, osm_id, tags, geometry)
    for way in data.waterways:
        if way.tags.get("waterway") in WATERWAY_WIDTHS:
            add_waterway(layers, way)
    for way in data.footways:
        add_path(layers, way)
    for way in data.railways:
        add_railway(layers, way)
    for way in data.roads:
        add_road(layers, way)
    for point in data.points:
        add_point(layers, point)
    return layers


def world_to_lonlat(area: Area, geometry):
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
            lonlat = world_to_lonlat(area, geometry)
            collection.append({"type": "Feature", "geometry": shapely.geometry.mapping(lonlat),
                               "properties": properties | {"style": style}})
        result[name] = {"type": "FeatureCollection", "features": collection}
    return result


def region_outline(area: Area) -> dict:
    """The region's build extent as a GeoJSON polygon in longitude and latitude."""
    box = shapely.box(area.x_min, area.y_min, area.x_max, area.y_max)
    return shapely.geometry.mapping(world_to_lonlat(area, box))
