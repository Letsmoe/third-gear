"""Trees and shrubs: real street trees (Hamburg Straßenbaumkataster), OSM trees / tree rows / hedges, and scattered
trees in woods, parks and scrub.

Every instance gets a model key (see Scripts/vegetation_models.py), position, yaw, crown diameter, height and trunk
diameter; the Unreal side scales the model to the real size.
"""
import json
import os
from dataclasses import dataclass

import numpy as np
import shapely
from shapely.ops import nearest_points

from .dem import HeightGrid
from .geo import Area, utm_to_world
from .osm import OsmData

# Genus (German name in the Straßenbaumkataster) -> (model, height / crown diameter ratio)
CONIFER_GENERA = {"Kiefer", "Fichte", "Tanne", "Lärche", "Douglasie", "Eibe", "Lebensbaum", "Scheinzypresse",
                  "Zeder", "Hemlocktanne", "Mammutbaum", "Sicheltanne", "Wacholder"}
SLENDER_GENERA = {"Pappel": 2.2, "Birke": 1.9, "Erle": 1.8, "Hainbuche": 1.4, "Säulen-Hainbuche": 2.5,
                  "Esche": 1.5, "Robinie, Scheinakazie": 1.6, "Weide": 1.3}
SMALL_GENERA = {"Steinobst", "Apfelbaum", "Weissdorn", "Vogelbeere", "Mehlbeere", "Birne", "Hasel", "Zierkirsche",
                "Magnolie", "Felsenbirne", "Amberbaum"}
DEFAULT_HEIGHT_RATIO = 1.3  # mature linden/oak/maple street trees: ~20 m tall at ~15 m crown

KEEP_OFF_ROAD = 0.4        # street trees whose trunk lands on the carriageway are pushed this far off it
KERB_STRIP = 1.2           # pavement within this distance of the carriageway is the planting strip of street trees
KERB_SNAP_OFFSET = 0.8     # register trees standing further out on the pavement are moved to this distance from the kerb
WIDE_PAVEMENT_HALF_WIDTH = 2.0  # pavement this far from every edge is wide enough for a tree row: no snapping
BUILDING_TRUNK_CLEARANCE = 1.0  # every trunk stays this far from facades
CROWN_FACADE_OVERLAP = 0.8      # a crown may reach this far past the nearest facade
MIN_CLAMPED_CROWN = 2.5         # trees whose crown would shrink below this next to a building are dropped
CROWN_OVERLAP_FACTOR = 0.6      # a trunk keeps this fraction of a neighbour's crown radius clear


@dataclass
class Plant:
    model: str
    x: float
    y: float
    crown: float   # m
    height: float  # m
    trunk: float   # trunk diameter m (0 = no collider)
    source: str


def _float(value):
    try:
        return float(str(value).replace(",", ".").replace("m", "").strip())
    except (TypeError, ValueError):
        return None


def _street_tree(props, rng):
    genus = props.get("gattung_deutsch") or ""
    crown = float(props.get("kronendurchmesser") or 0) or rng.uniform(4, 8)
    crown = float(np.clip(crown, 1.5, 25))
    girth_cm = props.get("stammumfang") or 0
    trunk = float(np.clip(girth_cm / 100 / np.pi, 0.08, 1.5)) if girth_cm else max(0.1, crown * 0.04)
    if genus in CONIFER_GENERA:
        return "conifer", crown, float(np.clip(crown * 2.8, 3, 30)), trunk
    if genus in SMALL_GENERA:
        return "broadleaf", crown, float(np.clip(crown * 1.2, 3, 12)), trunk
    ratio = SLENDER_GENERA.get(genus, DEFAULT_HEIGHT_RATIO)
    return "broadleaf", crown, float(np.clip(crown * ratio * rng.uniform(0.9, 1.1), 3, 32)), trunk


def _osm_tree(tags, rng):
    leaf = tags.get("leaf_type")
    crown = _float(tags.get("diameter_crown")) or rng.uniform(5, 10)
    height = _float(tags.get("height"))
    girth = _float(tags.get("circumference"))
    trunk = float(np.clip(girth / np.pi, 0.08, 1.5)) if girth else max(0.12, crown * 0.04)
    if leaf == "needleleaved":
        return "conifer", crown, height or crown * 2.8, trunk
    return "broadleaf", crown, height or crown * DEFAULT_HEIGHT_RATIO * rng.uniform(0.9, 1.15), trunk


