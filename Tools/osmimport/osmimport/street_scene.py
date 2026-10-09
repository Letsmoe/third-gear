"""A simple 3D reconstruction of a rectangle for the street viewer: terrain, the game's road surfaces, markings and
furniture, the cycleways, footways, railways and water from OSM, and the typed buildings.

Everything is in world metres (x east, y south, z up). The viewer page maps that to three.js as (x, z, y), which keeps
the handedness. Meshes go out as base64 float32 triangle lists (non-indexed, so every face shades flat) with a colour
per vertex.
"""
import base64
from dataclasses import dataclass, field

import mapbox_earcut
import numpy as np
import shapely

from . import building_types, roads, roofs, street_layers
from .dem import HeightGrid

TERRAIN_CELLS = 256
SEGMENT_LENGTH = 2.0
DRAPE_CELL = 3.0
ROOF_OVERHANG = 0.3
PITCHED_ROOFS = {"gabled", "hipped", "half_hipped", "mansard", "gambrel", "pyramidal"}
CYCLE_LANE_WIDTH = 1.5
CYCLEWAY_WIDTH = 2.0
FOOTWAY_WIDTH = 1.8
RAILWAY_WIDTH = 2.8

# Surface layers in drawing order: (name, colour, lift above the terrain in metres). Later layers lie on top.
SURFACE_LAYERS = [
    ("water", "#1d6b64", 0.03),
    ("footway", "#b9b4aa", 0.04),
    ("railway", "#5a4a3c", 0.05),
    ("pavement", "#a7a39b", 0.05),
    ("asphalt", "#3c3d40", 0.06),
    ("pavers", "#6e6259", 0.06),
    ("cobble", "#5d5148", 0.06),
    ("cycleway", "#2f6fdc", 0.08),
    ("marking", "#f5f5f0", 0.10),
]
POLE_HEIGHTS = {"signal_head": 3.4, "sign": 2.5, "lamp": 6.5}


@dataclass
class MeshBuilder:
    """Collects triangles with a colour per vertex."""
    positions: list = field(default_factory=list)
    colours: list = field(default_factory=list)

    def add(self, triangles: np.ndarray, colour: str):
        """Adds (n, 3, 3) world triangles in one colour."""
        if len(triangles) == 0:
            return
        self.positions.append(np.asarray(triangles, np.float32).reshape(-1, 3))
        rgb = np.array([int(colour[index:index + 2], 16) / 255 for index in (1, 3, 5)], np.float32)
        self.colours.append(np.tile(rgb, (len(triangles) * 3, 1)))

    def encode(self) -> dict:
        """{"positions", "colours"} as base64 float32, or empty strings when nothing was added."""
        if not self.positions:
            return {"positions": "", "colours": ""}
        return {"positions": _base64(np.concatenate(self.positions)), "colours": _base64(np.concatenate(self.colours))}


def _base64(array: np.ndarray) -> str:
    """Little-endian float32 bytes of the array, base64 encoded."""
    return base64.b64encode(np.ascontiguousarray(array, "<f4").tobytes()).decode()


def triangulate_polygon(polygon: shapely.Polygon) -> np.ndarray:
    """(n, 3, 2) plan triangles of a polygon with holes."""
    rings = [np.asarray(polygon.exterior.coords)[:-1]] + [np.asarray(ring.coords)[:-1] for ring in polygon.interiors]
    vertices = np.concatenate(rings).astype(np.float64)
    ends = np.cumsum([len(ring) for ring in rings]).astype(np.uint32)
    indices = np.asarray(mapbox_earcut.triangulate_float64(vertices, ends), np.int64).reshape(-1, 3)
    return vertices[indices]


def _polygons(geometry):
    """The polygons of a (multi) polygon or collection."""
    if geometry.is_empty:
        return []
    if isinstance(geometry, shapely.Polygon):
        return [geometry]
    return [part for part in getattr(geometry, "geoms", []) if isinstance(part, shapely.Polygon)]


