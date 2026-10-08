"""Road network: widths, surface polygons (with junction shapes), smooth surface heights, pavements and markings."""
from collections import Counter, defaultdict
from dataclasses import dataclass, field

import numpy as np
import shapely
from rasterio import features as rfeatures
from rasterio.transform import from_origin
from scipy import ndimage

from .dem import HeightGrid
from .osm import OsmData, Way

# Typical German carriageway widths (m) when OSM has no width tag. (per lane, default lanes two-way, one-way)
CLASS_DEFAULTS = {
    "motorway": (3.75, 2, 2), "motorway_link": (3.5, 1, 1),
    "trunk": (3.5, 2, 2), "trunk_link": (3.5, 1, 1),
    "primary": (3.25, 2, 2), "primary_link": (3.25, 1, 1),
    "secondary": (3.25, 2, 1), "secondary_link": (3.25, 1, 1),
    "tertiary": (3.0, 2, 1), "tertiary_link": (3.0, 1, 1),
    "unclassified": (2.75, 2, 1), "residential": (2.75, 2, 1), "road": (2.75, 2, 1),
    "living_street": (2.5, 2, 1), "service": (3.0, 1, 1),
}
# Higher rank wins where surfaces overlap (junction area belongs to the major road).
CLASS_RANK = {k: i for i, k in enumerate([
    "service", "living_street", "road", "residential", "unclassified", "tertiary_link", "tertiary", "secondary_link",
    "secondary", "primary_link", "primary", "trunk_link", "trunk", "motorway_link", "motorway"])}
PAVEMENT_CLASSES = {"primary", "secondary", "tertiary", "residential", "unclassified", "living_street", "primary_link",
                    "secondary_link", "tertiary_link"}
MARKED_CLASSES = {"motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary",
                  "secondary_link", "tertiary", "tertiary_link"}
COBBLE_SURFACES = {"sett", "cobblestone", "unhewn_cobblestone", "cobblestone:flattened"}
PAVER_SURFACES = {"paving_stones", "paving_stones:30", "concrete:plates", "grass_paver"}

KERB_HEIGHT = 0.12
PAVEMENT_WIDTH = 2.5
FILLET_RADIUS = 4.0
MARKING_LIFT = 0.008


def _float(value, default=None):
    if value is None:
        return default
    try:
        return float(str(value).split(";")[0].replace(",", ".").replace("m", "").strip())
    except ValueError:
        return default


def is_oneway(tags) -> bool:
    return tags.get("oneway") in {"yes", "1", "true", "-1"} or tags.get("junction") in {"roundabout", "circular"} \
        or tags.get("highway") in {"motorway", "motorway_link"}


def road_width(tags) -> float:
    width = _float(tags.get("width"))
    if width and 2.0 <= width <= 30.0:
        return width
    lane_width, lanes_two_way, lanes_one_way = CLASS_DEFAULTS.get(tags.get("highway"), (2.75, 2, 1))
    oneway = is_oneway(tags)
    lanes = _float(tags.get("lanes"))
    if not lanes:
        lanes = lanes_one_way if oneway else lanes_two_way
    width = lanes * lane_width
    if tags.get("highway") == "service" and tags.get("service") in {"driveway", "parking_aisle"}:
        width = 3.0
    if oneway and lanes == 1:
        width = max(width, 3.5)
    return width


def road_lanes(tags) -> int:
    lanes = _float(tags.get("lanes"))
    if lanes:
        return int(lanes)
    _, two_way, one_way = CLASS_DEFAULTS.get(tags.get("highway"), (2.75, 2, 1))
    return one_way if is_oneway(tags) else two_way


def surface_kind(tags) -> str:
    surface = tags.get("surface", "asphalt")
    if surface in COBBLE_SURFACES:
        return "cobble"
    if surface in PAVER_SURFACES:
        return "pavers"
    return "asphalt"


@dataclass
class RoadNetwork:
    ways: list
    widths: dict
    surfaces: dict  # kind -> polygon (ground level, disjoint)
    ground: shapely.Geometry  # union of all ground road surfaces
    bridges: list  # (way, polygon)
    pavement: shapely.Geometry
    height: HeightGrid  # smooth road surface height (valid on/near roads)
    junction_zones: shapely.Geometry
    markings: list = field(default_factory=list)  # (kind, LineString with z via height) — see build_markings


