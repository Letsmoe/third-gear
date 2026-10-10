"""Imports the structure families of the building kit (turbines, pylons, shelters, greenhouses, petrol, bridges,
landmarks; Tools/buildingkit/generate_structures.py) as Nanite static meshes under /Game/World/Structures and fills
every material slot by its name.

  blender -b --factory-startup -P Tools/buildingkit/export_fbx.py -- <data root>/building_kit
  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/import_structures.py -unattended -nosplash

Run Scripts/import_building_kit.py and Scripts/create_furniture_assets.py first: the structures share their masters,
the kit's facade instances (Brick, Plaster, Concrete) and its painted solids (Metal, Timber, Frame, Sill).

Meshes: Meshes/<family>/SM_<family>_<Piece>. Textures: Textures/T_<poster> from <data root>/building_kit/posters.
Materials: Materials/MI_Struct_<Slot>, and MI_Struct_<Slot>_<variant> for the slots that change per placement (the
petrol brands, the advertising posters, the bin stickers). The meshes carry the first variant; the game swaps in the
others per instance (WorldStructures.cpp). Meshes with a see-through slot (shelter and greenhouse glass, foil) are
not Nanite, because Nanite draws only opaque and masked materials.
"""
import glob
import os

import unreal

FOLDER = "/Game/World/Structures"
KIT_MATERIALS = "/Game/World/Kit/Materials"
FURNITURE = "/Game/World/Furniture"
DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
FBX_DIR = os.path.join(DATA_ROOT, "building_kit", "fbx", "structures")
POSTER_DIR = os.path.join(DATA_ROOT, "building_kit", "posters")

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# Slots shared with the building kit and the world's facades.
SHARED = {
    "Brick": "/Game/World/Facades/M_Facade_ClinkerDeepRed",
    "Plaster": "/Game/World/Facades/M_Facade_RenderScratchWhite",
    "Concrete": "/Game/World/Facades/M_Facade_ConcreteSlab",
    "Metal": f"{KIT_MATERIALS}/MI_Kit_Metal",
    "Timber": f"{KIT_MATERIALS}/MI_Kit_Timber",
    "Frame": f"{KIT_MATERIALS}/MI_Kit_Frame",
    "Sill": f"{KIT_MATERIALS}/MI_Kit_Sill",
    "SignFace": f"{FURNITURE}/M_SignFace",
}
# Painted and plain solids: slot -> (colour sRGB, roughness, metallic).
SOLIDS = {
    "Paint": ((0.86, 0.87, 0.86), 0.45, 0.0),          # RAL 7035 light grey turbine and canopy paint
    "PaintRed": ((0.62, 0.05, 0.04), 0.45, 0.0),       # RAL 3020 traffic red
    "PowderCoat": ((0.15, 0.16, 0.17), 0.5, 0.0),      # RAL 7016 anthracite
    "Lattice": ((0.55, 0.57, 0.56), 0.6, 0.6),         # weathered hot-dip galvanised steel
    "Insulator": ((0.48, 0.50, 0.52), 0.35, 0.0),      # glass cap insulators
    "Litter": ((0.78, 0.74, 0.66), 0.8, 0.0),
    "BinBag": ((0.03, 0.03, 0.035), 0.35, 0.0),
    "Aluminium": ((0.74, 0.76, 0.78), 0.35, 0.9),
    "SafetyYellow": ((0.95, 0.72, 0.05), 0.5, 0.0),
    "WashBrush": ((0.10, 0.32, 0.75), 0.9, 0.0),
    "RoofSequins": ((0.86, 0.86, 0.84), 0.3, 0.4),     # Elbphilharmonie roof, white pressed discs
}
# The opaque dark glass of windows: the Elbphilharmonie's base keeps the kit's.
OPAQUE_GLASS = f"{KIT_MATERIALS}/MI_Kit_Glass"
# See-through slots: slot -> (colour sRGB, opacity, roughness).
GLAZING = {
    "Glass": ((0.80, 0.86, 0.86), 0.18, 0.04),
    "GreenhouseGlass": ((0.86, 0.90, 0.88), 0.25, 0.08),
    "Foil": ((0.92, 0.93, 0.90), 0.55, 0.35),
}
# Printed faces: slot -> (default image, roughness, metallic, night glow cd/m2; 0 for faces that are not back-lit).
PRINTS = {
    "AdPanel": ("ad_bus_drivers", 0.15, 0.0, 120.0),
    "Timetable": ("timetable", 0.3, 0.0, 0.0),
    "StopSignFace": ("stop_panel", 0.35, 0.0, 0.0),
    "BinSticker": ("bin_sticker_0", 0.4, 0.0, 0.0),
    "BrandLogo": ("fuel_logo_nordtank", 0.3, 0.0, 150.0),
    "PriceBoard": ("fuel_prices_nordtank", 0.3, 0.0, 120.0),
    "PumpFace": ("fuel_pump_nordtank", 0.3, 0.0, 60.0),
    "WashSign": ("fuel_wash_nordtank", 0.4, 0.0, 0.0),
    "FacadeGlass": ("elbphilharmonie_glass", 0.12, 0.5, 0.0),
}
# Per-placement variants: slot -> {variant: image or colour}. The first entry is what the mesh carries.
BRANDS = ("nordtank", "hopp", "vierlaender")
BRAND_PAINT = {"nordtank": (0.0, 0.29, 0.60), "hopp": (0.85, 0.07, 0.10), "vierlaender": (0.0, 0.39, 0.22)}
PRINT_VARIANTS = {
    "BrandLogo": {brand: "fuel_logo_" + brand for brand in BRANDS},
    "PriceBoard": {brand: "fuel_prices_" + brand for brand in BRANDS},
    "PumpFace": {brand: "fuel_pump_" + brand for brand in BRANDS},
    "WashSign": {brand: "fuel_wash_" + brand for brand in BRANDS},
    "AdPanel": {name: "ad_" + name for name in ("bus_drivers", "bank", "boogle", "culture", "supermarket")},
    "BinSticker": {str(index): "bin_sticker_%d" % index for index in range(5)},
}
# Lights: slot -> (colour sRGB, day luminance, night luminance in cd/m2, blinks).
LIGHTS = {
    "CanopyLight": ((1.0, 0.97, 0.90), 300.0, 2500.0, False),
    "Beacon": ((1.0, 0.04, 0.02), 0.0, 4000.0, True),        # W-rot obstruction light, only at night
}
# Enercon's tower foot: five green bands, 3 m each from dark at the ground to pale (linear colours, kit/blender_io.py).
GREEN_BANDS = [(0.03, 0.17, 0.07), (0.07, 0.27, 0.11), (0.15, 0.40, 0.17), (0.30, 0.53, 0.26), (0.50, 0.66, 0.40)]

