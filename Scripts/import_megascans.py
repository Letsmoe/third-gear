"""Imports the downloaded Megascans texture sets (Fab content, data root raw_assets/megascans/<name>_<uid>/) into
/Game/Megascans/Textures/<folder>/T_<folder>_<Map>, the folder linked as Content/Megascans. Nothing in there is ever committed.

Run headless (editor closed):
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script="<this file> [-only=folder1,folder2] [-group=facade|weathering]" -unattended -nosplash

Quixel exports Displacement for height and OpenGL normals (green up), so the normal maps get the green channel flipped.
Re-running re-imports and re-applies the settings.
"""
import glob
import os
import sys

import unreal

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(PROJECT_DIR, "Tools", "bootstrap"))
import data_root  # noqa: E402

RAW_DIR = os.path.join(data_root.raw_assets_dir(), "megascans")
DEST_ROOT = "/Game/Megascans/Textures"

# Folder name in raw_assets/megascans -> group. "facade" sets are the wall, roof and metal surfaces of
# create_facade_materials.py, "weathering" sets are the grime, patch, moss and flaked-paint scans of the overlay (#84).
SETS = {
    "brick_facade_56f913a0": "facade",
    "brick_facade_efa35b96": "facade",
    "brick_wall_5d318a8f": "facade",
    "brick_wall_e2865ee3": "facade",
    "brick_wall_worn_499d9ba7": "facade",
    "stucco_wall_e2f69285": "facade",
    "stucco_wall_8e1150b6": "facade",
    "stucco_facade_19ea388c": "facade",
    "stucco_facade_a78c4b5c": "facade",
    "stucco_facade_ec540029": "facade",
    "wall_paint_cfdcd26b": "facade",
    "smooth_concrete_410ea2a8": "facade",
    "smooth_concrete_68db59f6": "facade",
    "weathered_concrete_wall_9485cd8b": "facade",
    "corrugated_metal_sheet_109b6f59": "facade",
    "dirty_corrugated_metal_sheet_5b15681e": "facade",
    "painted_wooden_planks_71b1478e": "facade",
    "painted_roof_764bd55b": "facade",
    "damaged_wall_plaster_c16498bb": "weathering",
    "flaked_paint_wall_42731ac6": "weathering",
    "flaked_paint_wall_a5b52c81": "weathering",
    "flaked_paint_wall_b718e7f1": "weathering",
    "concrete_damaged_3747b730": "weathering",
}

CS = unreal.TextureCompressionSettings
TG = unreal.TextureGroup
# map -> (file suffix, compression, srgb, lod group)
MAPS = {
    "BaseColor": ("BaseColor", CS.TC_DEFAULT, True, TG.TEXTUREGROUP_WORLD),
    "Normal": ("Normal", CS.TC_NORMALMAP, False, TG.TEXTUREGROUP_WORLD_NORMAL_MAP),
    "Roughness": ("Roughness", CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
    "AO": ("AO", CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
    "Height": ("Displacement", CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
}


def find_map_file(folder, suffix):
    """Path of the 4K jpg of one map in a set's folder, or None."""
    hits = sorted(glob.glob(os.path.join(RAW_DIR, folder, f"*_4K_{suffix}.jpg")))
    return hits[0] if hits else None


def import_set(folder):
    """Imports one set; returns the maps that failed."""
    destination = f"{DEST_ROOT}/{folder}"
    jobs = []
    for map_name, (suffix, _, _, _) in MAPS.items():
        path = find_map_file(folder, suffix)
        if path is None:
            unreal.log_warning(f"[import_megascans] {folder}: no {map_name} map")
            continue
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", path)
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("destination_name", f"T_{folder}_{map_name}")
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("replace_existing_settings", True)
        task.set_editor_property("save", True)
        jobs.append((map_name, task))
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task for _, task in jobs])

    failed = []
    for map_name, task in jobs:
        asset_path = f"{destination}/T_{folder}_{map_name}"
        texture = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not isinstance(texture, unreal.Texture2D):
            unreal.log_error(f"[import_megascans] FAILED to import {asset_path}")
            failed.append(map_name)
            continue
        _, compression, srgb, lod_group = MAPS[map_name]
        texture.set_editor_property("compression_settings", compression)
        texture.set_editor_property("srgb", srgb)
        texture.set_editor_property("lod_group", lod_group)
        if map_name == "Normal":
            texture.set_editor_property("flip_green_channel", True)
        unreal.EditorAssetLibrary.save_loaded_asset(texture)
    return failed


def main():
    only = next((a.split("=", 1)[1].split(",") for a in sys.argv[1:] if a.startswith("-only=")), None)
    group = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("-group=")), None)
    failures = []
    for folder, set_group in SETS.items():
        if (only and folder not in only) or (group and set_group != group):
            continue
        if not os.path.isdir(os.path.join(RAW_DIR, folder)):
            unreal.log_warning(f"[import_megascans] {folder} is not downloaded, skipped")
            continue
        unreal.log(f"[import_megascans] {folder}")
        failures.extend((folder, name) for name in import_set(folder))
    unreal.log(f"[import_megascans] done, {len(failures)} failures {failures}")


main()