def _is_ground(way: Way) -> bool:
    t = way.tags
    return t.get("bridge") in (None, "no") and _float(t.get("layer"), 0) >= 0 and t.get("tunnel") in (None, "no", "building_passage")


def _is_tunnel(way: Way) -> bool:
    return way.tags.get("tunnel") not in (None, "no", "building_passage") or _float(way.tags.get("layer"), 0) < 0


def node_degrees(ways) -> Counter:
    """How many way-ends/passes touch each node; >= 3 means a junction."""
    degree = Counter()
    for way in ways:
        ids = way.node_ids
        for i, node in enumerate(ids):
            degree[node] += 1 if i in (0, len(ids) - 1) else 2
    return degree


def build(osm: OsmData, dem: HeightGrid, buildings_union=None) -> RoadNetwork:
    ways = [w for w in osm.roads if not _is_tunnel(w) and len(w.xy) >= 2]
    widths = {w.id: road_width(w.tags) for w in ways}
    ground_ways = [w for w in ways if _is_ground(w)]
    bridge_ways = [w for w in ways if not _is_ground(w)]

    # --- surface polygons per kind, major road classes win overlaps ---
    strips = {}
    for w in ground_ways:
        strips[w.id] = shapely.buffer(shapely.LineString(w.xy), widths[w.id] / 2, cap_style="round", join_style="round",
                                      quad_segs=4)
    ground = shapely.union_all(list(strips.values()))
    # Fillet concave corners (kerb radii at junctions) without growing the outline elsewhere.
    ground = ground.buffer(FILLET_RADIUS, quad_segs=4).buffer(-FILLET_RADIUS, quad_segs=4).union(ground)
    ground = shapely.make_valid(ground)

    by_kind = defaultdict(list)
    for w in sorted(ground_ways, key=lambda w: CLASS_RANK.get(w.tags.get("highway"), 0), reverse=True):
        by_kind[surface_kind(w.tags)].append(strips[w.id])
    surfaces = {}
    claimed = shapely.Polygon()
    # asphalt first so it wins at junctions with cobbled side streets
    for kind in ("asphalt", "pavers", "cobble"):
        if kind not in by_kind:
            continue
        poly = shapely.union_all(by_kind[kind]).difference(claimed)
        surfaces[kind] = poly
        claimed = claimed.union(poly)
    # fillet areas (and anything not claimed) become asphalt
    rest = ground.difference(claimed)
    surfaces["asphalt"] = shapely.union_all([surfaces.get("asphalt", shapely.Polygon()), rest])

    # --- smooth road surface height from the DEM ---
    height = _road_height_field(dem, ground)

    # --- bridges: straight ramps between the ground heights at both ends ---
    bridges = []
    for w in bridge_ways:
        poly = shapely.buffer(shapely.LineString(w.xy), widths[w.id] / 2, cap_style="flat", join_style="round")
        bridges.append((w, poly))

    # --- pavements along urban roads ---
    pavement = _pavements(ground_ways, widths, ground, buildings_union, osm)

    degree = node_degrees(ground_ways)
    node_xy = {}
    for w in ground_ways:
        for nid, xy in zip(w.node_ids, w.xy):
            node_xy[nid] = xy
    zones = []
    for nid, deg in degree.items():
        if deg >= 3:
            r = max((widths[w.id] for w in ground_ways if nid in w.node_ids), default=6.0) * 0.6 + 3.0
            zones.append(shapely.Point(node_xy[nid]).buffer(r, quad_segs=6))
    junction_zones = shapely.union_all(zones) if zones else shapely.Polygon()

    net = RoadNetwork(ways=ways, widths=widths, surfaces=surfaces, ground=ground, bridges=bridges, pavement=pavement,
                      height=height, junction_zones=junction_zones)
    net.markings = build_markings(ground_ways, widths, junction_zones, ground)
    return net


