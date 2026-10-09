"""Imports the street furniture models and sign graphics and creates their materials under /Game/World/Furniture.

  Tools/furniture: blender -b --factory-startup -P Tools/furniture/make_models.py -- <data root>/furniture
  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_furniture_assets.py -unattended -nosplash

Meshes: SM_SignalPole, SM_SignalHead, SM_SignPlate, SM_SignClamp, SM_SignPole_<cm>, SM_StreetLamp. Textures: T_<sign
name> from the German sign graphics in <data root>/raw_assets/signs/png. Materials: solid instances of M_FurnitureSolid
for housings and metal, M_SignalLensRed/Amber/Green (glow from per-instance custom data 0, 1, 2 set by the signal
controller), M_LampLens (glows by night), M_SignFace (masked, two-sided; retroreflective at night in the car's
headlight cone). MPC_Furniture carries the night factor and the headlight pose the shaders read.
"""
import glob
import os

import unreal

FOLDER = "/Game/World/Furniture"
DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
MODEL_DIR = os.path.join(DATA_ROOT, "furniture")
SIGN_DIR = os.path.join(DATA_ROOT, "raw_assets", "signs", "png")

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# material name -> (colour sRGB, roughness, metallic)
SOLIDS = {
    "SignalHousing": ((0.20, 0.22, 0.235), 0.5, 0.0),       # RAL 7016 anthracite, powder coated
    "SignalPole": ((0.66, 0.68, 0.69), 0.42, 0.85),           # hot-dip galvanised
    "FurnitureMetalGalv": ((0.66, 0.68, 0.69), 0.42, 0.85),
    "FurnitureCap": ((0.03, 0.03, 0.03), 0.6, 0.0),
    "SignalBackplate": ((0.015, 0.015, 0.016), 0.7, 0.0),
    "LampHousing": ((0.50, 0.52, 0.53), 0.38, 0.7),
}

NIGHT_LENS_HLSL = "return Tint * Intensity * lerp(7000.0, 450.0, saturate(Night));"
LAMP_LENS_HLSL = "return Tint * Night * 2500.0;"
# Retroreflective sheeting returns light toward its source: at night the car's headlight cone lights the face.
SIGN_RETRO_HLSL = """
float3 toPixel = WorldPosition - HeadlightPosition;
float distanceCm = max(length(toPixel), 100.0);
float3 direction = toPixel / distanceCm;
float cone = smoothstep(0.80, 0.96, dot(direction, normalize(HeadlightDirection)));
float facing = saturate(dot(-direction, normalize(SignNormal)));
float metres = distanceCm * 0.01;
float luminance = min(250.0 * HeadlightCandela / (metres * metres), 12000.0);
return Graphic * luminance * cone * facing * facing * HeadlightOn * Front;
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


def fresh_material(name, **properties):
    path = f"{FOLDER}/{name}"
    if eal.does_asset_exist(path):
        material = unreal.load_asset(path)
        mel.delete_all_material_expressions(material)
    else:
        material = asset_tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("used_with_instanced_static_meshes", True)
    for key, value in properties.items():
        material.set_editor_property(key, value)
    return material


def create_collection():
    """MPC_Furniture: Night (0 day, 1 night), headlight pose and strength for the retroreflective signs."""
    path = f"{FOLDER}/MPC_Furniture"
    if eal.does_asset_exist(path):
        return unreal.load_asset(path)
    collection = asset_tools.create_asset("MPC_Furniture", FOLDER, unreal.MaterialParameterCollection,
                                          unreal.MaterialParameterCollectionFactoryNew())
    scalars = []
    for name, default in (("Night", 0.0), ("HeadlightOn", 0.0), ("HeadlightCandela", 5000.0)):
        parameter = unreal.CollectionScalarParameter()
        parameter.set_editor_property("parameter_name", name)
        parameter.set_editor_property("default_value", default)
        scalars.append(parameter)
    vectors = []
    for name, default in (("HeadlightPosition", unreal.LinearColor(0, 0, 0, 0)), ("HeadlightDirection", unreal.LinearColor(1, 0, 0, 0))):
        parameter = unreal.CollectionVectorParameter()
        parameter.set_editor_property("parameter_name", name)
        parameter.set_editor_property("default_value", default)
        vectors.append(parameter)
    collection.set_editor_property("scalar_parameters", scalars)
    collection.set_editor_property("vector_parameters", vectors)
    eal.save_loaded_asset(collection)
    return collection


def collection_scalar(material, collection, name, x, y):
    return expr(material, unreal.MaterialExpressionCollectionParameter, x, y, collection=collection, parameter_name=name)


def build_solid_master():
    material = fresh_material("M_FurnitureSolid")
    color = expr(material, unreal.MaterialExpressionVectorParameter, -500, 0, parameter_name="Color",
                 default_value=unreal.LinearColor(0.5, 0.5, 0.5, 1))
    rough = expr(material, unreal.MaterialExpressionScalarParameter, -500, 200, parameter_name="Roughness", default_value=0.5)
    metal = expr(material, unreal.MaterialExpressionScalarParameter, -500, 300, parameter_name="Metallic", default_value=0.0)
    mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def build_solid_instances(master):
    for name, (srgb, roughness, metallic) in SOLIDS.items():
        path = f"{FOLDER}/MI_{name}"
        instance = unreal.load_asset(path) if eal.does_asset_exist(path) else asset_tools.create_asset(
            f"MI_{name}", FOLDER, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        mel.set_material_instance_parent(instance, master)
        linear = unreal.LinearColor(*[c ** 2.2 for c in srgb], 1.0)
        mel.set_material_instance_vector_parameter_value(instance, "Color", linear)
        mel.set_material_instance_scalar_parameter_value(instance, "Roughness", roughness)
        mel.set_material_instance_scalar_parameter_value(instance, "Metallic", metallic)
        mel.update_material_instance(instance)
        eal.save_loaded_asset(instance)


def build_lens(name, srgb, channel, collection):
    """Signal lens: dark glass when off, emissive by the instance's custom data value at index channel."""
    material = fresh_material(name)
    tint = unreal.LinearColor(*[c ** 2.2 for c in srgb], 1.0)
    tint_node = expr(material, unreal.MaterialExpressionConstant3Vector, -700, 0, constant=tint)
    intensity = expr(material, unreal.MaterialExpressionPerInstanceCustomData, -700, 150, data_index=channel, const_default_value=0.0)
    night = collection_scalar(material, collection, "Night", -700, 300)
    custom = expr(material, unreal.MaterialExpressionCustom, -400, 100, code=NIGHT_LENS_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="LensGlow",
                  inputs=[custom_input("Tint"), custom_input("Intensity"), custom_input("Night")])
    link(tint_node, "", custom, "Tint")
    link(intensity, "", custom, "Intensity")
    link(night, "", custom, "Night")
    base = expr(material, unreal.MaterialExpressionConstant3Vector, -400, -150,
                constant=unreal.LinearColor(tint.r * 0.08, tint.g * 0.08, tint.b * 0.08, 1))
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -400, 300, r=0.12), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)


