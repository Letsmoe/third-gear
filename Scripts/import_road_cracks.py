"""Imports the road crack atlas (Tools/texturegen/road_cracks.py, data root texturegen/road_cracks/) into
/Game/Textures/Generated/road_cracks/ as T_RoadCracks_Mask (linear RGBA masks) and T_RoadCracks_Normal.

Run headless (editor closed), holding the gpu lock:
  Scripts/lock.sh gpu UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import os
import sys

import unreal

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(PROJECT_DIR, "Tools", "bootstrap"))
import data_root  # noqa: E402

SOURCE_DIR = os.path.join(data_root.data_root(), "texturegen", "road_cracks")
DESTINATION = "/Game/Textures/Generated/road_cracks"
CS = unreal.TextureCompressionSettings
# name -> (compression, lod group)
TEXTURES = {
    "T_RoadCracks_Mask": (CS.TC_DEFAULT, unreal.TextureGroup.TEXTUREGROUP_WORLD),
    "T_RoadCracks_Normal": (CS.TC_NORMALMAP, unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP),
}


def import_texture(name, compression, lod_group):
    """Imports one PNG and applies its settings; masks and normals are linear."""
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", os.path.join(SOURCE_DIR, f"{name}.png"))
    task.set_editor_property("destination_path", DESTINATION)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.EditorAssetLibrary.load_asset(f"{DESTINATION}/{name}")
    if not isinstance(texture, unreal.Texture2D):
        unreal.log_error(f"import_road_cracks: {name} failed")
        return
    texture.set_editor_property("compression_settings", compression)
    texture.set_editor_property("srgb", False)
    texture.set_editor_property("lod_group", lod_group)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)
    unreal.log_warning(f"import_road_cracks: {name} done")


for texture_name, (texture_compression, texture_group) in TEXTURES.items():
    import_texture(texture_name, texture_compression, texture_group)