# The glow of a back-lit face: its image times the glow, weak against daylight, full at night.
PRINT_GLOW_HLSL = "return Print * Glow * lerp(0.15, 1.0, saturate(Night));"
# W-rot obstruction light: on 1 s, off 0.5 s, on 1 s, off 1.5 s, at night only.
LIGHT_HLSL = """
float phase = fmod(Time, 4.0);
float on = Blinks > 0.5 ? (phase < 1.0 || (phase >= 1.5 && phase < 2.5) ? 1.0 : 0.0) : 1.0;
return Tint * lerp(Day, NightLuminance, saturate(Night)) * on;
"""
# A conductor sags on a parabola below its straight chord: u runs 0 to 1 along the span, the sag in metres comes
# from the instance (custom data 0), set per span by the game (WorldStructures.cpp).
WIRE_SAG_HLSL = "return float3(0.0, 0.0, -100.0 * Sag * 4.0 * UV.x * (1.0 - UV.x));"
# How far below its chord the wire mesh may reach, cm, so the bounds cover the deepest sag.
WIRE_SAG_BOUNDS_CM = 3000.0
# The height on the tower is 1 - V (V of the kit's metre UVs is the height, flipped by the FBX import).
BANDS_HLSL = """
float height = 1.0 - UV.y;
int band = clamp((int)floor(height / 3.0), 0, 4);
float3 colours[5] = {%s};
return colours[band];
"""


def custom_input(name):
    item = unreal.CustomInput()
    item.set_editor_property("input_name", name)
    return item


def expr(material, cls, x, y, **props):
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def link(a, a_out, b, b_in):
    if not mel.connect_material_expressions(a, a_out, b, b_in):
        raise RuntimeError(f"failed to connect {a.get_name()}.{a_out} -> {b.get_name()}.{b_in}")


def linear(srgb):
    return unreal.LinearColor(*[channel ** 2.2 for channel in srgb], 1.0)


