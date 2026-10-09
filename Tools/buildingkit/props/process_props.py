"""Step 3 of the prop pipeline, run inside Blender: scale each raw Hunyuan mesh to its measured real size, fix the
orientation and origin, decimate, give it UVs in metres and one named material, export GLB and render a sheet.

    blender -b --factory-startup --python-exit-code 1 -P Tools/buildingkit/props/process_props.py -- <raw_dir> <output_dir> [prop ...]
"""

import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bmesh  # noqa: E402
import bpy  # noqa: E402
from mathutils import Matrix, Vector  # noqa: E402

from kit import blender_io  # noqa: E402
from kit.geom import Mesh  # noqa: E402
from prop_list import PROPS  # noqa: E402

SHARP_ANGLE_DEGREES = 35.0


def import_raw_mesh(path):
    """Imports a raw Hunyuan GLB and returns its single mesh object with the import transform applied."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=path)
    imported = [obj for obj in bpy.data.objects if obj not in before]
    meshes = [obj for obj in imported if obj.type == "MESH"]
    bpy.ops.object.select_all(action="DESELECT")
    for obj in meshes:
        obj.parent = None
        obj.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for obj in imported:
        if obj.type != "MESH":
            bpy.data.objects.remove(obj)
    return meshes[0]


def rotate_about_z(obj, degrees):
    """Rotates the mesh data about the world Z axis through the origin."""
    if abs(degrees) < 1e-6:
        return
    matrix = Matrix.Rotation(math.radians(degrees), 4, "Z")
    obj.data.transform(matrix)
    obj.data.update()


def bounding_box(obj):
    """Returns (minimum, maximum) corners of the mesh in object space, read from the vertices because Blender's
    cached bound_box is stale right after mesh.transform()."""
    coordinates = [vertex.co for vertex in obj.data.vertices]
    minimum = Vector((min(c[i] for c in coordinates) for i in range(3)))
    maximum = Vector((max(c[i] for c in coordinates) for i in range(3)))
    return minimum, maximum


def scale_to_size(obj, size):
    """Scales non-uniformly so the bounding box matches the measured width, depth and height."""
    minimum, maximum = bounding_box(obj)
    extent = maximum - minimum
    factors = (size[0] / extent[0], size[1] / extent[1], size[2] / extent[2])
    obj.data.transform(Matrix.Diagonal((*factors, 1.0)))
    obj.data.update()
    return factors


def place_origin(obj, mount):
    """Ground props get the origin at the bottom centre; wall props at the bottom-left of the wall face, with the
    back plane on Y = 0 and the body extending toward -Y."""
    minimum, maximum = bounding_box(obj)
    if mount == "ground":
        offset = Vector((-(minimum.x + maximum.x) / 2.0, -(minimum.y + maximum.y) / 2.0, -minimum.z))
    else:
        offset = Vector((-minimum.x, -maximum.y, -minimum.z))
    obj.data.transform(Matrix.Translation(offset))
    obj.data.update()


def decimate(obj, target_triangles):
    """Rebuilds clean topology, then collapse-decimates to roughly the target triangle count.

    Hunyuan's surface-net output has many non-manifold edges, so the decimate modifier stalls on it; a voxel remesh
    (about 300 voxels along the longest side) turns it into a manifold surface first."""
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    minimum, maximum = bounding_box(obj)
    obj.data.remesh_voxel_size = max(maximum - minimum) / 300.0
    bpy.ops.object.voxel_remesh()
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()
    current = len(obj.data.polygons)
    if current <= target_triangles:
        return
    modifier = obj.modifiers.new("Decimate", "DECIMATE")
    modifier.ratio = target_triangles / current
    bpy.ops.object.modifier_apply(modifier=modifier.name)


def shade_with_sharp_edges(obj):
    """Smooth shading, but edges sharper than SHARP_ANGLE_DEGREES stay creased."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    threshold = math.radians(SHARP_ANGLE_DEGREES)
    for edge in bm.edges:
        edge.smooth = True
        if len(edge.link_faces) == 2 and edge.calc_face_angle(0.0) > threshold:
            edge.smooth = False
    for face in bm.faces:
        face.smooth = True
    bm.to_mesh(obj.data)
    bm.free()


