"""Imports the generated ambience loops and thunder one-shots from the data root (audio/loops, audio/oneshots, made by
Tools/audiogen/run_audiogen.sh) into /Game/Audio/Loops and /Game/Audio/OneShots, with looping switched on for the loops.

Run headless (editor closed): Scripts/lock.sh gpu UnrealEditor-Cmd <project>.uproject -run=pythonscript
  -script=Scripts/import_audio.py -unattended -nosplash
Re-running re-imports and replaces.
"""
import glob
import os

import unreal

AUDIO_ROOT = os.environ.get("AUDIO_ROOT", "/mnt/storage/third-gear/audio")
FOLDERS = [("loops", "/Game/Audio/Loops", True), ("oneshots", "/Game/Audio/OneShots", False)]


def import_folder(source_folder, destination, looping):
    """Imports every WAV of source_folder and sets looping on the resulting sound waves."""
    files = sorted(glob.glob(os.path.join(AUDIO_ROOT, source_folder, "*.wav")))
    tasks = []
    for path in files:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", path)
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("save", True)
        tasks.append(task)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
    for path in files:
        name = os.path.splitext(os.path.basename(path))[0]
        sound = unreal.load_asset(f"{destination}/{name}")
        if sound is None:
            unreal.log_error(f"AUDIOIMPORT {name} failed")
            continue
        sound.set_editor_property("looping", looping)
        unreal.EditorAssetLibrary.save_loaded_asset(sound)
        unreal.log(f"AUDIOIMPORT {destination}/{name} looping={looping}")


for folder, destination, looping in FOLDERS:
    import_folder(folder, destination, looping)
