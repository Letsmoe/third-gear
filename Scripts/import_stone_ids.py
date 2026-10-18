"""Imports the stone id maps (Tools/texturegen/stone_ids.py, data root texturegen/stone_ids/) into
/Game/Textures/Generated/stone_ids/ as T_StoneIds_<Set>: uncompressed, linear, no mips and nearest filtering, since the
pixels are ids and must not be blended.

Run headless (editor closed), holding the gpu lock:
  Scripts/lock.sh gpu UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import glob
import os
import sys

import unreal

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(PROJECT_DIR, "Tools", "bootstrap"))
import data_root  # noqa: E402

SOURCE_DIR = os.path.join(data_root.data_root(), "texturegen", "stone_ids")
DESTINATION = "/Game/Textures/Generated/stone_ids"


def import_texture(path):
    """Imports one id map PNG and applies the settings that keep its ids exact."""
    name = os.path.splitext(os.path.basename(path))[0]
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", DESTINATION)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.EditorAssetLibrary.load_asset(f"{DESTINATION}/{name}")
    if not isinstance(texture, unreal.Texture2D):
        unreal.log_error(f"import_stone_ids: {name} failed")
        return
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    texture.set_editor_property("srgb", False)
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("filter", unreal.TextureFilter.TF_NEAREST)
    texture.set_editor_property("never_stream", True)
    unreal.EditorAssetLibrary.save_loaded_asset(texture)
    unreal.log_warning(f"import_stone_ids: {name} done")


for source_path in sorted(glob.glob(os.path.join(SOURCE_DIR, "T_StoneIds_*.png"))):
    import_texture(source_path)
