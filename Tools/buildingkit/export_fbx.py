"""Converts the kit GLBs to FBX for the Unreal import (Scripts/import_building_kit.py).

  blender -b --factory-startup -P Tools/buildingkit/export_fbx.py -- <building_kit dir>

Reads <dir>/glb/<style>/<style>_<Piece>.glb and writes <dir>/fbx/SM_<style>_<Piece>.fbx, one mesh per file, in
the kit's own frame (metres, Z up, wall face on Y = 0) with the same axis settings as the street furniture, so the
Unreal side can check the orientation once. Material slots keep the kit's slot names (Brick, Frame, Glass ...).
The structure families (turbines, pylons, petrol ... from generate_structures.py) go to
<dir>/fbx/structures/<family>/ instead, for Scripts/import_structures.py.
"""
import glob
import os
import sys

import bpy

# The building styles of kit/styles.py; every other family in glb/ is a structure family.
STYLES = ("brick", "plaster", "block", "farm")


def export_piece(glb_path, fbx_dir):
    """Imports one GLB into an empty scene and exports its mesh as FBX."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=glb_path)
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    if len(meshes) != 1:
        bpy.ops.object.select_all(action="DESELECT")
        for obj in meshes:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    obj = meshes[0]
    obj.parent = None
    obj.matrix_world.identity()
    name = "SM_" + os.path.splitext(os.path.basename(glb_path))[0]
    obj.name = name
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.fbx(filepath=os.path.join(fbx_dir, name + ".fbx"), use_selection=True,
                             object_types={"MESH"}, axis_forward="X", axis_up="Z", global_scale=1.0,
                             apply_unit_scale=True, apply_scale_options="FBX_SCALE_ALL", mesh_smooth_type="EDGE")


def output_dir(root, family):
    """The FBX folder of a family: fbx/ for the building styles, fbx/structures/<family>/ for the rest."""
    if family in STYLES:
        path = os.path.join(root, "fbx")
    else:
        path = os.path.join(root, "fbx", "structures", family)
    os.makedirs(path, exist_ok=True)
    return path


def main():
    root = sys.argv[sys.argv.index("--") + 1]
    count = 0
    for path in sorted(glob.glob(os.path.join(root, "glb", "*", "*.glb"))):
        family = os.path.basename(os.path.dirname(path))
        if family == "props":
            continue
        export_piece(path, output_dir(root, family))
        count += 1
    print("exported", count, "pieces")


main()
