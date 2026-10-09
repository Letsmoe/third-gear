"""Builds every kit piece of the chosen styles, exports one GLB per piece, saves one .blend per style and renders a
contact sheet per style.

Run inside Blender:
    blender -b --factory-startup --python-exit-code 1 -P Tools/buildingkit/generate.py -- <output_dir> [style ...]
"""

import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bpy  # noqa: E402

from kit import blender_io, styles  # noqa: E402

ROW_WIDTH = 24.0
LABEL_HEIGHT = 0.9
CELL_MARGIN = 0.7


def layout_shelf(pieces):
    """Packs the pieces into rows for a contact sheet; returns name -> (x, z, width) of the bounding-box corner."""
    placements = {}
    cursor_x = 0.0
    row_top = 0.0
    row_height = 0.0
    for name, kit_mesh in pieces.items():
        (min_x, _min_y, min_z), (max_x, _max_y, max_z) = kit_mesh.bounds()
        width = max(max_x - min_x, len(name) * 0.16)
        height = max_z - min_z
        if cursor_x > 0.0 and cursor_x + width > ROW_WIDTH:
            row_top -= row_height + LABEL_HEIGHT + CELL_MARGIN
            cursor_x = 0.0
            row_height = 0.0
        placements[name] = (cursor_x, row_top - height, width)
        cursor_x += width + CELL_MARGIN
        row_height = max(row_height, height)
    return placements


def render_contact_sheet(style_name, pieces, output_path):
    """Lays all pieces out on one backdrop and renders them with a slightly turned camera."""
    blender_io.reset_scene()
    blender_io.setup_world(sun_strength=2.6, neutral=True)
    collection = bpy.context.scene.collection
    placements = layout_shelf(pieces)
    lowest = 0.0
    widest = 0.0
    for name, kit_mesh in pieces.items():
        corner_x, corner_z, width = placements[name]
        (min_x, min_y, min_z), _upper = kit_mesh.bounds()
        offset = (corner_x - min_x, 0.0, corner_z - min_z)
        blender_io.add_piece_object(collection, name, kit_mesh, location=offset)
        blender_io.add_label(name, (corner_x + width / 2.0, -0.1, corner_z - 0.5), size=0.24)
        lowest = min(lowest, corner_z - 0.9)
        widest = max(widest, corner_x + width)
    center = (widest / 2.0, 0.0, lowest / 2.0 + 0.5)
    span = max(widest, -lowest)
    camera_distance = span * 1.15 + 8.0
    yaw = math.radians(18)
    camera_location = (center[0] - math.sin(yaw) * camera_distance, -math.cos(yaw) * camera_distance, center[2] + 3.0)
    blender_io.add_camera(camera_location, center, lens=50.0, orthographic_scale=widest * 1.04)
    blender_io.render_to(output_path, 2600, int(2600 * (-lowest + 2.0) / (widest * 1.04)))


def build_style(style_name, output_dir):
    """Builds one style: GLB per piece, the .blend, the contact sheet and a stats entry."""
    pieces, spec = styles.STYLES[style_name]()
    blender_io.reset_scene()
    collection = bpy.context.scene.collection
    glb_dir = os.path.join(output_dir, "glb", style_name)
    stats = {}
    objects = {}
    for name, kit_mesh in pieces.items():
        full_name = "%s_%s" % (style_name, name)
        obj = blender_io.add_piece_object(collection, full_name, kit_mesh)
        blender_io.export_glb(obj, os.path.join(glb_dir, full_name + ".glb"))
        objects[name] = obj
        lower, upper = kit_mesh.bounds()
        stats[name] = {
            "triangles": kit_mesh.triangle_count(),
            "materials": kit_mesh.materials_used(),
            "size": [round(upper[i] - lower[i], 3) for i in range(3)],
            "min": [round(v, 3) for v in lower],
        }
    placements = layout_shelf(pieces)
    for name, obj in objects.items():
        corner_x, corner_z, _width = placements[name]
        (min_x, _min_y, min_z), _upper = pieces[name].bounds()
        obj.location = (corner_x - min_x, 0.0, corner_z - min_z)
    blend_path = os.path.join(output_dir, "blend", style_name + ".blend")
    os.makedirs(os.path.dirname(blend_path), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    render_contact_sheet(style_name, pieces, os.path.join(output_dir, "renders", style_name + "_pieces.png"))
    return spec, stats


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    output_dir = arguments[0]
    wanted = arguments[1:] or list(styles.STYLES.keys())
    summary = {}
    for style_name in wanted:
        spec, stats = build_style(style_name, output_dir)
        summary[style_name] = {"spec": spec, "pieces": stats}
        total = sum(entry["triangles"] for entry in stats.values())
        print("STYLE %s: %d pieces, %d triangles" % (style_name, len(stats), total))
    with open(os.path.join(output_dir, "kit_stats_%s.json" % "_".join(wanted)), "w") as handle:
        json.dump(summary, handle, indent=1)


main()
