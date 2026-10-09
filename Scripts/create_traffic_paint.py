"""Creates one clean paint material instance per AI traffic car model in /Game/Vehicles/TrafficPaint.

Each instance has the City Sample paint instance of that model as parent (so its normal map and grime mask stay), and
switches off the effects that grey the colour out: the painted-imperfection layers (dirt, dust, splatter, primer and
bare-metal chips) and the paint and albedo variation lookups, which replace or shift the BaseColor parameter.
The City Sample assets themselves are not changed. Run headless; existing instances are updated in place.
"""
import unreal

MODELS = ["vehicle07_Car", "vehicle02_Car", "vehicle03_Car", "vehicle05_Car", "vehicle06_Car", "vehicle01_Van", "vehicle12_Car"]
SWITCHES_OFF = ["Enable Painted Imp PI", "Damage PI", "DirtSplatter PI", "Dirt PI", "Dust PI", "Paint Variation", "Albedo Variation"]
OUTPUT_FOLDER = "/Game/Vehicles/TrafficPaint"


def find_paint_instance(folder):
    """Returns the body slot material named veh_carPaint of a model."""
    suffix = ("vehCar_" if folder.endswith("Car") else "vehVan_") + folder.split("_")[0]
    body = unreal.EditorAssetLibrary.load_asset("/Game/CitySampleVehicles/%s/Mesh/SM_%s_No_Wheel" % (folder, suffix))
    for slot in body.static_materials:
        if str(slot.material_slot_name).lower() == "veh_carpaint":
            return slot.material_interface
    return None


def create_instance(folder):
    parent = find_paint_instance(folder)
    if parent is None:
        unreal.log_warning("TRAFFICPAINT no paint slot in " + folder)
        return
    name = "MI_TrafficPaint_" + folder
    path = OUTPUT_FOLDER + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        instance = unreal.EditorAssetLibrary.load_asset(path)
    else:
        factory = unreal.MaterialInstanceConstantFactoryNew()
        instance = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, OUTPUT_FOLDER, unreal.MaterialInstanceConstant, factory)
    unreal.MaterialEditingLibrary.set_material_instance_parent(instance, parent)
    for switch in SWITCHES_OFF:
        unreal.MaterialEditingLibrary.set_material_instance_static_switch_parameter_value(instance, switch, False)
    unreal.MaterialEditingLibrary.update_material_instance(instance)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("TRAFFICPAINT created %s from %s" % (path, parent.get_name()))


for model_folder in MODELS:
    create_instance(model_folder)