def grid_pieces(geometry, cell: float):
    """The geometry cut along a square grid of the given cell size, so no triangle of it spans more than one cell."""
    if geometry.is_empty:
        return []
    x_min, y_min, x_max, y_max = geometry.bounds
    xs = np.arange(np.floor(x_min / cell) * cell, x_max, cell)
    ys = np.arange(np.floor(y_min / cell) * cell, y_max, cell)
    grid_x, grid_y = np.meshgrid(xs, ys)
    cells = shapely.box(grid_x.ravel(), grid_y.ravel(), grid_x.ravel() + cell, grid_y.ravel() + cell)
    touching = shapely.STRtree(cells).query(geometry, predicate="intersects")
    pieces = shapely.intersection(cells[touching], geometry)
    return [polygon for piece in pieces for polygon in _polygons(piece)]


def draped_triangles(geometry, heights: HeightGrid, lift: float) -> np.ndarray:
    """(n, 3, 3) triangles of a plan area laid on the terrain: the area is cut into DRAPE_CELL squares and every
    vertex takes the terrain height plus lift, so the surface follows the ground instead of cutting through it."""
    triangles = []
    for polygon in grid_pieces(geometry, DRAPE_CELL):
        plan = triangulate_polygon(polygon)
        if len(plan) == 0:
            continue
        z = heights.sample(plan[..., 0], plan[..., 1]) + lift
        triangles.append(np.concatenate([plan, z[..., None]], axis=-1))
    if not triangles:
        return np.zeros((0, 3, 3))
    return np.concatenate(triangles)


def bridge_triangles(line: shapely.LineString, outline, heights: HeightGrid, lift: float) -> np.ndarray:
    """Triangles of a bridge deck: heights run straight between the terrain at the two ends of the way, because the
    terrain model is bare earth and dips under the bridge."""
    start_z = float(heights.sample(*line.coords[0]))
    end_z = float(heights.sample(*line.coords[-1]))
    triangles = []
    for polygon in _polygons(shapely.segmentize(outline, SEGMENT_LENGTH)):
        plan = triangulate_polygon(polygon)
        if len(plan) == 0:
            continue
        along = shapely.line_locate_point(line, shapely.points(plan.reshape(-1, 2)), normalized=True).reshape(-1, 3)
        z = start_z + (end_z - start_z) * along + lift
        triangles.append(np.concatenate([plan, z[..., None]], axis=-1))
    if not triangles:
        return np.zeros((0, 3, 3))
    return np.concatenate(triangles)


@dataclass
class StreetShapes:
    """Plan areas of the street surfaces by layer, with the bridge parts kept apart as (layer, line, outline)."""
    areas: dict = field(default_factory=dict)
    bridges: list = field(default_factory=list)

    def add(self, layer: str, line, outline, on_bridge: bool):
        """Adds one outline to a layer, or as a bridge deck when the way crosses on a bridge."""
        if outline.is_empty:
            return
        if on_bridge:
            self.bridges.append((layer, line, outline))
        else:
            self.areas.setdefault(layer, []).append(outline)


def _strip(line, width: float):
    """A flat-ended strip of the given width along a line."""
    return shapely.buffer(line, width / 2, cap_style="flat")


def street_shapes(data, streets) -> StreetShapes:
    """The plan areas of the game's roads and markings, and of the cycle lanes, cycleways, footways,
    railways and water from OSM."""
    shapes = StreetShapes()
    _add_game_shapes(shapes, streets)
    for way in data.roads:
        _add_road_shapes(shapes, way)
    for way in data.footways:
        line = shapely.LineString(way.xy)
        on_bridge = street_layers.is_bridge(way.tags)
        # Crossing ways stay footways: the game paints its own zebra bars over the road, which lies on top.
        if way.tags.get("highway") == "cycleway" or way.tags.get("bicycle") == "designated":
            shapes.add("cycleway", line, _strip(line, CYCLEWAY_WIDTH), on_bridge)
        else:
            shapes.add("footway", line, _strip(line, FOOTWAY_WIDTH), on_bridge)
    for way in data.railways:
        line = shapely.LineString(way.xy)
        shapes.add("railway", line, _strip(line, RAILWAY_WIDTH), street_layers.is_bridge(way.tags))
    _add_water_shapes(shapes, data)
    return shapes


