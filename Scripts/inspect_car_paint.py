"""Logs the parameters of the City Sample car paint material and the paint instance of each AI car model (prefix CARPAINT)."""
import unreal

MODELS = ["vehicle07_Car", "vehicle02_Car", "vehicle03_Car", "vehicle05_Car", "vehicle06_Car", "vehicle01_Van", "vehicle12_Car"]


def log(text):
    unreal.log("CARPAINT " + text)


def dump_material(asset_path):
    material = unreal.EditorAssetLibrary.load_asset(asset_path)
    if not material:
        log("missing " + asset_path)
        return
    lib = unreal.MaterialEditingLibrary
    log("== %s (%s)" % (asset_path, material.get_class().get_name()))
    if isinstance(material, unreal.MaterialInstanceConstant):
        log("parent: %s" % material.parent.get_path_name() if material.parent else "parent none")
        for entry in material.vector_parameter_values:
            log("  vector %s = %s" % (entry.parameter_info.name, entry.parameter_value))
        for entry in material.scalar_parameter_values:
            log("  scalar %s = %s" % (entry.parameter_info.name, entry.parameter_value))
        for entry in material.texture_parameter_values:
            log("  texture %s = %s" % (entry.parameter_info.name, entry.parameter_value.get_name() if entry.parameter_value else None))
        for name in lib.get_static_switch_parameter_names(material):
            log("  static switch %s = %s" % (name, lib.get_material_instance_static_switch_parameter_value(material, name)))
    else:
        for name in lib.get_vector_parameter_names(material):
            log("  vector %s = %s" % (name, lib.get_material_default_vector_parameter_value(material, name)))
        for name in lib.get_scalar_parameter_names(material):
            log("  scalar %s = %s" % (name, lib.get_material_default_scalar_parameter_value(material, name)))
        for name in lib.get_texture_parameter_names(material):
            log("  texture %s = %s" % (name, lib.get_material_default_texture_parameter_value(material, name)))
        for name in lib.get_static_switch_parameter_names(material):
            log("  static switch %s = %s" % (name, lib.get_material_default_static_switch_parameter_value(material, name)))


dump_material("/Game/CitySampleVehicles/Material/M_Veh_CarPaint")
dump_material("/Game/CitySampleVehicles/Material/MI/MI_Veh_CarPaint")
for folder in MODELS:
    root = "/Game/CitySampleVehicles/" + folder + "/Material/MI"
    for asset in unreal.EditorAssetLibrary.list_assets(root, recursive=False):
        instance = unreal.EditorAssetLibrary.load_asset(asset.split(".")[0])
        if isinstance(instance, unreal.MaterialInstanceConstant) and instance.parent and "CarPaint" in instance.parent.get_name():
            dump_material(asset.split(".")[0])

for folder in MODELS:
    suffix = "vehCar_" + folder.split("_")[0] if folder.endswith("Car") else "vehVan_" + folder.split("_")[0]
    body = unreal.EditorAssetLibrary.load_asset("/Game/CitySampleVehicles/%s/Mesh/SM_%s_No_Wheel" % (folder, suffix))
    if not body:
        log("no body for " + folder)
        continue
    for index, slot in enumerate(body.static_materials):
        material = slot.material_interface
        chain = []
        while material:
            chain.append(material.get_name())
            material = material.parent if isinstance(material, unreal.MaterialInstanceConstant) else None
        log("body %s slot %d %s: %s" % (folder, index, slot.material_slot_name, " < ".join(chain)))
    for index, slot in enumerate(body.static_materials):
        if str(slot.material_slot_name).lower() == "veh_carpaint":
            dump_material(slot.material_interface.get_path_name().split(".")[0])
