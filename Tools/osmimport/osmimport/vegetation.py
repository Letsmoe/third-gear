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

    def add(model, x, y, crown, height, trunk, source, min_gap):
        if not (area.x_min <= x < area.x_max and area.y_min <= y < area.y_max):
            return False
        if not occ.free(x, y, min_gap):
            return False
        occ.add(x, y)
        plants.append(Plant(model, float(x), float(y), float(crown), float(height), float(trunk), source))
        return True

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
            pt = shapely.Point(x, y)
            if shapely.contains(road_prep, pt):
                # register positions are ~1 m accurate; never leave a trunk on the carriageway
                edge = nearest_points(road_prep.boundary, pt)[0]
                d = np.array([edge.x - x, edge.y - y])
                d /= max(np.linalg.norm(d), 1e-6)
                x, y = edge.x + d[0] * KEEP_OFF_ROAD, edge.y + d[1] * KEEP_OFF_ROAD
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
