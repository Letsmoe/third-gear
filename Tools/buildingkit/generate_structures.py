"""Builds the structures that stand on OSM nodes rather than footprints: one GLB per piece, one .blend per family with
every model assembled in a row, and a contact sheet of the line-up plus close-ups.

Run inside Blender:
    blender -b --factory-startup --python-exit-code 1 -P Tools/buildingkit/generate_structures.py -- <output_dir> [family ...]

A family module (kit/turbines.py, kit/pylons.py) provides build(), returning (pieces, spec), and assemblies(), a list
of dicts that say how the sheet shows each model:
- name, title: object prefix and the label under the model (may contain a line break).
- parts: (piece name, location, rotation in degrees, rotation mode) relative to the model's foot.
- half_width, top: the model's extent, for spacing the row and framing the camera.
- extras: a geom.Mesh shown only on the sheet (conductor stubs), or None.
- closeups: dicts with caption, target, direction (from the target toward the camera), distance and lens.
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402
import numpy  # noqa: E402
from mathutils import Vector  # noqa: E402

from kit import blender_io, pylons, turbines  # noqa: E402
from kit.geom import Mesh  # noqa: E402

FAMILIES = {"turbines": turbines, "pylons": pylons}
LINEUP_GAP = 15.0  # metres between neighbouring models
CLOSEUP_TILE = (1300, 900)


def export_pieces(family, pieces, output_dir):
    """Exports every piece on its own at the origin."""
    blender_io.reset_scene()
    collection = bpy.context.scene.collection
    for name, kit_mesh in pieces.items():
        full_name = "%s_%s" % (family, name)
        obj = blender_io.add_piece_object(collection, full_name, kit_mesh)
        blender_io.export_glb(obj, os.path.join(output_dir, "glb", family, full_name + ".glb"))


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


def lineup_positions(models):
    """X position of every model, side by side with LINEUP_GAP between them."""
    positions = []
    x = 0.0
    for index, model in enumerate(models):
        if index > 0:
            x += models[index - 1]["half_width"] + model["half_width"] + LINEUP_GAP
        positions.append(x)
    return positions


def place_model(collection, model, mesh_data, x):
    """Adds the parts of one model at x; returns its objects."""
    objects = []
    for piece_name, location, rotation, mode in model["parts"]:
        obj = bpy.data.objects.new(piece_name, mesh_data[piece_name])
        obj.location = (x + location[0], location[1], location[2])
        obj.rotation_mode = mode
        obj.rotation_euler = tuple(math.radians(angle) for angle in rotation)
        collection.objects.link(obj)
        objects.append(obj)
    if model["extras"] is not None:
        objects.append(blender_io.add_piece_object(collection, model["name"] + "_Preview", model["extras"],
                                                   location=(x, 0.0, 0.0)))
    return objects


def build_scale_house(collection, x):
    """A plain two-storey house with a gabled roof (10 by 8 m, ridge at 10 m) to show the scale."""
    house = Mesh()
    house.box(-5.0, 5.0, -4.0, 4.0, 0.0, 6.0, "Brick")
    house.prism([(-4.6, 5.9), (4.6, 5.9), (0.0, 10.0)], "yz", -5.4, 5.4, "RoofTile")
    blender_io.add_piece_object(collection, "ScaleHouse", house, location=(x, 0.0, 0.0))


def build_lineup(pieces, models, label_size):
    """Assembles every model in a row with a house for scale and labels in a fresh scene; returns the objects per
    model, their positions and the extent of the row."""
    blender_io.reset_scene()
    blender_io.setup_world(sun_strength=3.0, neutral=True)
    collection = bpy.context.scene.collection
    mesh_data = {name: blender_io.build_mesh_data(name, kit_mesh) for name, kit_mesh in pieces.items()}
    positions = lineup_positions(models)
    objects = []
    for model, x in zip(models, positions):
        objects.append(place_model(collection, model, mesh_data, x))
        blender_io.add_label(model["title"], (x, -2.0, -label_size * 2.2), size=label_size)
    house_x = positions[-1] + models[-1]["half_width"] + 12.0
    build_scale_house(collection, house_x)
    blender_io.add_label("house\n10 m", (house_x, -2.0, -label_size * 2.2), size=label_size)
    left = positions[0] - models[0]["half_width"] - 8.0
    right = house_x + 14.0
    return objects, positions, left, right


def render_lineup(models, left, right, label_size, output_path):
    """Front view of the row, turned a little so depth shows."""
    top = max(model["top"] for model in models) * 1.04 + 2.0
    bottom = -label_size * 5.0
    width = right - left
    centre = ((left + right) / 2.0, 0.0, (top + bottom) / 2.0)
    yaw = math.radians(14)
    distance = 900.0
    camera_location = (centre[0] - math.sin(yaw) * distance, -math.cos(yaw) * distance, centre[2] + 40.0)
    camera = blender_io.add_camera(camera_location, centre, orthographic_scale=width * 1.02, clip_end=3000.0)
    pixel_width = 2600
    blender_io.render_to(output_path, pixel_width, int(pixel_width * (top - bottom) / (width * 1.02)))
    bpy.data.objects.remove(camera)


def hide_lineup_extras():
    """Hides the scale house and the line-up labels, which would show in the background of the close-ups."""
    for obj in bpy.data.objects:
        if obj.name == "ScaleHouse" or obj.name.startswith("label_"):
            obj.hide_render = True


def render_closeup(closeup, model_index, x, objects, output_path):
    """Renders one close-up with only its own model visible."""
    for index, model_objects in enumerate(objects):
        for obj in model_objects:
            obj.hide_render = index != model_index
    target = Vector(closeup["target"]) + Vector((x, 0.0, 0.0))
    direction = Vector(closeup["direction"]).normalized()
    camera = blender_io.add_camera(tuple(target + direction * closeup["distance"]), tuple(target),
                                   lens=closeup["lens"], clip_end=3000.0)
    caption = blender_io.add_label(closeup["caption"], (0.0, -0.27, -1.0), size=0.035, rotation=(0.0, 0.0, 0.0))
    caption.parent = camera
    blender_io.render_to(output_path, *CLOSEUP_TILE)
    bpy.data.objects.remove(caption)
    bpy.data.objects.remove(camera)


def stitch_grid(tile_paths, columns, output_path):
    """Combines equally sized PNG tiles into one image, row by row from the top left."""
    tile_width, tile_height = CLOSEUP_TILE
    rows = math.ceil(len(tile_paths) / columns)
    sheet = numpy.full((rows * tile_height, columns * tile_width, 4), 1.0, dtype=numpy.float32)
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


def build_family(family, output_dir):
    """GLBs, .blend, line-up, close-up sheet and stats for one family."""
    module = FAMILIES[family]
    pieces, spec = module.build()
    models = module.assemblies()
    export_pieces(family, pieces, output_dir)
    label_size = max(model["top"] for model in models) * 0.035
    objects, positions, left, right = build_lineup(pieces, models, label_size)
    blend_path = os.path.join(output_dir, "blend", family + ".blend")
    os.makedirs(os.path.dirname(blend_path), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    renders = os.path.join(output_dir, "renders")
    render_lineup(models, left, right, label_size, os.path.join(renders, family + "_lineup.png"))
    hide_lineup_extras()
    tile_paths = []
    for model_index, (model, x) in enumerate(zip(models, positions)):
        for closeup_index, closeup in enumerate(model["closeups"]):
            path = os.path.join(renders, "%s_closeup_%s_%d.png" % (family, model["name"], closeup_index))
            render_closeup(closeup, model_index, x, objects, path)
            tile_paths.append(path)
    stitch_grid(tile_paths, 2, os.path.join(renders, family + "_closeups.png"))
    with open(os.path.join(output_dir, "kit_stats_%s.json" % family), "w") as handle:
        json.dump({family: {"spec": spec, "pieces": piece_stats(pieces)}}, handle, indent=1)
    total = sum(kit_mesh.triangle_count() for kit_mesh in pieces.values())
    print("FAMILY %s: %d pieces, %d triangles" % (family, len(pieces), total))


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    output_dir = arguments[0]
    for family in arguments[1:] or list(FAMILIES.keys()):
        build_family(family, output_dir)


main()
