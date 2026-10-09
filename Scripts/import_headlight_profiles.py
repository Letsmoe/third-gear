"""Imports the headlamp IES files as light profile assets in /Game/Vehicles/Lights (headless editor script).

Generate the files first with Tools/texturegen/headlamp_ies.py. Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import os

import unreal

DESTINATION = "/Game/Vehicles/Lights"
DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
SOURCE_DIR = os.path.join(DATA_ROOT, "texturegen", "headlamps")
PROFILES = {"headlamp_low.ies": "IES_LowBeam", "headlamp_high.ies": "IES_HighBeam"}


def import_profile(file_name, asset_name):
    """Imports one .ies file, replacing the asset of the same name."""
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", os.path.join(SOURCE_DIR, file_name))
    task.set_editor_property("destination_path", DESTINATION)
    task.set_editor_property("destination_name", asset_name)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(f"{DESTINATION}/{asset_name}")
    unreal.log(f"HEADLIGHT_IES {asset_name}: {asset.get_class().get_name() if asset else 'missing'}")


for source_name, target_name in PROFILES.items():
    import_profile(source_name, target_name)