def _road_height_field(dem: HeightGrid, ground) -> HeightGrid:
    """Normalised Gaussian smoothing of DEM heights inside the road mask, extended outward by nearest value."""
    h, w = dem.z.shape
    transform = from_origin(dem.x0, -dem.y0, dem.res, dem.res)  # rasterio wants y up; we flip y below
    # Rasterize in a y-flipped frame: world y grows southward = rows grow downward, so use (x, -y).
    flipped = shapely.transform(ground, lambda c: c * np.array([1.0, -1.0]))
    mask = rfeatures.rasterize([(flipped, 1)], out_shape=(h, w), transform=transform, fill=0, dtype="uint8",
                               all_touched=False).astype(bool)
    z = dem.z.astype(np.float64)
    sigma = 4.0 / dem.res
    weight = ndimage.gaussian_filter(mask.astype(np.float64), sigma)
    smooth = ndimage.gaussian_filter(np.where(mask, z, 0.0), sigma)
    with np.errstate(invalid="ignore", divide="ignore"):
        smooth = np.where(weight > 1e-3, smooth / weight, np.nan)
    # Extend outward from road cells so samples just outside the polygon edge stay sane.
    road_or_near = mask | (weight > 0.05)
    invalid = ~road_or_near | np.isnan(smooth)
    if invalid.all():
        return HeightGrid(z=dem.z.copy(), x0=dem.x0, y0=dem.y0, res=dem.res)
    _, (ri, ci) = ndimage.distance_transform_edt(invalid, return_indices=True)
    extended = smooth[ri, ci]
    return HeightGrid(z=extended.astype(np.float32), x0=dem.x0, y0=dem.y0, res=dem.res)


def _is_urban(way: Way, buildings_union) -> bool:
    if buildings_union is None:
        return True
    speed = _float(way.tags.get("maxspeed"))
    if speed and speed >= 70:
        return False
    return shapely.dwithin(buildings_union, shapely.LineString(way.xy), 35.0)


def _pavements(ground_ways, widths, ground, buildings_union, osm: OsmData):
    bands = []
    for w in ground_ways:
        t = w.tags
        if t.get("highway") not in PAVEMENT_CLASSES:
            continue
        sidewalk = t.get("sidewalk", t.get("sidewalk:both"))
        if sidewalk in {"no", "none"}:
            continue
        if not _is_urban(w, buildings_union):
            continue
        line = shapely.LineString(w.xy)
        sides = {"left": 1, "right": -1}
        if sidewalk in {"left", "right"}:
            offset = sides[sidewalk] * (widths[w.id] / 2 + PAVEMENT_WIDTH / 2)
            band = shapely.buffer(shapely.offset_curve(line, offset), PAVEMENT_WIDTH / 2 + 0.3, cap_style="flat")
        else:
            band = shapely.buffer(line, widths[w.id] / 2 + PAVEMENT_WIDTH, cap_style="flat", join_style="round")
        bands.append(band)
    if not bands:
        return shapely.Polygon()
    pavement = shapely.union_all(bands).difference(ground)
    if buildings_union is not None:
        pavement = pavement.difference(buildings_union.buffer(0.2))
    # Drop slivers.
    pavement = shapely.make_valid(pavement.buffer(-0.3).buffer(0.3))
    return pavement


def build_markings(ground_ways, widths, junction_zones, ground):
    """Returns a list of (kind, LineString) in world xy. kind: 'dash_urban', 'dash_rural', 'solid', 'edge'."""
    markings = []
    keep_out = junction_zones
    for w in ground_ways:
        t = w.tags
        hw = t.get("highway")
        if hw not in MARKED_CLASSES or t.get("lane_markings") == "no" or t.get("area") == "yes":
            continue
        width = widths[w.id]
        lanes = road_lanes(t)
        line = shapely.LineString(w.xy)
        if line.length < 8:
            continue
        speed = _float(t.get("maxspeed"), 50)
        rural = speed >= 70
        dash = "dash_rural" if rural else "dash_urban"
        oneway = is_oneway(t)
        lines = []
        if not oneway and lanes >= 2 and width >= 5.0:
            # centre line; multi-lane two-way roads get lane lines on each side as well
            lines.append((dash, line))
            per_side = lanes // 2
            lane_w = width / max(lanes, 1)
            for i in range(1, per_side):
                for side in (1, -1):
                    lines.append((dash, shapely.offset_curve(line, side * i * lane_w)))
        elif oneway and lanes >= 2:
            lane_w = width / lanes
            for i in range(1, lanes):
                lines.append((dash, shapely.offset_curve(line, -width / 2 + i * lane_w)))
        if hw in {"motorway", "trunk", "primary", "motorway_link", "trunk_link"} or (rural and width >= 5.5):
            for side in (1, -1):
                lines.append(("edge", shapely.offset_curve(line, side * (width / 2 - 0.35))))
        for kind, geom in lines:
            clipped = geom.difference(keep_out).intersection(ground.buffer(-0.1))
            for part in getattr(clipped, "geoms", [clipped]):
                if isinstance(part, shapely.LineString) and part.length > 2.0:
                    markings.append((kind, part))
    return markings
