"""Creates RT_SnowTracks, the render target of the tyre track map (Plugins/MapRuntime SnowTrackSubsystem.cpp).

  Scripts/lock.sh gpu $UE/Engine/Binaries/Linux/UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript \
      -script=Scripts/create_snow_track_map.py -unattended -nosplash

The subsystem fills it at runtime and M_Snow_Layer samples it, so it only needs to exist with the right size and format
(8 bit RGBA, linear, repeating). Run this before create_snow_material.py.
"""
import unreal

FOLDER = "/Game/World/SnowTracks"
NAME = "RT_SnowTracks"
SIZE = 2048

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary

path = f"{FOLDER}/{NAME}"
if eal.does_asset_exist(path):
    target = unreal.load_asset(path)
else:
    target = asset_tools.create_asset(NAME, FOLDER, unreal.TextureRenderTarget2D, unreal.TextureRenderTargetFactoryNew())
target.set_editor_property("size_x", SIZE)
target.set_editor_property("size_y", SIZE)
target.set_editor_property("render_target_format", unreal.TextureRenderTargetFormat.RTF_RGBA8)
target.set_editor_property("auto_generate_mips", False)
target.set_editor_property("address_x", unreal.TextureAddress.TA_WRAP)
target.set_editor_property("address_y", unreal.TextureAddress.TA_WRAP)
target.set_editor_property("clear_color", unreal.LinearColor(0.0, 0.0, 0.5, 0.5))
eal.save_loaded_asset(target)
unreal.log_warning(f"create_snow_track_map: {NAME} done")
