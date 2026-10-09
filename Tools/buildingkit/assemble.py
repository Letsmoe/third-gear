"""Assembles a test house per style from kit pieces only (plus the generated gable fill) and renders it.

Run inside Blender:
    blender -b --factory-startup --python-exit-code 1 -P Tools/buildingkit/assemble.py -- <output_dir> [style ...]

Each house is three modules wide and three deep, with corner posts, so every piece type and every wall rotation is
exercised. Rotations about Z: right wall +90, back wall 180, left wall -90 (see README, "Assembling a building")."""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402

from kit import blender_io, styles  # noqa: E402
from kit.geom import Mesh  # noqa: E402

MODULE = styles.MODULE

# What belongs to a wall bay type, positioned in the bay frame so no extra offsets are needed.
BAY_EXTRAS = {
    "brick": {
        "Wall_Window": ["Window", "Sill", "Lintel_Arch_Window"],
        "Wall_Door": ["Door", "Lintel_Arch_Door", "Door_Step"],
    },
    "plaster": {"Wall_Window": ["Window", "Sill"], "Wall_Door": ["Door", "Canopy"]},
    "block": {
        "Wall_Window": ["Window", "Sill"],
        "Wall_Balcony": ["Window_Balcony", "Balcony_Panel"],
        "Wall_Door": ["Door_Glazed", "Canopy"],
        "Wall_Stair": ["Window_Stair"],
    },
    "farm": {
        "Wall_Frame_Window": ["Window"],
        "Wall_Frame_Door": ["Door"],
        "Wall_Frame_Gate": ["Gate"],
    },
}

# Per style: front bays per storey (bottom to top), side bays, bands, roof setup.
HOUSES = {
    "brick": {
        "front": [["Wall_Window", "Wall_Door", "Wall_Window"], ["Wall_Window"] * 3, ["Wall_Window"] * 3],
        "side": ["Wall_Solid", "Wall_Window", "Wall_Solid"],
        "bands": {"Plinth": [0], "StringCourse": [0, 1], "Cornice": [2]},
        "roof": "tile", "roof_prefix": "Roof_Tile", "eave_overhang": 0.42, "eave_height": 0.10,
        "gutter": True, "downpipe": True, "dormer": "Dormer_Gable", "dormer_x": 3.0, "chimney": True,
    },
    "plaster": {
        "front": [["Wall_Window", "Wall_Door", "Wall_Window"], ["Wall_Window"] * 3],
        "side": ["Wall_Solid", "Wall_Window", "Wall_Solid"],
        "bands": {"Plinth": [0], "StringCourse": [0], "Cornice": [1]},
        "roof": "tile", "roof_prefix": "Roof_Tile", "eave_overhang": 0.32, "eave_height": 0.08,
        "gutter": True, "downpipe": True, "dormer": "Dormer_Shed", "dormer_x": 2.1, "chimney": True,
    },
    "block": {
        "front": [["Wall_Window", "Wall_Door", "Wall_Stair"],
                  ["Wall_Balcony", "Wall_Window", "Wall_Balcony"],
                  ["Wall_Balcony", "Wall_Window", "Wall_Balcony"]],
        "side": ["Wall_Solid", "Wall_Window", "Wall_Solid"],
        "bands": {"Plinth": [0], "FloorBand": [0, 1], "RoofEdge": [2]},
        "roof": "flat", "downpipe": True,
    },
    "farm": {
        "front": [["Wall_Frame_Window", "Wall_Frame_Door", "Wall_Frame_Gate"]],
        "side": ["Wall_Frame", "Wall_Frame_Window", "Wall_Frame"],
        "bands": {"Plinth": [0]},
        "roof": "thatch", "roof_prefix": "Roof_Thatch", "eave_overhang": 0.6, "eave_height": -0.10,
        "dormer": "Dormer_Eyebrow", "dormer_x": 3.6, "chimney": True,
    },
}