def _add_water_shapes(shapes: StreetShapes, data):
    """Water areas, and waterway lines as strips of their width (tunnels and culverts left out)."""
    for _, tags, geometry in data.areas:
        if street_layers.is_water_area(tags):
            shapes.add("water", None, geometry, False)
    for way in data.waterways:
        if way.tags.get("waterway") not in street_layers.WATERWAY_WIDTHS or way.tags.get("tunnel", "no") != "no":
            continue
        line = shapely.LineString(way.xy)
        shapes.add("water", line, shapely.buffer(line, street_layers.waterway_width(way.tags) / 2), False)


def _add_road_shapes(shapes: StreetShapes, way):
    """The cycle lanes tagged on one road (the carriageway and its markings come from the game's own generator)."""
    centre = shapely.LineString(way.xy)
    section = roads.road_section(way.tags)
    on_bridge = street_layers.is_bridge(way.tags)
    for side, value in street_layers.cycleway_sides(way.tags).items():
        if value not in {"lane", "opposite_lane"}:
            continue
        lane = street_layers.side_offset(centre, street_layers.cycle_lane_offset(section, side), side)
        shapes.add("cycleway", lane, _strip(lane, CYCLE_LANE_WIDTH), on_bridge)


def _add_game_shapes(shapes: StreetShapes, streets):
    """The game's road surfaces, bridge decks, pavements and painted markings."""
    for kind, polygon in streets.surfaces:
        shapes.add(kind, None, polygon, False)
    for line, polygon in streets.bridges:
        shapes.add("asphalt", line, polygon, True)
    if streets.pavement is not None:
        shapes.add("pavement", None, streets.pavement, False)
    for _, polygon in streets.markings:
        shapes.add("marking", None, polygon, False)


def street_mesh(data, streets, heights: HeightGrid, clip_box) -> MeshBuilder:
    """Draped street surfaces inside the clip box, each layer unioned so overlaps don't flicker."""
    shapes = street_shapes(data, streets)
    mesh = MeshBuilder()
    for layer, colour, lift in SURFACE_LAYERS:
        outlines = shapes.areas.get(layer)
        if outlines:
            area = shapely.intersection(shapely.union_all(outlines), clip_box)
            mesh.add(draped_triangles(area, heights, lift), colour)
        for bridge_layer, line, outline in shapes.bridges:
            if bridge_layer == layer:
                mesh.add(bridge_triangles(line, shapely.intersection(outline, clip_box), heights, lift + 0.3), colour)
    return mesh


def _wall_triangles(polygon: shapely.Polygon, base: float, top: float) -> np.ndarray:
    """(n, 3, 3) triangles of the vertical walls around every ring of a footprint."""
    triangles = []
    for ring in [polygon.exterior, *polygon.interiors]:
        points = np.asarray(ring.coords)
        for (x0, y0), (x1, y1) in zip(points[:-1], points[1:]):
            triangles.append([[x0, y0, base], [x1, y1, base], [x1, y1, top]])
            triangles.append([[x0, y0, base], [x1, y1, top], [x0, y0, top]])
    return np.asarray(triangles, np.float64).reshape(-1, 3, 3)


