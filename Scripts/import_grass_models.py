"""Imports the Poly Haven grass clumps (CC0, glTF) into /Game/Grass/Imported.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/import_grass_models.py -unattended -nosplash

The glTF files are in <data root>/raw_assets/polyhaven (see Data/raw_assets.md); each holds several tufts of different
shape as separate meshes.
"""
import os

import unreal

SOURCES = ["grass_medium_01", "grass_medium_02"]
DESTINATION = "/Game/Grass/Imported"


def raw_assets_dir():
    """The polyhaven folder in the data root."""
    root = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
    return os.path.join(root, "raw_assets", "polyhaven")


def import_source(name):
    """Imports one glTF with Interchange into its own folder."""
    path = os.path.join(raw_assets_dir(), name, f"{name}_2k.gltf")
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", f"{DESTINATION}/{name}")
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)
    task.set_editor_property("replace_existing", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    for imported in task.get_editor_property("imported_object_paths"):
        unreal.log_warning(f"grass import: {imported}")


for source in SOURCES:
    import_source(source)