def fresh_material(name, **properties):
    """An empty master material under Materials, used with instanced meshes."""
    folder = f"{FOLDER}/Materials"
    path = f"{folder}/{name}"
    if eal.does_asset_exist(path):
        material = unreal.load_asset(path)
        mel.delete_all_material_expressions(material)
    else:
        material = asset_tools.create_asset(name, folder, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("used_with_instanced_static_meshes", True)
    material.set_editor_property("used_with_nanite", True)
    for key, value in properties.items():
        material.set_editor_property(key, value)
    return material


def get_instance(name, parent):
    folder = f"{FOLDER}/Materials"
    path = f"{folder}/{name}"
    if eal.does_asset_exist(path):
        instance = unreal.load_asset(path)
    else:
        instance = asset_tools.create_asset(name, folder, unreal.MaterialInstanceConstant,
                                            unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(instance, parent)
    return instance


def save_instance(instance):
    mel.update_material_instance(instance)
    eal.save_loaded_asset(instance)


def finish(material):
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def night_parameter(material, x, y):
    collection = unreal.load_asset(f"{FURNITURE}/MPC_Furniture")
    return expr(material, unreal.MaterialExpressionCollectionParameter, x, y, collection=collection, parameter_name="Night")


def build_print_master():
    """A printed face: the image, its roughness and metalness, and a glow for back-lit faces."""
    material = fresh_material("M_StructurePrint")
    image = expr(material, unreal.MaterialExpressionTextureSampleParameter2D, -900, 0, parameter_name="Print",
                 sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    rough = expr(material, unreal.MaterialExpressionScalarParameter, -900, 250, parameter_name="Roughness", default_value=0.3)
    metal = expr(material, unreal.MaterialExpressionScalarParameter, -900, 350, parameter_name="Metallic", default_value=0.0)
    glow = expr(material, unreal.MaterialExpressionScalarParameter, -900, 450, parameter_name="Glow", default_value=0.0)
    night = night_parameter(material, -900, 550)
    custom = expr(material, unreal.MaterialExpressionCustom, -500, 400, code=PRINT_GLOW_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="BackLight",
                  inputs=[custom_input("Print"), custom_input("Glow"), custom_input("Night")])
    link(image, "RGB", custom, "Print")
    link(glow, "", custom, "Glow")
    link(night, "", custom, "Night")
    mel.connect_material_property(image, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return finish(material)


def build_glazing_master():
    """Clear glass and foil: translucent, lit with forward shading so it reflects the sky."""
    material = fresh_material("M_StructureGlazing", blend_mode=unreal.BlendMode.BLEND_TRANSLUCENT,
                              translucency_lighting_mode=unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING,
                              two_sided=True)
    material.set_editor_property("used_with_nanite", False)
    colour = expr(material, unreal.MaterialExpressionVectorParameter, -500, 0, parameter_name="Color",
                  default_value=unreal.LinearColor(0.7, 0.75, 0.75, 1))
    opacity = expr(material, unreal.MaterialExpressionScalarParameter, -500, 200, parameter_name="Opacity", default_value=0.2)
    rough = expr(material, unreal.MaterialExpressionScalarParameter, -500, 300, parameter_name="Roughness", default_value=0.05)
    specular = expr(material, unreal.MaterialExpressionConstant, -500, 400, r=0.5)
    mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR)
    return finish(material)


def build_light_master():
    """A light face: its tint at a day and a night luminance, optionally blinking like an obstruction light."""
    material = fresh_material("M_StructureLight")
    tint = expr(material, unreal.MaterialExpressionVectorParameter, -900, 0, parameter_name="Tint",
                default_value=unreal.LinearColor(1, 1, 1, 1))
    day = expr(material, unreal.MaterialExpressionScalarParameter, -900, 200, parameter_name="Day", default_value=0.0)
    night_luminance = expr(material, unreal.MaterialExpressionScalarParameter, -900, 300, parameter_name="NightLuminance",
                           default_value=1000.0)
    blinks = expr(material, unreal.MaterialExpressionScalarParameter, -900, 400, parameter_name="Blinks", default_value=0.0)
    time = expr(material, unreal.MaterialExpressionTime, -900, 500)
    night = night_parameter(material, -900, 600)
    custom = expr(material, unreal.MaterialExpressionCustom, -500, 300, code=LIGHT_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="Light",
                  inputs=[custom_input(name) for name in ("Tint", "Day", "NightLuminance", "Blinks", "Time", "Night")])
    for node, name in ((tint, "Tint"), (day, "Day"), (night_luminance, "NightLuminance"), (blinks, "Blinks"),
                       (time, "Time"), (night, "Night")):
        link(node, "", custom, name)
    base = expr(material, unreal.MaterialExpressionMultiply, -500, 0, const_b=0.6)
    link(tint, "", base, "A")
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -500, 150, r=0.2), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    return finish(material)


def build_bands_material():
    """M_StructureGreenBands: the Enercon tower foot."""
    material = fresh_material("M_StructureGreenBands")
    colours = ", ".join("float3(%g, %g, %g)" % band for band in GREEN_BANDS)
    uv = expr(material, unreal.MaterialExpressionTextureCoordinate, -700, 0)
    custom = expr(material, unreal.MaterialExpressionCustom, -400, 0, code=BANDS_HLSL % colours,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="GreenBands",
                  inputs=[custom_input("UV")])
    link(uv, "", custom, "UV")
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -400, 200, r=0.45), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    return finish(material)


def build_wire_material():
    """M_StructureWire: weathered aluminium conductor, sagged by the world position offset."""
    material = fresh_material("M_StructureWire")
    material.set_editor_property("used_with_nanite", False)
    uv = expr(material, unreal.MaterialExpressionTextureCoordinate, -700, 200)
    sag = expr(material, unreal.MaterialExpressionPerInstanceCustomData, -700, 300, data_index=0, const_default_value=0.0)
    custom = expr(material, unreal.MaterialExpressionCustom, -400, 250, code=WIRE_SAG_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="Sag",
                  inputs=[custom_input("UV"), custom_input("Sag")])
    link(uv, "", custom, "UV")
    link(sag, "", custom, "Sag")
    colour = expr(material, unreal.MaterialExpressionConstant3Vector, -400, 0, constant=linear((0.42, 0.43, 0.44)))
    mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -400, 100, r=0.5), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -400, 150, r=0.8), "",
                                  unreal.MaterialProperty.MP_METALLIC)
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    return finish(material)


