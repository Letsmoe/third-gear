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
from .streets.cross_section import CrossSection, RoadContext, StripKind, cross_section
from .streets.tags import is_oneway, number

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


# Context of the ways the viewers and checks look at one by one, without buildings around them.
URBAN = RoadContext(urban=True)


def road_context(way: Way, buildings_union) -> RoadContext:
    """What a road's surroundings say about its cross-section."""
    return RoadContext(urban=_is_urban(way, buildings_union))


def road_section(tags, context: RoadContext = URBAN) -> CrossSection:
    """The road's cross-section: its strips from kerb to kerb (streets.cross_section)."""
    return cross_section(tags, context)


def road_width(tags, context: RoadContext = URBAN) -> float:
    """The carriageway width from kerb to kerb."""
    return cross_section(tags, context).width()


def road_lanes(tags) -> int:
    """The number of travel lanes for motor traffic, both directions together."""
    return cross_section(tags, URBAN).lane_count()


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
    sections: dict  # way id -> CrossSection
    widths: dict  # way id -> carriageway width
    surfaces: dict  # kind -> polygon (ground level, disjoint)
    ground: shapely.Geometry  # union of all ground road surfaces
    bridges: list  # (way, polygon)
    pavement: shapely.Geometry
    height: HeightGrid  # smooth road surface height (valid on/near roads)
    junction_zones: shapely.Geometry
    markings: list = field(default_factory=list)  # (kind, LineString with z via height) — see build_markings


def _is_ground(way: Way) -> bool:
    t = way.tags
    return t.get("bridge") in (None, "no") and number(t.get("layer"), 0) >= 0 and t.get("tunnel") in (None, "no", "building_passage")


def _is_tunnel(way: Way) -> bool:
    return way.tags.get("tunnel") not in (None, "no", "building_passage") or number(way.tags.get("layer"), 0) < 0


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
    sections = {w.id: cross_section(w.tags, road_context(w, buildings_union)) for w in ways}
    widths = {way_id: section.width() for way_id, section in sections.items()}
    ground_ways = [w for w in ways if _is_ground(w)]
    bridge_ways = [w for w in ways if not _is_ground(w)]

    # --- surface polygons per kind, major road classes win overlaps ---
    strips = {}
    for w in ground_ways:
        strips[w.id] = shapely.buffer(shapely.LineString(w.xy), widths[w.id] / 2, cap_style="round", join_style="round",
                                      quad_segs=4)
    ground = shapely.union_all(list(strips.values()))
    junction_zones = _junction_zones(ground_ways, widths)
    # Fillet concave corners (kerb radii at junctions) without growing the outline elsewhere. Only inside junctions:
    # along the road the same closing would pave over medians and islands narrower than twice the radius.
    closed = ground.buffer(FILLET_RADIUS, quad_segs=4).buffer(-FILLET_RADIUS, quad_segs=4)
    ground = shapely.make_valid(ground.union(closed.difference(ground).intersection(junction_zones)))

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

    net = RoadNetwork(ways=ways, sections=sections, widths=widths, surfaces=surfaces, ground=ground, bridges=bridges, pavement=pavement,
                      height=height, junction_zones=junction_zones)
    net.markings = build_markings(ground_ways, sections, junction_zones, ground)
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
    speed = number(way.tags.get("maxspeed"))
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


def _junction_zones(ground_ways, widths):
    """Discs around every junction node (three or more road ends), sized by the widest road there."""
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
    return shapely.union_all(zones) if zones else shapely.Polygon()


def build_markings(ground_ways, sections, junction_zones, ground):
    """Returns a list of (kind, LineString) in world xy. kind: 'dash_urban', 'dash_rural', 'solid', 'edge'.

    The lines sit on the boundaries of the cross-section's strips (positive offsets are to the physical right, which
    shapely's offset_curve gives for positive distances in the mirrored world frame)."""
    markings = []
    keep_out = junction_zones
    for w in ground_ways:
        t = w.tags
        hw = t.get("highway")
        if hw not in MARKED_CLASSES or t.get("lane_markings") == "no" or t.get("area") == "yes":
            continue
        section = sections[w.id]
        width = section.width()
        lanes = section.lane_count()
        line = shapely.LineString(w.xy)
        if line.length < 8:
            continue
        speed = number(t.get("maxspeed"), 50)
        rural = speed >= 70
        dash = "dash_rural" if rural else "dash_urban"
        lines = []
        if lanes >= 2 and (is_oneway(t) or width >= 5.0):
            lines += [(dash, _offset_line(line, offset)) for offset in section.lane_dividers()]
        if hw in {"motorway", "trunk", "primary", "motorway_link", "trunk_link"} or (rural and width >= 5.5):
            lines += [("edge", _offset_line(line, offset)) for offset in _travel_lane_edges(section)]
        for kind, geom in lines:
            clipped = geom.difference(keep_out).intersection(ground.buffer(-0.1))
            for part in getattr(clipped, "geoms", [clipped]):
                if isinstance(part, shapely.LineString) and part.length > 2.0:
                    markings.append((kind, part))
    return markings


def _offset_line(line, offset: float):
    """The line moved sideways (positive to the physical right); the line itself for offsets of about zero, which
    GEOS cannot offset (the centre line comes out of the strip sums as something like 1e-16)."""
    if abs(offset) < 0.01:
        return line
    return shapely.offset_curve(line, offset)


def _travel_lane_edges(section: CrossSection) -> list:
    """Offsets of the outer edges of the travel lanes, where the edge lines run."""
    lane_edges = [(left, right) for strip, left, right in section.strip_edges()
                  if strip.kind == StripKind.TRAVEL_LANE]
    return [lane_edges[0][0], lane_edges[-1][1]]