def _roof_triangles(footprint, building_type, eave_z: float) -> np.ndarray:
    """(n, 3, 3) roof triangles: the skeleton roof for pitched shapes (gables come out hipped for now), a flat lid
    otherwise or when the skeleton fails."""
    shape = building_types.ROOF_NAMES[building_type.roof_shape]
    if shape in PITCHED_ROOFS:
        effective = "mansard" if shape in ("mansard", "gambrel") else "hipped"
        roof = roofs.build_roof(footprint, effective, building_type.pitch_degrees, ROOF_OVERHANG)
        if roof is not None and roof.faces:
            faces = [face.vertices[face.triangles][..., :3] for face in roof.faces]
            triangles = np.concatenate(faces).astype(np.float64)
            triangles[..., 2] += eave_z
            return triangles
    plan = triangulate_polygon(footprint)
    return np.concatenate([plan, np.full(plan.shape[:-1] + (1,), eave_z)], axis=-1)


def building_mesh(buildings, heights: HeightGrid):
    """Walls up to the typed eave height on the lowest ground under the footprint, and the roofs, coloured by class.
    Returns the mesh and, per building in mesh order, its triangle count and the pop-up the map shows for it."""
    mesh = MeshBuilder()
    info = []
    for osm_id, tags, footprint, building_type in buildings:
        colour = street_layers.BUILDING_CLASS_COLOURS[building_types.CLASS_NAMES[building_type.class_id]]
        ring = np.asarray(footprint.exterior.coords)
        base = float(np.min(heights.sample(ring[:, 0], ring[:, 1])))
        eave_z = base + building_type.eave_height
        walls = _wall_triangles(footprint, base - 0.5, eave_z)
        roof = _roof_triangles(footprint, building_type, eave_z)
        mesh.add(walls, colour)
        mesh.add(roof, _darker(colour))
        popup = street_layers.Layers()
        street_layers.add_building(popup, osm_id, tags, footprint, building_type)
        info.append({"triangles": len(walls) + len(roof), "properties": popup.by_name["Buildings"][0][2]})
    return mesh, info


def _darker(colour: str) -> str:
    """The colour at 70 % brightness, so roofs read apart from walls."""
    channels = [int(int(colour[index:index + 2], 16) * 0.7) for index in (1, 3, 5)]
    return "#" + "".join(f"{channel:02x}" for channel in channels)


def poles(streets, heights: HeightGrid, clip_box) -> list:
    """The game's signal heads, signs and lamps as {kind, x, y, z, height, label} for the page to draw as poles."""
    result = []
    for pole in streets.poles:
        if not clip_box.contains(shapely.Point(pole["x"], pole["y"])):
            continue
        z = float(heights.sample(pole["x"], pole["y"]))
        result.append({"kind": pole["kind"], "x": round(pole["x"], 2), "y": round(pole["y"], 2), "z": round(z, 2),
                       "height": POLE_HEIGHTS[pole["kind"]], "label": pole["label"]})
    return result


def terrain(heights: HeightGrid, clip_box) -> dict:
    """The terrain over the clip box as a TERRAIN_CELLS square grid of heights, row 0 at the north edge."""
    x_min, y_min, x_max, y_max = clip_box.bounds
    xs = np.linspace(x_min, x_max, TERRAIN_CELLS + 1)
    ys = np.linspace(y_min, y_max, TERRAIN_CELLS + 1)
    grid_x, grid_y = np.meshgrid(xs, ys)
    z = heights.sample(grid_x, grid_y)
    return {"x_min": x_min, "y_min": y_min, "x_max": x_max, "y_max": y_max, "cells": TERRAIN_CELLS,
            "heights": _base64(z.astype(np.float32))}


def build(data, buildings, streets, heights: HeightGrid, clip_box) -> dict:
    """The whole scene for the viewer page; streets is the game's generated street geometry (game_streets.py)."""
    buildings_mesh, building_info = building_mesh(buildings, heights)
    return {
        "terrain": terrain(heights, clip_box),
        "streets": street_mesh(data, streets, heights, clip_box).encode(),
        "buildings": buildings_mesh.encode(),
        "building_info": building_info,
        "poles": poles(streets, heights, clip_box),
    }