class Assembler:
    """Instances kit pieces into the scene, sharing one mesh per piece."""

    def __init__(self, style_name, pieces, spec):
        self.style_name = style_name
        self.pieces = pieces
        self.spec = spec
        self.collection = bpy.context.scene.collection
        self.mesh_cache = {}
        self.thickness = spec["thickness"]
        self.storey = spec["storey"]
        self.width_total = 3 * MODULE + 2 * self.thickness
        self.depth_total = 3 * MODULE + 2 * self.thickness

    def put(self, name, x, y, z, rotation=0.0):
        """Places one kit piece with its origin at (x, y, z) rotated about Z."""
        if name not in self.mesh_cache:
            self.mesh_cache[name] = blender_io.build_mesh_data(name, self.pieces[name])
        blender_io.add_piece_object(self.collection, name, None, (x, y, z), rotation, self.mesh_cache[name])

    def put_mesh(self, name, kit_mesh, x=0.0, y=0.0, z=0.0):
        blender_io.add_piece_object(self.collection, name, kit_mesh, (x, y, z))

    def wall_origin(self, wall, distance_along, z):
        """Origin and rotation for a piece standing on the given wall at the distance along the wall (after the corner)."""
        thickness = self.thickness
        if wall == "front":
            return (thickness + distance_along, 0.0, z), 0.0
        if wall == "right":
            return (self.width_total, thickness + distance_along, z), 90.0
        if wall == "back":
            return (self.width_total - thickness - distance_along, self.depth_total, z), 180.0
        return (0.0, self.depth_total - thickness - distance_along, z), -90.0

    def corner_positions(self):
        """Corner pieces with origin and rotation: front-left, front-right, back-right, back-left."""
        thickness = self.thickness
        return [
            ("L", (0.0, 0.0), 0.0),
            ("R", (self.width_total - thickness, 0.0), 0.0),
            ("L", (self.width_total, self.depth_total), 180.0),
            ("R", (thickness, self.depth_total), 180.0),
        ]

    def wall_bays(self, wall, bay_names, storey_index):
        z = storey_index * self.storey
        extras = BAY_EXTRAS[self.style_name]
        for index, bay in enumerate(bay_names):
            origin, rotation = self.wall_origin(wall, index * MODULE, z)
            self.put(bay, *origin, rotation)
            for extra in extras.get(bay, []):
                self.put(extra, *origin, rotation)

    def band_ring(self, prefix, storey_index):
        """A band around the whole building at a storey: straight pieces on every wall plus four mitred corners."""
        z = storey_index * self.storey
        for wall in ("front", "right", "back", "left"):
            for index in range(3):
                origin, rotation = self.wall_origin(wall, index * MODULE, z)
                self.put(prefix + "_M", *origin, rotation)
        for side, (x, y), rotation in self.corner_positions():
            self.put("%s_Corner%s" % (prefix, side), x, y, z, rotation)

    def corners(self, storey_index):
        z = storey_index * self.storey
        for side, (x, y), rotation in self.corner_positions():
            self.put("Corner_" + side, x, y, z, rotation)

    def body(self, house):
        storeys = len(house["front"])
        for storey_index, front_bays in enumerate(house["front"]):
            self.wall_bays("front", front_bays, storey_index)
            for wall in ("right", "back", "left"):
                bays = house["side"] if wall != "back" else ["Wall_Solid", "Wall_Window", "Wall_Solid"]
                if self.style_name == "farm" and wall == "back":
                    bays = ["Wall_Frame", "Wall_Frame", "Wall_Frame"]
                self.wall_bays(wall, bays, storey_index)
            self.corners(storey_index)
        for prefix, storey_list in house["bands"].items():
            for storey_index in storey_list:
                self.band_ring(prefix, storey_index)
        if house.get("downpipe"):
            for storey_index in range(storeys):
                self.put("Downpipe", self.thickness, 0.0, storey_index * self.storey)
        return storeys * self.storey

    # ------------------------------------------------------------------------------------------------- roofs
    def gable_fill(self, wall_top, eave_edge_z, eave_overhang, slope_tan):
        """Plain generated gable triangles for the two side walls, as the runtime generator will produce them."""
        depth = self.depth_total
        ridge_z = eave_edge_z + (depth / 2.0 + eave_overhang) * slope_tan
        under = eave_edge_z + eave_overhang * slope_tan - 0.14
        polygon = [(0.0, wall_top), (depth, wall_top), (depth, under), (depth / 2.0, ridge_z - 0.14), (0.0, under)]
        material = self.spec["wall_material"]
        for x_start in (0.0, self.width_total - self.thickness):
            mesh = Mesh()
            mesh.prism(polygon, "yz", x_start, x_start + self.thickness, material, caps="LH")
            self.put_mesh("GableFill", mesh)

    def gable_roof(self, house, wall_top):
        prefix = house["roof_prefix"]
        pitch = self.spec["roof_pitch"]
        cosine = math.cos(math.radians(pitch))
        sine = math.sin(math.radians(pitch))
        slope_tan = math.tan(math.radians(pitch))
        overhang = house["eave_overhang"]
        eave_z = wall_top + house["eave_height"]
        run_total = self.depth_total / 2.0 + overhang
        count = math.ceil(run_total / cosine - 1e-6)
        piece_count_x = math.ceil((self.width_total + 0.2) / MODULE)
        x_start = (self.width_total - piece_count_x * MODULE) / 2.0
        self.gable_fill(wall_top, eave_z, overhang, slope_tan)
        apex_z = eave_z + run_total * slope_tan
        for slope in (0, 1):
            for column in range(piece_count_x):
                for row in range(count):
                    name = prefix + ("_Eave" if row == 0 else "_Plane")
                    run_start = row * cosine
                    lift = 0.0
                    if row == count - 1 and row > 0:
                        run_start = run_total - cosine
                        lift = 0.012
                    local = (x_start + column * MODULE, -overhang + run_start, eave_z + run_start * slope_tan)
                    local = (local[0], local[1] - lift * sine, local[2] + lift * cosine)
                    self.place_on_slope(name, local, slope)
                self.place_on_slope(prefix + "_Ridge", (x_start + column * MODULE, self.depth_total / 2.0, apex_z), slope, flat=True)
        return eave_z, apex_z, overhang

    def place_on_slope(self, name, local, slope, flat=False):
        """Front slope as is; back slope rotated 180 degrees around the building's vertical centre axis."""
        x, y, z = local
        if slope == 0:
            self.put(name, x, y, z, 0.0)
            return
        if flat and name.endswith("Ridge") and slope == 1:
            return
        self.put(name, self.width_total - x, self.depth_total - y, z, 180.0)

    def tile_extras(self, house, wall_top, eave_z, apex_z, overhang):
        pitch = self.spec["roof_pitch"]
        slope_tan = math.tan(math.radians(pitch))
        if house.get("gutter"):
            gutter_count = math.ceil((self.width_total + 0.2) / MODULE)
            x_start = (self.width_total - gutter_count * MODULE) / 2.0
            for column in range(gutter_count):
                self.put("Gutter_M", x_start + column * MODULE, 0.0, eave_z)
                self.put("Gutter_M", self.width_total - x_start - column * MODULE, self.depth_total, eave_z, 180.0)
        if house.get("dormer"):
            surface_y = 2.0
            surface_z = eave_z + (surface_y + overhang) * slope_tan
            self.put(house["dormer"], house["dormer_x"], surface_y, surface_z - 0.03)
        if house.get("chimney"):
            self.put("Chimney", self.width_total - 1.6, self.depth_total / 2.0 - 0.8, apex_z - 0.35)

    def flat_roof(self, house, wall_top):
        for column in range(3):
            for row in range(3):
                self.put("Roof_Flat", self.thickness + column * MODULE, self.thickness + row * MODULE, wall_top)