def _poisson_points(poly, spacing, rng, keep=1.0):
    """Jittered grid inside poly: roughly natural spacing without visible rows."""
    if poly.is_empty:
        return np.empty((0, 2))
    x0, y0, x1, y1 = poly.bounds
    gx, gy = np.meshgrid(np.arange(x0, x1, spacing), np.arange(y0, y1, spacing))
    pts = np.column_stack([gx.ravel(), gy.ravel()])
    pts += rng.uniform(-0.45, 0.45, pts.shape) * spacing
    if keep < 1.0:
        pts = pts[rng.random(len(pts)) < keep]
    if len(pts) == 0:
        return pts
    inside = shapely.contains_xy(poly, pts[:, 0], pts[:, 1])
    return pts[inside]


def _line_points(xy, spacing, rng, jitter=0.15):
    line = shapely.LineString(xy)
    if line.length < 1e-3:
        return np.empty((0, 2))
    n = max(1, int(line.length / spacing))
    d = (np.arange(n) + 0.5) * line.length / n
    d += rng.uniform(-jitter, jitter, n) * spacing
    pts = shapely.line_interpolate_point(line, np.clip(d, 0, line.length))
    return shapely.get_coordinates(pts)


class _Occupancy:
    """Rejects new plants too close to existing ones (simple grid hash)."""

    def __init__(self, cell=4.0):
        self.cell = cell
        self.grid = {}
        self.crowns = {}
        self.max_crown_radius = 12.5  # register crowns are clipped to 25 m diameter

    def free(self, x, y, radius):
        cx, cy = int(x // self.cell), int(y // self.cell)
        r = int(np.ceil(radius / self.cell))
        for i in range(cx - r, cx + r + 1):
            for j in range(cy - r, cy + r + 1):
                for px, py in self.grid.get((i, j), ()):
                    if (px - x) ** 2 + (py - y) ** 2 < radius * radius:
                        return False
        return True

    def add(self, x, y):
        self.grid.setdefault((int(x // self.cell), int(y // self.cell)), []).append((x, y))

    def add_crown(self, x, y, radius):
        """Records a tree crown for crown_free."""
        self.crowns.setdefault((int(x // self.cell), int(y // self.cell)), []).append((x, y, radius))

    def crown_free(self, x, y, radius):
        """True if the trunk is clear of every recorded crown (scaled) and no recorded trunk lies in the new crown."""
        reach = max(radius, self.max_crown_radius)
        span = int(np.ceil(reach / self.cell))
        cx, cy = int(x // self.cell), int(y // self.cell)
        for i in range(cx - span, cx + span + 1):
            for j in range(cy - span, cy + span + 1):
                for px, py, other_radius in self.crowns.get((i, j), ()):
                    distance_squared = (px - x) ** 2 + (py - y) ** 2
                    needed = CROWN_OVERLAP_FACTOR * max(radius, other_radius)
                    if distance_squared < needed * needed:
                        return False
        return True


def push_towards(boundary, point, distance):
    """The point on boundary nearest to point, moved `distance` further along the line from point through it."""
    edge = nearest_points(boundary, point)[0]
    direction = np.array([edge.x - point.x, edge.y - point.y])
    direction /= max(np.linalg.norm(direction), 1e-6)
    return edge.x + direction[0] * distance, edge.y + direction[1] * distance


def push_away(footprint, point, distance):
    """The point at `distance` outside the footprint, on the line from the nearest edge point through `point`."""
    edge = nearest_points(footprint.boundary, point)[0]
    direction = np.array([point.x - edge.x, point.y - edge.y])
    if footprint.contains(point):
        direction = -direction
    if np.linalg.norm(direction) < 1e-6:
        return point.x, point.y
    direction /= np.linalg.norm(direction)
    return edge.x + direction[0] * distance, edge.y + direction[1] * distance


def build(osm: OsmData, area: Area, ground: HeightGrid, road_ground, pavement, paths, water, buildings_union,
          street_trees_path=None, canopy=None, seed=7):
    rng = np.random.default_rng(seed)
    occ = _Occupancy()
    plants = []
    extent = shapely.box(area.x_min, area.y_min, area.x_max, area.y_max)

    road_prep = road_ground
    shapely.prepare(road_prep)
    hard = shapely.union_all([g for g in (road_ground, pavement, buildings_union, water) if g is not None])
    # procedural plants keep clear of roads, pavements, buildings, paths and water
    blocked = shapely.union_all([hard.buffer(1.5), paths.buffer(0.8)]) if not paths.is_empty else hard.buffer(1.5)
    shapely.prepare(blocked)

    building_parts = shapely.get_parts(buildings_union) if buildings_union is not None else []
    building_index = shapely.STRtree(list(building_parts)) if len(building_parts) else None
    pavement_prep = pavement if pavement is not None else shapely.Polygon()
    shapely.prepare(pavement_prep)
    # the kerb-side strip of the pavement, where street trees belong
    walkable_pavement = pavement_prep.difference(road_ground.buffer(KERB_STRIP))
    shapely.prepare(walkable_pavement)
    wide_pavement = pavement_prep.buffer(-WIDE_PAVEMENT_HALF_WIDTH)
    shapely.prepare(wide_pavement)

    def facade_distance(x, y):
        """Distance from (x, y) to the nearest building footprint (inf without buildings)."""
        if building_index is None:
            return np.inf
        _, distances = building_index.query_nearest(shapely.Point(x, y), return_distance=True)
        return float(distances[0])

    def fit_crown_to_facades(x, y, crown, height):
        """Shrinks a crown that would reach through a facade; returns (crown, height), crown 0 if too little is left."""
        allowed = 2.0 * (facade_distance(x, y) + CROWN_FACADE_OVERLAP)
        if crown <= allowed:
            return crown, height
        if allowed < MIN_CLAMPED_CROWN:
            return 0.0, height
        return allowed, height * (allowed / crown) ** 0.5

    def tree_position_allowed(model, x, y, source):
        """Placement rules for every tree that is not a register street tree: clear of buildings and pavement."""
        if model == "shrub":
            return True
        if facade_distance(x, y) < BUILDING_TRUNK_CLEARANCE:
            return False
        if source == "canopy":
            return not shapely.contains_xy(pavement_prep, x, y)
        return not shapely.contains_xy(walkable_pavement, x, y)

    def add(model, x, y, crown, height, trunk, source, min_gap):
        if not (area.x_min <= x < area.x_max and area.y_min <= y < area.y_max):
            return False
        if not occ.free(x, y, min_gap):
            return False
        is_tree = model != "shrub"
        if is_tree:
            if source != "kataster":
                if not tree_position_allowed(model, x, y, source):
                    return False
                if not occ.crown_free(x, y, crown / 2):
                    return False
            crown, height = fit_crown_to_facades(x, y, crown, height)
            if crown == 0.0:
                if source != "kataster":
                    return False
                crown = MIN_CLAMPED_CROWN
            occ.add_crown(x, y, crown / 2)
        occ.add(x, y)
        plants.append(Plant(model, float(x), float(y), float(crown), float(height), float(trunk), source))
        return True

    def street_tree_position(x, y):
        """Register position corrected for its ~1 m error: off the carriageway, out of buildings, onto the kerb strip."""
        point = shapely.Point(x, y)
        if shapely.contains(road_prep, point):
            x, y = push_towards(road_prep.boundary, point, KEEP_OFF_ROAD)
        if building_index is not None and facade_distance(x, y) < BUILDING_TRUNK_CLEARANCE:
            nearest = building_index.nearest(point)
            x, y = push_away(building_parts[nearest], point, BUILDING_TRUNK_CLEARANCE)
        elif shapely.contains(walkable_pavement, point) and not shapely.contains(wide_pavement, point):
            edge = nearest_points(road_prep.boundary, point)[0]
            away = np.array([x - edge.x, y - edge.y])
            away /= max(np.linalg.norm(away), 1e-6)
            snapped = shapely.Point(edge.x + away[0] * KERB_SNAP_OFFSET, edge.y + away[1] * KERB_SNAP_OFFSET)
            if shapely.contains(pavement_prep, snapped):
                x, y = snapped.x, snapped.y
        return x, y

    # 1) Real street trees: exact positions, species, crown size.
    n_street = 0
    if street_trees_path and os.path.exists(street_trees_path):
        with open(street_trees_path) as f:
            features = json.load(f)["features"]
        for feat in features:
            e, n = feat["geometry"]["coordinates"][0][:2]
            x, y = utm_to_world(e, n, area)
            if not (area.x_min <= x < area.x_max and area.y_min <= y < area.y_max):
                continue
            x, y = street_tree_position(x, y)
            model, crown, height, trunk = _street_tree(feat["properties"], rng)
            if canopy is not None and canopy.is_covered(x, y):
                measured = _canopy_height(canopy, x, y)
                if measured > 3.0:
                    height = float(np.clip(measured, 0.5 * height, 1.6 * height))
            n_street += add(model, x, y, crown, height, trunk, "kataster", 0.8)

    # 2) OSM single trees (Hamburg's register already covers street trees -> skip near-duplicates).
    for p in osm.points:
        if p.tags.get("natural") == "tree" and not shapely.contains_xy(road_prep, p.x, p.y):
            model, crown, height, trunk = _osm_tree(p.tags, rng)
            add(model, p.x, p.y, crown, height, trunk, "osm_tree", 3.0)

    # 3) Tree rows and hedges.
    for w in osm.tree_rows:
        for x, y in _line_points(w.xy, 9.0, rng):
            if not shapely.contains_xy(blocked, x, y):
                model, crown, height, trunk = _osm_tree(w.tags, rng)
                add(model, x, y, crown, height, trunk, "tree_row", 4.0)
    for w in osm.hedges:
        for x, y in _line_points(w.xy, 1.6, rng, jitter=0.2):
            if not shapely.contains_xy(hard, x, y):
                h = _float(w.tags.get("height")) or rng.uniform(1.4, 2.2)
                add("shrub", x, y, rng.uniform(1.6, 2.2), h, 0.0, "hedge", 0.9)

    # 4) Trees and bushes measured in the surface model (gardens, parks, woods).
    if canopy is not None:
        needle = [g for _, t, g in osm.areas if t.get("leaf_type") == "needleleaved"]
        needle = shapely.union_all(needle) if needle else shapely.Polygon()
        shapely.prepare(needle)
        for x, y, height, crown in canopy.trees:
            if shapely.contains_xy(road_prep, x, y):
                continue
            conifer = shapely.contains_xy(needle, x, y) or rng.random() < 0.12
            if conifer:
                add("conifer", x, y, min(crown, height * 0.45), height, max(0.1, height * 0.02), "canopy", max(2.0, 0.3 * crown))
            else:
                add("broadleaf", x, y, crown, height, max(0.1, crown * 0.045), "canopy", max(2.5, 0.35 * crown))
        for x, y, height in canopy.shrubs:
            if not shapely.contains_xy(hard, x, y):
                add("shrub", x, y, rng.uniform(1.6, 2.6), height, 0.0, "canopy_shrub", 1.4)

    # 5) Woods, parks, scrub without surface-model coverage: scattered.
    for _, tags, geom in osm.areas:
        geom = geom.intersection(extent)
        if geom.is_empty:
            continue
        free_area = geom.difference(blocked)
        if canopy is not None and canopy.is_covered(*free_area.representative_point().xy).all():
            continue  # measured vegetation already placed
        if tags.get("landuse") == "forest" or tags.get("natural") == "wood":
            leaf = tags.get("leaf_type", "mixed")
            for x, y in _poisson_points(free_area, 6.5, rng):
                conifer = leaf == "needleleaved" or (leaf == "mixed" and rng.random() < 0.35)
                crown = rng.uniform(5, 9)
                if conifer:
                    add("conifer", x, y, crown * 0.8, rng.uniform(16, 26), crown * 0.05, "wood", 3.5)
                else:
                    add("broadleaf", x, y, crown, crown * rng.uniform(1.8, 2.4), crown * 0.045, "wood", 3.5)
            # understorey along the wood edge
            edge = free_area.difference(free_area.buffer(-6))
            for x, y in _poisson_points(edge, 3.5, rng, keep=0.6):
                add("shrub", x, y, rng.uniform(1.5, 3), rng.uniform(1.2, 2.5), 0.0, "wood_edge", 1.5)
        elif tags.get("natural") == "scrub":
            for x, y in _poisson_points(free_area, 3.0, rng, keep=0.7):
                add("shrub", x, y, rng.uniform(1.5, 3.5), rng.uniform(1.0, 2.5), 0.0, "scrub", 1.5)
        elif tags.get("leisure") == "park" or tags.get("landuse") in {"cemetery", "village_green"}:
            for x, y in _poisson_points(free_area, 16.0, rng, keep=0.45):
                model, crown, height, trunk = _osm_tree({}, rng)
                crown *= rng.uniform(1.0, 1.4)  # park trees grow freely
                add(model, x, y, crown, crown * DEFAULT_HEIGHT_RATIO, trunk, "park", 6.0)

    if plants:
        xs = np.array([p.x for p in plants])
        ys = np.array([p.y for p in plants])
        zs = ground.sample(xs, ys)
    else:
        zs = []
    counts = {}
    for p in plants:
        counts[p.source] = counts.get(p.source, 0) + 1
    return plants, zs, counts


def _canopy_height(canopy, x, y):
    """Highest canopy point within ~2 m (the register position is the trunk, not the crown top)."""
    g = canopy.grid
    c = int((x - g.x0) / g.res)
    r = int((y - g.y0) / g.res)
    h, w = g.z.shape
    window = g.z[max(r - 2, 0):min(r + 3, h), max(c - 2, 0):min(c + 3, w)]
    return float(window.max()) if window.size else 0.0


def write(path, plants, zs, seed=11):
    """vegetation.json: {model: [[x, y, z, yaw, crown, height, trunk], ...]} in world metres / degrees."""
    rng = np.random.default_rng(seed)
    out = {}
    for p, z in zip(plants, zs):
        out.setdefault(p.model, []).append([round(p.x, 2), round(p.y, 2), round(float(z), 2),
                                            round(float(rng.uniform(0, 360)), 1), round(p.crown, 2),
                                            round(p.height, 2), round(p.trunk, 3)])
    with open(path, "w") as f:
        json.dump({"models": out}, f)