def import_posters():
    """Every poster image as T_<name>: sRGB, clamped, streamed."""
    tasks = []
    for path in sorted(glob.glob(os.path.join(POSTER_DIR, "*.png"))):
        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = f"{FOLDER}/Textures"
        task.destination_name = "T_" + os.path.splitext(os.path.basename(path))[0]
        task.automated = True
        task.save = True
        task.replace_existing = True
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)


def poster(name):
    return unreal.load_asset(f"{FOLDER}/Textures/T_{name}")


def print_instance(name, master, image, roughness, metallic, glow):
    instance = get_instance(name, master)
    mel.set_material_instance_texture_parameter_value(instance, "Print", poster(image))
    mel.set_material_instance_scalar_parameter_value(instance, "Roughness", roughness)
    mel.set_material_instance_scalar_parameter_value(instance, "Metallic", metallic)
    mel.set_material_instance_scalar_parameter_value(instance, "Glow", glow)
    save_instance(instance)
    return instance


def solid_instance(name, master, srgb, roughness, metallic):
    instance = get_instance(name, master)
    mel.set_material_instance_vector_parameter_value(instance, "Color", linear(srgb))
    mel.set_material_instance_scalar_parameter_value(instance, "Roughness", roughness)
    mel.set_material_instance_scalar_parameter_value(instance, "Metallic", metallic)
    save_instance(instance)
    return instance