def build_lamp_lens(collection):
    material = fresh_material("M_LampLens")
    tint = expr(material, unreal.MaterialExpressionConstant3Vector, -600, 0,
                constant=unreal.LinearColor(1.0, 0.72, 0.42, 1.0))
    night = collection_scalar(material, collection, "Night", -600, 150)
    custom = expr(material, unreal.MaterialExpressionCustom, -300, 50, code=LAMP_LENS_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="LampGlow",
                  inputs=[custom_input("Tint"), custom_input("Night")])
    link(tint, "", custom, "Tint")
    link(night, "", custom, "Night")
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant3Vector, -300, -150,
                                       constant=unreal.LinearColor(0.35, 0.33, 0.28, 1)), "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -300, 200, r=0.15), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)


def build_border(collection):
    """Retroreflective white-yellow border of the signal backplates."""
    material = fresh_material("M_SignalBackplateBorder")
    color = expr(material, unreal.MaterialExpressionConstant3Vector, -500, 0, constant=unreal.LinearColor(0.85, 0.83, 0.62, 1))
    mel.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -500, 200, r=0.3), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)


def build_sign_master(collection):
    """Sign face: the graphic on the front, galvanised grey on the back, alpha mask for the outline."""
    material = fresh_material("M_SignFace", blend_mode=unreal.BlendMode.BLEND_MASKED, two_sided=True)
    graphic = expr(material, unreal.MaterialExpressionTextureSampleParameter2D, -1100, 0, parameter_name="Graphic",
                   sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    sign_side = expr(material, unreal.MaterialExpressionTwoSidedSign, -1100, 250)
    front = expr(material, unreal.MaterialExpressionSaturate, -900, 250)
    link(sign_side, "", front, "")
    back_color = expr(material, unreal.MaterialExpressionConstant3Vector, -900, 100,
                      constant=unreal.LinearColor(0.38, 0.40, 0.41, 1))
    base = expr(material, unreal.MaterialExpressionLinearInterpolate, -650, 50)
    link(back_color, "", base, "A")
    link(graphic, "RGB", base, "B")
    link(front, "", base, "Alpha")
    rough = expr(material, unreal.MaterialExpressionLinearInterpolate, -650, 300, const_a=0.55, const_b=0.30)
    link(front, "", rough, "Alpha")
    metallic = expr(material, unreal.MaterialExpressionLinearInterpolate, -650, 400, const_a=0.8, const_b=0.0)
    link(front, "", metallic, "Alpha")

    names = ("HeadlightPosition", "HeadlightDirection")
    collection_nodes = {n: expr(material, unreal.MaterialExpressionCollectionParameter, -1100, 450 + 100 * i,
                                collection=collection, parameter_name=n) for i, n in enumerate(names)}
    on = collection_scalar(material, collection, "HeadlightOn", -1100, 700)
    candela = collection_scalar(material, collection, "HeadlightCandela", -1100, 800)
    world_position = expr(material, unreal.MaterialExpressionWorldPosition, -1100, 900)
    normal = expr(material, unreal.MaterialExpressionVertexNormalWS, -1100, 1000)
    retro = expr(material, unreal.MaterialExpressionCustom, -650, 600, code=SIGN_RETRO_HLSL,
                 output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="Retroreflection",
                 inputs=[custom_input(n) for n in ("Graphic", "WorldPosition", "HeadlightPosition", "HeadlightDirection",
                                                   "SignNormal", "HeadlightOn", "HeadlightCandela", "Front")])
    link(graphic, "RGB", retro, "Graphic")
    link(world_position, "", retro, "WorldPosition")
    link(collection_nodes["HeadlightPosition"], "", retro, "HeadlightPosition")
    link(collection_nodes["HeadlightDirection"], "", retro, "HeadlightDirection")
    link(normal, "", retro, "SignNormal")
    link(on, "", retro, "HeadlightOn")
    link(candela, "", retro, "HeadlightCandela")
    link(front, "", retro, "Front")
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)
    mel.connect_material_property(retro, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(graphic, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)


def import_models():
    """Imports every FBX from the model folder as a static mesh named after the file, replacing the old one."""
    tasks = []
    for path in sorted(glob.glob(os.path.join(MODEL_DIR, "SM_*.fbx"))):
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
        task.options = options
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)


