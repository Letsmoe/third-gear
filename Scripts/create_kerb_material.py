"""Creates M_KerbStone (grey granite kerb stones, a copy of M_Kerb with the generated granite textures) and M_KerbGutter
(smaller, greyer setts, a copy of M_Road_Cobble) in /Game/World/Materials, so season and wetness parameters stay the same. Vertex colour R varies the stone tone (TintMin at 0, TintMax at 1); joints are drawn at R = 0.
Import the texture set first:
  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script="Scripts/import_generated_textures.py -only=kerb_granite"
Then run this script the same way, under the gpu lock.
"""
import unreal

FOLDER = "/Game/World/Materials"
TEXTURE_FOLDER = "/Game/Textures/Generated/kerb_granite"
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary


def copy_instance(source_name, target_name):
    """Duplicates a material instance (replacing an earlier copy) and returns the copy."""
    target_path = f"{FOLDER}/{target_name}"
    if eal.does_asset_exist(target_path):
        eal.delete_asset(target_path)
    eal.duplicate_asset(f"{FOLDER}/{source_name}", target_path)
    return unreal.load_asset(target_path)


def main():
    stone = copy_instance("M_Kerb", "M_KerbStone")
    for kind in ("BaseColor", "Normal", "Roughness", "AO", "Height"):
        texture = unreal.load_asset(f"{TEXTURE_FOLDER}/T_kerb_granite_{kind}")
        mel.set_material_instance_texture_parameter_value(stone, kind, texture)
    mel.set_material_instance_scalar_parameter_value(stone, "TileSize", 1.0)
    mel.set_material_instance_scalar_parameter_value(stone, "TintMin", 0.4)
    mel.set_material_instance_scalar_parameter_value(stone, "TintMax", 2.8)
    mel.set_material_instance_static_switch_parameter_value(stone, "Parallax", False)
    mel.update_material_instance(stone)
    eal.save_loaded_asset(stone)

    gutter = copy_instance("M_Road_Cobble", "M_KerbGutter")
    mel.set_material_instance_scalar_parameter_value(gutter, "TileSize", 1.1)
    mel.set_material_instance_scalar_parameter_value(gutter, "TintMin", 0.75)
    mel.set_material_instance_scalar_parameter_value(gutter, "TintMax", 0.75)
    mel.update_material_instance(gutter)
    eal.save_loaded_asset(gutter)
    unreal.log("[create_kerb_material] done")


main()
