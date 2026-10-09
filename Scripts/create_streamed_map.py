"""Creates /Game/Maps/Streamed: lighting plus one AWorldStreamer, which generates the world at runtime from the
compiled world data (Tools/osmimport/build_world.py). Nothing of the world itself is saved in the map.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script="Scripts/create_streamed_map.py [region]" -unattended -nosplash

The region (default bergedorf_test) can also be chosen when playing, with -Region=<name>.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import world_lighting  # noqa: E402

MAP = "/Game/Maps/Streamed"

args = [a for a in sys.argv[1:] if not a.startswith("-")]
region = args[0] if args else "bergedorf_test"
level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
if unreal.EditorAssetLibrary.does_asset_exist(MAP):
    level_subsystem.load_level(MAP)
    for actor in actor_subsystem.get_all_level_actors():
        actor_subsystem.destroy_actor(actor)
else:
    level_subsystem.new_level(MAP)
world_lighting.setup(actor_subsystem)
streamer = actor_subsystem.spawn_actor_from_class(unreal.WorldStreamer, unreal.Vector(0, 0, 0))
streamer.set_actor_label("WorldStreamer")
streamer.set_editor_property("region", region)
level_subsystem.save_current_level()
unreal.log_warning(f"create_streamed_map: {MAP} streams {region}")
