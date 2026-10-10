"""Builds the structures that stand on OSM nodes rather than footprints, for now the wind turbines (#100): one GLB per
piece, one .blend with every model assembled, and a contact sheet of the line-up plus close-ups of the heads.

Run inside Blender:
    blender -b --factory-startup --python-exit-code 1 -P Tools/buildingkit/generate_structures.py -- <output_dir>
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy  # noqa: E402
from mathutils import Vector  # noqa: E402

from kit import blender_io, turbines  # noqa: E402
from kit.geom import Mesh  # noqa: E402

FAMILY = "turbines"
LINEUP_GAP = 15.0  # metres between the swept discs of neighbouring rotors
HEAD_TILE = (1300, 900)
# Each model gets a different blade angle in the line-up, so the sheet shows the rotor in several positions.
ROTOR_ANGLES = {"MM100": 0.0, "N117": 35.0, "E92": 75.0, "NM48": 20.0}
MODEL_TITLES = {
    "MM100": "Senvion MM100",
    "N117": "Nordex N117",
    "E92": "Enercon E-92",
    "NM48": "NEG Micon NM48",
}


def export_pieces(pieces, output_dir):
    """Exports every piece on its own at the origin."""
    blender_io.reset_scene()
    collection = bpy.context.scene.collection
    for name, kit_mesh in pieces.items():
        full_name = "%s_%s" % (FAMILY, name)
        obj = blender_io.add_piece_object(collection, full_name, kit_mesh)
        blender_io.export_glb(obj, os.path.join(output_dir, "glb", FAMILY, full_name + ".glb"))


def piece_stats(pieces):
    """Triangle count, materials and size of every piece, for the stats file."""
    stats = {}
    for name, kit_mesh in pieces.items():
        lower, upper = kit_mesh.bounds()
        stats[name] = {
            "triangles": kit_mesh.triangle_count(),
            "materials": kit_mesh.materials_used(),
            "size": [round(upper[i] - lower[i], 3) for i in range(3)],
            "min": [round(value, 3) for value in lower],
        }
    return stats


def assemble_turbine(collection, model, mesh_data, x, rotor_angle):
    """Places tower, nacelle and rotor of one model with the sockets from the spec; returns the three objects."""
    name = model.name
    tower = bpy.data.objects.new(name + "_Tower", mesh_data[name + "_Tower"])
    tower.location = (x, 0.0, 0.0)
    nacelle = bpy.data.objects.new(name + "_Nacelle", mesh_data[name + "_Nacelle"])
    nacelle.parent = tower
    nacelle.location = (0.0, 0.0, model.tower_height)
    rotor = bpy.data.objects.new(name + "_Rotor", mesh_data[name + "_Rotor"])
    rotor.parent = nacelle
    rotor.location = (0.0, -model.overhang, model.axis_height)
    # Spin about the rotor's own axis first, then tilt the front end up.
    rotor.rotation_mode = "YXZ"
    rotor.rotation_euler = (math.radians(-model.tilt_degrees), math.radians(rotor_angle), 0.0)
    for obj in (tower, nacelle, rotor):
        collection.objects.link(obj)
    return [tower, nacelle, rotor]


def build_scale_house(collection, x):
    """A plain two-storey house with a gabled roof (10 by 8 m, ridge at 10 m) to show the scale."""
    house = Mesh()
    house.box(-5.0, 5.0, -4.0, 4.0, 0.0, 6.0, "Brick")
    house.prism([(-4.6, 5.9), (4.6, 5.9), (0.0, 10.0)], "yz", -5.4, 5.4, "RoofTile")
    blender_io.add_piece_object(collection, "ScaleHouse", house, location=(x, 0.0, 0.0))


def lineup_positions():
    """X position of every model, side by side with LINEUP_GAP between the rotor discs."""
    positions = {}
    x = 0.0
    previous_radius = None
    for model in turbines.MODELS.values():
        if previous_radius is not None:
            x += previous_radius + model.rotor_radius + LINEUP_GAP
        positions[model.name] = x
        previous_radius = model.rotor_radius
    return positions


def build_lineup(pieces):
    """Assembles every model in a row with a house for scale and labels in a fresh scene; returns the objects per
    model and the extent of the row."""
    blender_io.reset_scene()
    blender_io.setup_world(sun_strength=3.0, neutral=True)
    collection = bpy.context.scene.collection
    mesh_data = {name: blender_io.build_mesh_data(name, kit_mesh) for name, kit_mesh in pieces.items()}
    positions = lineup_positions()
    objects = {}
    for model in turbines.MODELS.values():
        x = positions[model.name]
        objects[model.name] = assemble_turbine(collection, model, mesh_data, x, ROTOR_ANGLES[model.name])
        label = "%s\nhub %.0f m, rotor %.0f m" % (MODEL_TITLES[model.name], model.hub_height, model.rotor_diameter)
        blender_io.add_label(label, (x, -2.0, -9.0), size=4.2)
    last = list(turbines.MODELS.values())[-1]
    house_x = positions[last.name] + last.rotor_radius + 12.0
    build_scale_house(collection, house_x)
    blender_io.add_label("house\n10 m", (house_x, -2.0, -9.0), size=4.2)
    first = list(turbines.MODELS.values())[0]
    left = positions[first.name] - first.rotor_radius - 8.0
    right = house_x + 14.0
    return objects, left, right


def render_lineup(left, right, output_path):
    """Front view of the row, turned a little so the nacelles show their length."""
    top = max(model.hub_height + model.rotor_radius for model in turbines.MODELS.values()) + 6.0
    bottom = -22.0
    width = right - left
    centre = ((left + right) / 2.0, 0.0, (top + bottom) / 2.0)
    yaw = math.radians(14)
    distance = 900.0
    camera_location = (centre[0] - math.sin(yaw) * distance, -math.cos(yaw) * distance, centre[2] + 40.0)
    blender_io.add_camera(camera_location, centre, orthographic_scale=width * 1.02, clip_end=3000.0)
    pixel_width = 2600
    blender_io.render_to(output_path, pixel_width, int(pixel_width * (top - bottom) / (width * 1.02)))


def hub_location(model, x):
    """World position of the hub centre of a model standing at x."""
    return Vector((x, -model.overhang, model.hub_height))


def hide_lineup_extras():
    """Hides the scale house and the line-up labels, which would show in the background of the close-ups."""
    for obj in bpy.data.objects:
        if obj.name == "ScaleHouse" or obj.name.startswith("label_"):
            obj.hide_render = True


def render_head(model, x, objects, output_path):
    """Close-up of one nacelle and hub from the side, slightly behind and above the hub, with one blade pointing up
    so the other two pass below the nacelle."""
    for name, model_objects in objects.items():
        for obj in model_objects:
            obj.hide_render = name != model.name
    rotor = objects[model.name][2]
    rotor.rotation_euler = (math.radians(-model.tilt_degrees), 0.0, 0.0)
    hub = hub_location(model, x)
    target = hub + Vector((0.0, model.nacelle_length * 0.35, -model.nacelle_height * 0.15))
    direction = Vector((-1.0, 0.4, 0.15)).normalized()
    distance = model.nacelle_length * 2.3
    camera = blender_io.add_camera(tuple(target + direction * distance), tuple(target), lens=40.0, clip_end=3000.0)
    caption = blender_io.add_label(MODEL_TITLES[model.name], (0.0, -0.27, -1.0), size=0.035, rotation=(0.0, 0.0, 0.0))
    caption.parent = camera
    blender_io.render_to(output_path, *HEAD_TILE)
    bpy.data.objects.remove(caption)
    bpy.data.objects.remove(camera)


def stitch_grid(tile_paths, columns, output_path):
    """Combines equally sized PNG tiles into one image, row by row from the top left."""
    tile_width, tile_height = HEAD_TILE
    rows = math.ceil(len(tile_paths) / columns)
    sheet = numpy.zeros((rows * tile_height, columns * tile_width, 4), dtype=numpy.float32)
    for index, path in enumerate(tile_paths):
        image = bpy.data.images.load(path)
        pixels = numpy.empty(tile_width * tile_height * 4, dtype=numpy.float32)
        image.pixels.foreach_get(pixels)
        row = rows - 1 - index // columns  # Blender images start at the bottom row
        column = index % columns
        sheet[row * tile_height:(row + 1) * tile_height, column * tile_width:(column + 1) * tile_width] = (
            pixels.reshape(tile_height, tile_width, 4))
        bpy.data.images.remove(image)
    result = bpy.data.images.new("sheet", columns * tile_width, rows * tile_height, alpha=True)
    result.pixels.foreach_set(sheet.ravel())
    result.filepath_raw = output_path
    result.file_format = "PNG"
    result.save()


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    output_dir = arguments[0]
    pieces, spec = turbines.build_turbines()
    export_pieces(pieces, output_dir)
    objects, left, right = build_lineup(pieces)
    blend_path = os.path.join(output_dir, "blend", FAMILY + ".blend")
    os.makedirs(os.path.dirname(blend_path), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    renders = os.path.join(output_dir, "renders")
    render_lineup(left, right, os.path.join(renders, FAMILY + "_lineup.png"))
    hide_lineup_extras()
    positions = lineup_positions()
    tile_paths = []
    for model in turbines.MODELS.values():
        path = os.path.join(renders, "%s_head_%s.png" % (FAMILY, model.name))
        render_head(model, positions[model.name], objects, path)
        tile_paths.append(path)
    stitch_grid(tile_paths, 2, os.path.join(renders, FAMILY + "_heads.png"))
    with open(os.path.join(output_dir, "kit_stats_%s.json" % FAMILY), "w") as handle:
        json.dump({FAMILY: {"spec": spec, "pieces": piece_stats(pieces)}}, handle, indent=1)
    total = sum(kit_mesh.triangle_count() for kit_mesh in pieces.values())
    print("FAMILY %s: %d pieces, %d triangles" % (FAMILY, len(pieces), total))


main()
