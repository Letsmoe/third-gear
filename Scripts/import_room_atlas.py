"""Imports the window interior atlas (Tools/buildingkit/rooms/run_rooms.sh) as /Game/World/Windows/T_RoomAtlas.

  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/import_room_atlas.py -unattended -nosplash

Run create_window_materials.py afterwards so M_WindowGlass picks it up.
"""
import unreal

SOURCE = "/mnt/storage/third-gear/building_kit/rooms/room_atlas.png"

task = unreal.AssetImportTask()
task.set_editor_property("filename", SOURCE)
task.set_editor_property("destination_path", "/Game/World/Windows")
task.set_editor_property("destination_name", "T_RoomAtlas")
task.set_editor_property("replace_existing", True)
task.set_editor_property("automated", True)
task.set_editor_property("save", True)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
texture = unreal.load_asset("/Game/World/Windows/T_RoomAtlas")
texture.set_editor_property("srgb", True)
texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP)
texture.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
texture.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
unreal.EditorAssetLibrary.save_loaded_asset(texture)
unreal.log_warning("import_room_atlas: done")
