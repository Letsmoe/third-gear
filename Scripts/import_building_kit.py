"""Imports the building kit pieces (Tools/buildingkit) as Nanite static meshes under /Game/World/Kit and gives every
material slot a named instance, so the facade, window and weathering materials (#46, #83, #84) can replace them one
slot at a time.

  blender -b --factory-startup -P Tools/buildingkit/export_fbx.py -- <data root>/building_kit
  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/import_building_kit.py -unattended -nosplash

Meshes: SM_<style>_<piece> (SM_brick_Wall_Window ...). Materials: MI_Kit_<Slot> for the slots Brick, Plaster, Concrete,
RoofTile, Frame, Glass, Sill, Metal, Timber and Thatch. The wall and roof slots default to the world's facade instances
(/Game/World/Facades, painted windows are off for them) and each building replaces them by its own, see WorldKitBuildings.cpp.
The flat roof coverings M_Roof_Gravel and M_Roof_Bitumen are made here as well.
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

# Default material of the textured slots, a world facade instance; the buildings replace them per building (WorldKitBuildings.cpp).
TEXTURED = {
    "Brick": "/Game/World/Facades/M_Facade_ClinkerDeepRed",
    "Plaster": "/Game/World/Facades/M_Facade_RenderScratchWhite",
    "Concrete": "/Game/World/Facades/M_Facade_ConcreteSlab",
    "RoofTile": "/Game/World/Facades/M_Roof_Clay",
}
FACADE_MASTER = "/Game/World/Facades/M_FacadeMaster"
# slot -> (colour sRGB, roughness, metallic) for plain painted or metal parts
SOLIDS = {
    "Frame": ((0.88, 0.88, 0.85), 0.40, 0.0),     # white painted timber or PVC
    "Glass": ((0.015, 0.02, 0.025), 0.03, 0.0),   # dark reflective pane until the window shader (#83) replaces it
    "Sill": ((0.62, 0.58, 0.52), 0.85, 0.0),      # sandstone, cement or concrete
    "Metal": ((0.50, 0.52, 0.53), 0.40, 0.85),    # zinc gutters and flashing
    "Timber": ((0.09, 0.055, 0.03), 0.75, 0.0),   # black-brown oak frame
    "Thatch": ((0.42, 0.33, 0.16), 0.95, 0.0),
}
# Flat roof coverings, instances of the facade master made from a copy of a roof instance: name -> (texture set, tile metres, value, parallax).
FLAT_ROOFS = {
    "Roof_Gravel": ("Ground/gravel_ground_01", 2.0, 0.85, 0.02),
    "Roof_Bitumen": ("Asphalt/Asphalt031", 3.0, 0.45, 0.004),
}


def get_instance(name):
    path = f"{FOLDER}/Materials/{name}"
    if eal.does_asset_exist(path):
        return unreal.load_asset(path)
    return asset_tools.create_asset(name, f"{FOLDER}/Materials", unreal.MaterialInstanceConstant,
                                    unreal.MaterialInstanceConstantFactoryNew())


def allow_instanced_meshes():
    """The facade master has to be allowed on instanced static meshes, or the kit walls show the grid material."""
    master = unreal.load_asset(FACADE_MASTER)
    if not master.get_editor_property("used_with_instanced_static_meshes"):
        master.set_editor_property("used_with_instanced_static_meshes", True)
        mel.recompile_material(master)
        eal.save_loaded_asset(master)


def create_flat_roofs():
    """M_Roof_Gravel and M_Roof_Bitumen in the facade folder, for the flat roofs of blocks, halls and shops."""
    for name, (texture_set, tile, value, parallax) in FLAT_ROOFS.items():
        path = f"/Game/World/Facades/M_{name}"
        if not eal.does_asset_exist(path):
            eal.duplicate_asset("/Game/World/Facades/M_Roof_Clay", path)
        instance = unreal.load_asset(path)
        set_name = texture_set.split("/")[-1]
        for kind in ("BaseColor", "Normal", "Roughness", "AO", "Height"):
            texture_path = f"/Game/Textures/{texture_set}/T_{set_name}_{kind}"
            if eal.does_asset_exist(texture_path):
                mel.set_material_instance_texture_parameter_value(instance, kind, unreal.load_asset(texture_path))
        mel.set_material_instance_vector_parameter_value(instance, "TileMetres", unreal.LinearColor(tile, tile, 0, 0))
        mel.set_material_instance_scalar_parameter_value(instance, "Value", value)
        mel.set_material_instance_scalar_parameter_value(instance, "RecolorAmount", 0.0)
        mel.set_material_instance_scalar_parameter_value(instance, "TintMin", 0.9)
        mel.set_material_instance_scalar_parameter_value(instance, "TintMax", 1.1)
        mel.set_material_instance_scalar_parameter_value(instance, "HeightRatio", parallax)
        mel.update_material_instance(instance)
        eal.save_loaded_asset(instance)


def create_materials():
    """Slot name -> default material: the world's facade instances for the textured slots, solid instances for the rest."""
    allow_instanced_meshes()
    create_flat_roofs()
    result = {slot: unreal.load_asset(path) for slot, path in TEXTURED.items()}
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
            unreal.log_warning(f"KITBOUNDS {name} min {box.min} max {box.max}")


import_meshes()
assign_materials(create_materials())
report_bounds()