def create_materials():
    """Slot name -> material for every structure slot, plus the variant instances."""
    solid_master = unreal.load_asset(f"{FURNITURE}/M_FurnitureSolid")
    print_master = build_print_master()
    glazing_master = build_glazing_master()
    light_master = build_light_master()
    result = {slot: unreal.load_asset(path) for slot, path in SHARED.items()}
    result["PaintGreen"] = build_bands_material()
    result["Conductor"] = build_wire_material()
    for slot, (srgb, roughness, metallic) in SOLIDS.items():
        result[slot] = solid_instance(f"MI_Struct_{slot}", solid_master, srgb, roughness, metallic)
    for brand in BRANDS:
        solid_instance(f"MI_Struct_BrandPaint_{brand}", solid_master, BRAND_PAINT[brand], 0.4, 0.0)
    result["BrandPaint"] = unreal.load_asset(f"{FOLDER}/Materials/MI_Struct_BrandPaint_{BRANDS[0]}")
    for slot, (srgb, opacity, roughness) in GLAZING.items():
        instance = get_instance(f"MI_Struct_{slot}", glazing_master)
        mel.set_material_instance_vector_parameter_value(instance, "Color", linear(srgb))
        mel.set_material_instance_scalar_parameter_value(instance, "Opacity", opacity)
        mel.set_material_instance_scalar_parameter_value(instance, "Roughness", roughness)
        save_instance(instance)
        result[slot] = instance
    for slot, (image, roughness, metallic, glow) in PRINTS.items():
        result[slot] = print_instance(f"MI_Struct_{slot}", print_master, image, roughness, metallic, glow)
        for variant, variant_image in PRINT_VARIANTS.get(slot, {}).items():
            print_instance(f"MI_Struct_{slot}_{variant}", print_master, variant_image, roughness, metallic, glow)
    for slot, (srgb, day, night, blinks) in LIGHTS.items():
        instance = get_instance(f"MI_Struct_{slot}", light_master)
        mel.set_material_instance_vector_parameter_value(instance, "Tint", linear(srgb))
        mel.set_material_instance_scalar_parameter_value(instance, "Day", day)
        mel.set_material_instance_scalar_parameter_value(instance, "NightLuminance", night)
        mel.set_material_instance_scalar_parameter_value(instance, "Blinks", 1.0 if blinks else 0.0)
        save_instance(instance)
        result[slot] = instance
    return result


def import_meshes():
    """Every structure FBX as Meshes/<family>/SM_<family>_<Piece>, replacing the old one."""
    tasks = []
    for path in sorted(glob.glob(os.path.join(FBX_DIR, "*", "SM_*.fbx"))):
        family = os.path.basename(os.path.dirname(path))
        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = f"{FOLDER}/Meshes/{family}"
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


def material_for(family, slot, materials):
    """The material of one slot; the Elbphilharmonie's window panes stay the kit's opaque dark glass."""
    if slot == "Glass" and family == "landmarks":
        return unreal.load_asset(OPAQUE_GLASS)
    return materials.get(slot)


def prepare_wire(mesh):
    """The conductor: no Nanite (it is sagged by the world position offset), no collision, bounds down to the sag."""
    settings = mesh.get_editor_property("nanite_settings")
    settings.enabled = False
    mesh.set_editor_property("nanite_settings", settings)
    mesh.set_editor_property("negative_bounds_extension", unreal.Vector(0.0, 0.0, WIRE_SAG_BOUNDS_CM))
    body_setup = mesh.get_editor_property("body_setup")
    body_setup.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)
    mesh.set_editor_property("body_setup", body_setup)
    eal.save_loaded_asset(mesh)


def assign_materials(materials):
    """Fills every slot by its name, collides with the mesh itself, and turns on Nanite unless a slot is see-through."""
    for path in sorted(eal.list_assets(f"{FOLDER}/Meshes", recursive=True, include_folder=False)):
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            continue
        family = path.split("/Meshes/")[1].split("/")[0]
        see_through = False
        for index, slot in enumerate(mesh.static_materials):
            name = str(slot.material_slot_name)
            material = material_for(family, name, materials)
            if material is None:
                unreal.log_warning(f"{mesh.get_name()}: no material for slot {name}")
                continue
            mesh.set_material(index, material)
            see_through = see_through or (name in GLAZING and family != "landmarks")
        if mesh.get_name() == "SM_pylons_Wire":
            prepare_wire(mesh)
            continue
        settings = mesh.get_editor_property("nanite_settings")
        settings.enabled = not see_through
        mesh.set_editor_property("nanite_settings", settings)
        body_setup = mesh.get_editor_property("body_setup")
        body_setup.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_COMPLEX_AS_SIMPLE)
        mesh.set_editor_property("body_setup", body_setup)
        eal.save_loaded_asset(mesh)
        box = mesh.get_bounding_box()
        unreal.log_warning(f"STRUCTURE {mesh.get_name()} nanite={not see_through} min {box.min} max {box.max}")


import_posters()
import_meshes()
assign_materials(create_materials())
unreal.log_warning("import_structures: done")