def add_reference_human(location):
    """A 1.8 m by 0.5 m by 0.3 m grey box for scale."""
    mesh = Mesh()
    mesh.box(-0.25, 0.25, -0.15, 0.15, 0.0, 1.8, "Metal")
    blender_io.add_piece_object(bpy.context.scene.collection, "Reference_1.8m", mesh, location)


def render_house(style_name, output_dir):
    """Builds the style's test house and renders a front view and a three-quarter view."""
    pieces, spec = styles.STYLES[style_name]()
    house = HOUSES[style_name]
    blender_io.reset_scene()
    blender_io.setup_world()
    blender_io.add_ground(size=200.0, z=-0.005, colour=(0.28, 0.28, 0.27, 1.0))
    assembler = Assembler(style_name, pieces, spec)
    wall_top = assembler.body(house)
    if house["roof"] == "flat":
        assembler.flat_roof(house, wall_top)
        apex = wall_top + 0.4
    else:
        eave_z, apex, overhang = assembler.gable_roof(house, wall_top)
        assembler.tile_extras(house, wall_top, eave_z, apex, overhang)
    add_reference_human((-1.6, -1.8, 0.0))
    center = (assembler.width_total / 2.0, assembler.depth_total / 2.0, apex / 2.0)
    height = apex
    width = assembler.width_total
    size = max(width, height)
    blender_io.add_camera((center[0], -size * 1.55, 1.7 + height * 0.1), (center[0], 0.0, height * 0.5), lens=40.0)
    blender_io.render_to(os.path.join(output_dir, "renders", style_name + "_facade_front.png"), 1600, 1600)
    reach = size * 1.35
    blender_io.add_camera((-reach * 0.75, -reach * 0.95, 1.8 + height * 0.18), (center[0], center[1] * 0.7, height * 0.45), lens=38.0)
    blender_io.render_to(os.path.join(output_dir, "renders", style_name + "_facade_corner.png"), 1800, 1500)
    detail_x = assembler.thickness + MODULE * 0.5
    blender_io.add_camera((detail_x + 3.2, -3.6, 2.0), (detail_x, 0.0, 1.5), lens=45.0)
    blender_io.render_to(os.path.join(output_dir, "renders", style_name + "_facade_detail.png"), 1600, 1200)


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    output_dir = arguments[0]
    wanted = arguments[1:] or list(styles.STYLES.keys())
    for style_name in wanted:
        render_house(style_name, output_dir)
        print("ASSEMBLED", style_name)


main()
