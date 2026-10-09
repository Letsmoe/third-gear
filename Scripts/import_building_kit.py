"""Imports the building kit pieces (Tools/buildingkit) as Nanite static meshes under /Game/World/Kit and gives every
material slot a named instance, so the facade, window and weathering materials (#46, #83, #84) can replace them one
slot at a time.

  blender -b --factory-startup -P Tools/buildingkit/export_fbx.py -- <data root>/building_kit
  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/import_building_kit.py -unattended -nosplash

Meshes: SM_<style>_<piece> (SM_brick_Wall_Window ...). Materials: MI_Kit_<Slot> for the slots Brick, Plaster, Concrete,
RoofTile, Frame, Glass, Sill, Metal, Timber and Thatch. The wall slots are instances of the streamed world's facade
materials with the painted windows switched off, because the kit walls have real openings.
"""
import glob
import os

import unreal

FOLDER = "/Game/World/Kit"
DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
FBX_DIR = os.path.join(DATA_ROOT, "building_kit", "fbx")

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# slot -> (parent material, tint) for slots that reuse the world's textured materials
TEXTURED = {
    "Brick": ("/Game/World/Materials/M_Facade_Brick", 3.4),
    "Plaster": ("/Game/World/Materials/M_Facade_Plaster", 0.97),
    "Concrete": ("/Game/World/Materials/M_Facade_Concrete", 0.95),
    "RoofTile": ("/Game/World/Materials/M_Roof_Tiles", 0.9),
}
# slot -> (colour sRGB, roughness, metallic) for plain painted or metal parts
SOLIDS = {
    "Frame": ((0.88, 0.88, 0.85), 0.40, 0.0),     # white painted timber or PVC
    "Glass": ((0.015, 0.02, 0.025), 0.03, 0.0),   # dark reflective pane until the window shader (#83) replaces it
    "Sill": ((0.62, 0.58, 0.52), 0.85, 0.0),      # sandstone, cement or concrete
    "Metal": ((0.50, 0.52, 0.53), 0.40, 0.85),    # zinc gutters and flashing
    "Timber": ((0.09, 0.055, 0.03), 0.75, 0.0),   # black-brown oak frame
    "Thatch": ((0.42, 0.33, 0.16), 0.95, 0.0),
}


def get_instance(name):
    path = f"{FOLDER}/Materials/{name}"
    if eal.does_asset_exist(path):
        return unreal.load_asset(path)
    return asset_tools.create_asset(name, f"{FOLDER}/Materials", unreal.MaterialInstanceConstant,
                                    unreal.MaterialInstanceConstantFactoryNew())


def create_materials():
    """MI_Kit_<Slot> for every slot; returns slot name -> material instance."""
    result = {}
    for slot, (parent_path, tint) in TEXTURED.items():
        instance = get_instance(f"MI_Kit_{slot}")
        mel.set_material_instance_parent(instance, unreal.load_asset(parent_path))
        if slot != "RoofTile":
            mel.set_material_instance_static_switch_parameter_value(instance, "Windows", False)
            mel.set_material_instance_scalar_parameter_value(instance, "TintMin", tint)
            mel.set_material_instance_scalar_parameter_value(instance, "TintMax", tint)
        mel.update_material_instance(instance)
        eal.save_loaded_asset(instance)
        result[slot] = instance
    solid_master = unreal.load_asset("/Game/World/Furniture/M_FurnitureSolid")
    for slot, (srgb, roughness, metallic) in SOLIDS.items():
        instance = get_instance(f"MI_Kit_{slot}")
        mel.set_material_instance_parent(instance, solid_master)
        mel.set_material_instance_vector_parameter_value(instance, "Color", unreal.LinearColor(*[c ** 2.2 for c in srgb], 1.0))
        mel.set_material_instance_scalar_parameter_value(instance, "Roughness", roughness)
        mel.set_material_instance_scalar_parameter_value(instance, "Metallic", metallic)
        mel.update_material_instance(instance)
        eal.save_loaded_asset(instance)
        result[slot] = instance
    return result


def import_meshes():
    tasks = []
    for path in sorted(glob.glob(os.path.join(FBX_DIR, "SM_*.fbx"))):
        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = f"{FOLDER}/Meshes"
        task.automated = True
        task.save = True
        task.replace_existing = True
        options = unreal.FbxImportUI()
        options.import_mesh = True
        options.import_as_skeletal = False
        options.import_materials = False
        options.import_textures = False
        options.static_mesh_import_data.set_editor_property("combine_meshes", True)
        options.static_mesh_import_data.set_editor_property("generate_lightmap_u_vs", False)
        options.static_mesh_import_data.set_editor_property("auto_generate_collision", False)
        task.options = options
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)


def assign_materials(materials):
    """Fills every slot by its name and turns on Nanite."""
    for path in sorted(eal.list_assets(f"{FOLDER}/Meshes", recursive=False, include_folder=False)):
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            continue
        for index, slot in enumerate(mesh.static_materials):
            name = str(slot.material_slot_name)
            if name not in materials:
                unreal.log_warning(f"{mesh.get_name()}: no material for slot {name}")
                continue
            mesh.set_material(index, materials[name])
        settings = mesh.get_editor_property("nanite_settings")
        settings.enabled = True
        mesh.set_editor_property("nanite_settings", settings)
        eal.save_loaded_asset(mesh)


def report_bounds():
    """Logs the bounds of the pieces that show the axis convention (wall bay, door step, sill)."""
    for name in ("SM_brick_Wall_Solid", "SM_brick_Door_Step", "SM_brick_Sill", "SM_brick_Corner_L"):
        mesh = unreal.load_asset(f"{FOLDER}/Meshes/{name}")
        if mesh:
            box = mesh.get_bounding_box()
            unreal.log(f"KITBOUNDS {name} min {box.min} max {box.max}")


import_meshes()
assign_materials(create_materials())
report_bounds()
