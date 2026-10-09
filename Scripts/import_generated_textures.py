"""Imports the ComfyUI-generated texture sets (Tools/texturegen, data root texturegen/<name>/) into
/Game/Textures/Generated/<name>/ as T_<name>_<Map>, with the same compression and sRGB settings as import_textures.py.

Run headless (editor closed), holding the gpu lock:
  Scripts/lock.sh gpu UnrealEditor-Cmd <project>.uproject -run=pythonscript -script="<this file> [-only=a,b]" -unattended -nosplash

Normals come out of CHORD in OpenGL convention (green up), so the import flips green for Unreal.
Re-running re-imports and re-applies the settings.
"""
import os
import sys

import unreal

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(PROJECT_DIR, "Tools", "bootstrap"))
import data_root  # noqa: E402

SOURCE_ROOT = os.path.join(data_root.data_root(), "texturegen")
DEST_ROOT = "/Game/Textures/Generated"
MAPS = ["BaseColor", "Normal", "Roughness", "AO", "Height"]

CS = unreal.TextureCompressionSettings
TG = unreal.TextureGroup
# map -> (compression, srgb, lod group)
TEXTURE_SETTINGS = {
    "BaseColor": (CS.TC_DEFAULT, True, TG.TEXTUREGROUP_WORLD),
    "Normal": (CS.TC_NORMALMAP, False, TG.TEXTUREGROUP_WORLD_NORMAL_MAP),
    "Roughness": (CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
    "AO": (CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
    "Height": (CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
}


def generated_set_names():
    """Names of the finished sets in the data root (folders holding a base colour map; folders starting with _ are scratch)."""
    names = []
    for entry in sorted(os.listdir(SOURCE_ROOT)):
        if entry.startswith("_"):
            continue
        if os.path.exists(os.path.join(SOURCE_ROOT, entry, f"T_{entry}_BaseColor.png")):
            names.append(entry)
    return names


def import_set(set_name):
    """Imports the five maps of one set and applies the texture settings; returns the maps that failed."""
    destination = f"{DEST_ROOT}/{set_name}"
    tasks = []
    for map_name in MAPS:
        path = os.path.join(SOURCE_ROOT, set_name, f"T_{set_name}_{map_name}.png")
        if not os.path.exists(path):
            unreal.log_warning(f"[import_generated_textures] {set_name}: no {map_name} map")
            continue
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", path)
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("destination_name", f"T_{set_name}_{map_name}")
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("replace_existing_settings", True)
        task.set_editor_property("save", True)
        tasks.append((map_name, task))
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task for _, task in tasks])

    failed = []
    for map_name, _ in tasks:
        texture = unreal.EditorAssetLibrary.load_asset(f"{destination}/T_{set_name}_{map_name}")
        if not isinstance(texture, unreal.Texture2D):
            failed.append(map_name)
            continue
        compression, srgb, lod_group = TEXTURE_SETTINGS[map_name]
        texture.set_editor_property("compression_settings", compression)
        texture.set_editor_property("srgb", srgb)
        texture.set_editor_property("lod_group", lod_group)
        if map_name == "Normal":
            texture.set_editor_property("flip_green_channel", True)
        unreal.EditorAssetLibrary.save_loaded_asset(texture)
    return failed


def main():
    only = next((a.split("=", 1)[1].split(",") for a in sys.argv[1:] if a.startswith("-only=")), None)
    for set_name in generated_set_names():
        if only and set_name not in only:
            continue
        failed = import_set(set_name)
        unreal.log(f"[import_generated_textures] {set_name}: failed maps {failed}")


main()