def project_metre_uvs(obj):
    """Box-projects UVs at one UV unit per metre, since the geometry is already at real scale."""
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.cube_project(cube_size=1.0)
    bpy.ops.object.mode_set(mode="OBJECT")


def assign_material(obj, material_name):
    """Replaces whatever material the import brought with the single named slot."""
    obj.data.materials.clear()
    if material_name not in blender_io.PREVIEW_COLOURS:
        blender_io.PREVIEW_COLOURS[material_name] = (0.15, 0.17, 0.20, 1.0)
    obj.data.materials.append(blender_io.get_material(material_name))


def process_prop(name, prop, raw_dir, output_dir):
    """Runs the whole clean-up for one prop and returns the finished object."""
    obj = import_raw_mesh(os.path.join(raw_dir, name + ".glb"))
    obj.name = "prop_" + name
    obj.data.name = "prop_" + name
    rotate_about_z(obj, prop.get("rotate_z", 0.0))
    decimate(obj, prop["triangles"])
    scale_to_size(obj, prop["size"])
    place_origin(obj, prop["mount"])
    assign_material(obj, prop["material"])
    shade_with_sharp_edges(obj)
    project_metre_uvs(obj)
    obj.location = (0.0, 0.0, 0.0)
    glb_path = os.path.join(output_dir, "glb", "props", name + ".glb")
    blender_io.export_glb(obj, glb_path)
    print("PROP %s: %d triangles, size %s" % (name, len(obj.data.polygons), [round(v, 3) for v in prop["size"]]))
    return obj


def render_sheet(objects, output_path):
    """Props in a row next to a 1.8 m reference box; wall props stand in front of a plain wall panel."""
    blender_io.setup_world(sun_strength=2.6, neutral=True)
    blender_io.add_ground(size=100.0, z=-0.002, colour=(0.22, 0.22, 0.21, 1.0))
    spacing = 2.6
    reference = Mesh()
    reference.box(-0.25, 0.25, -0.15, 0.15, 0.0, 1.8, "Metal")
    blender_io.add_piece_object(bpy.context.scene.collection, "Reference_1.8m", reference, (0.0, 0.0, 0.0))
    blender_io.add_label("1.8 m reference", (0.0, -0.9, 0.01), size=0.16)
    cursor = 1.8
    wall = Mesh()
    for name, obj in objects.items():
        prop = PROPS[name]
        minimum, maximum = bounding_box(obj)
        width = maximum.x - minimum.x
        if prop["mount"] == "ground":
            obj.location = (cursor + width / 2.0, 0.0, 0.0)
        else:
            obj.location = (cursor, 0.0, 0.0)
            wall.box(cursor - 0.3, cursor + width + 0.3, 0.0, 0.2, 0.0, maximum.z + 0.4, "Concrete")
        blender_io.add_label(name, (cursor + width / 2.0, -0.9, 0.01), size=0.16)
        cursor += max(width, 0.9) + 0.9
    blender_io.add_piece_object(bpy.context.scene.collection, "WallPanels", wall, (0.0, 0.0, 0.0))
    center_x = cursor / 2.0
    blender_io.add_camera((center_x - 0.3, -cursor * 0.9 - 3.0, 2.2), (center_x, 0.0, 1.2), lens=30.0)
    blender_io.render_to(output_path, 2400, 900)


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    raw_dir, output_dir = arguments[0], arguments[1]
    wanted = arguments[2:] or list(PROPS.keys())
    blender_io.reset_scene()
    objects = {}
    for name in wanted:
        objects[name] = process_prop(name, PROPS[name], raw_dir, output_dir)
    blend_path = os.path.join(output_dir, "blend", "props.blend")
    os.makedirs(os.path.dirname(blend_path), exist_ok=True)
    cursor = 0.0
    for obj in objects.values():
        minimum, maximum = bounding_box(obj)
        obj.location = (cursor, 0.0, 0.0)
        cursor += maximum.x - minimum.x + 1.0
    bpy.ops.wm.save_as_mainfile(filepath=blend_path)
    for obj in objects.values():
        obj.location = (0.0, 0.0, 0.0)
    render_sheet(objects, os.path.join(output_dir, "renders", "props_sheet.png"))


main()
