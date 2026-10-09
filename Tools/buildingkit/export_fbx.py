"""Converts the kit GLBs to FBX for the Unreal import (Scripts/import_building_kit.py).

  blender -b --factory-startup -P Tools/buildingkit/export_fbx.py -- <building_kit dir>

Reads <dir>/glb/<style>/<style>_<Piece>.glb and writes <dir>/fbx/SM_<style>_<Piece>.fbx, one mesh per file, in
the kit's own frame (metres, Z up, wall face on Y = 0) with the same axis settings as the street furniture, so the
Unreal side can check the orientation once. Material slots keep the kit's slot names (Brick, Frame, Glass ...).
"""
import glob
import os
import sys

import bpy


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


def main():
    root = sys.argv[sys.argv.index("--") + 1]
    fbx_dir = os.path.join(root, "fbx")
    os.makedirs(fbx_dir, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(root, "glb", "*", "*.glb"))):
        if "/props/" in path:
            continue
        export_piece(path, fbx_dir)
    print("exported", len(os.listdir(fbx_dir)), "pieces")


main()