def add_pole_collision(mesh, radius_cm, height_cm):
    """Replaces the mesh's collision with one upright capsule at the origin: poles are what the car can hit."""
    body_setup = mesh.get_editor_property("body_setup")
    geometry = body_setup.get_editor_property("agg_geom")
    capsule = unreal.KSphylElem()
    capsule.set_editor_property("center", unreal.Vector(0, 0, height_cm / 2))
    capsule.set_editor_property("radius", radius_cm)
    capsule.set_editor_property("length", max(height_cm - 2 * radius_cm, 1.0))
    geometry.set_editor_property("sphyl_elems", [capsule])
    geometry.set_editor_property("box_elems", [])
    geometry.set_editor_property("sphere_elems", [])
    geometry.set_editor_property("convex_elems", [])
    body_setup.set_editor_property("agg_geom", geometry)
    body_setup.set_editor_property("collision_trace_flag", unreal.CollisionTraceFlag.CTF_USE_SIMPLE_AS_COMPLEX)
    mesh.set_editor_property("body_setup", body_setup)


POLE_COLLISION = {"SM_StreetLamp": (10.0, 700.0), "SM_SignalPole": (6.0, 360.0), "SM_SignPole_300": (4.0, 300.0),
                  "SM_SignPole_350": (4.0, 350.0), "SM_SignPole_400": (4.0, 400.0)}


def assign_materials():
    """Gives each mesh slot the material of the same name (MI_<name> for solids, M_<name> otherwise)."""
    for path in eal.list_assets(f"{FOLDER}/Meshes"):
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            continue
        for index, static_material in enumerate(mesh.get_editor_property("static_materials")):
            slot = str(static_material.get_editor_property("material_slot_name"))
            candidates = [f"{FOLDER}/MI_{slot}", f"{FOLDER}/M_{slot}"]
            if slot == "SignFace":
                candidates = [f"{FOLDER}/M_SignFace"]
            found = next((c for c in candidates if eal.does_asset_exist(c)), None)
            if found is None:
                unreal.log_warning(f"furniture: no material for slot {slot} of {mesh.get_name()}")
                continue
            mesh.set_material(index, unreal.load_asset(found))
        if mesh.get_name() in POLE_COLLISION:
            add_pole_collision(mesh, *POLE_COLLISION[mesh.get_name()])
        eal.save_loaded_asset(mesh)


def import_signs():
    tasks = []
    for path in sorted(glob.glob(os.path.join(SIGN_DIR, "*.png"))):
        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = f"{FOLDER}/Signs"
        task.destination_name = "T_" + os.path.splitext(os.path.basename(path))[0].replace(".", "_")
        task.automated = True
        task.save = True
        task.replace_existing = True
        tasks.append(task)
    asset_tools.import_asset_tasks(tasks)
    for path in eal.list_assets(f"{FOLDER}/Signs"):
        texture = unreal.load_asset(path)
        if isinstance(texture, unreal.Texture2D):
            texture.set_editor_property("srgb", True)
            texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
            texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP)
            texture.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
            texture.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
            texture.set_editor_property("never_stream", False)
            eal.save_loaded_asset(texture)


def main():
    if eal.does_directory_exist(f"{FOLDER}/Test"):
        eal.delete_directory(f"{FOLDER}/Test")
    collection = create_collection()
    build_solid_instances(build_solid_master())
    for name, color, channel in (("M_SignalLensRed", (0.95, 0.04, 0.02), 0), ("M_SignalLensAmber", (1.0, 0.45, 0.02), 1),
                                 ("M_SignalLensGreen", (0.05, 0.9, 0.35), 2)):
        build_lens(name, color, channel, collection)
    build_lamp_lens(collection)
    build_border(collection)
    build_sign_master(collection)
    import_models()
    import_signs()
    assign_materials()
    unreal.log_warning("create_furniture_assets: done")


main()
