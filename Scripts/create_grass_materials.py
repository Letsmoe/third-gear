"""Creates the grass tuft materials in /Game/Grass/Materials: the master M_GrassTuft and one instance per kind of
grass (MI_GrassLawn, MI_GrassMeadow). FGrassField (Plugins/MapRuntime) puts them on the imported tuft meshes.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_grass_materials.py -unattended

Run Scripts/import_grass_models.py first. The photo texture of each tuft carries the blade shapes in its alpha
channel; the shader tints it to the ground colour, bends it with the weather's wind (DgGrassWind in
Plugins/MapRuntime/Shaders/Private/WindSway.ush) and shrinks tufts away with distance. The fade distances here must
stay inside the radii in FGrassField::LoadAssets. Rebuilt in place.
"""
import unreal

FOLDER = "/Game/Grass/Materials"
MASTER = f"{FOLDER}/M_GrassTuft"
COLLECTION = "/Game/World/MPC_Weather"
INCLUDE = "/Plugin/MapRuntime/Private/WindSway.ush"
TEXTURES = "/Game/Grass/Imported/{pack}/{pack}_2k/Textures"

# kind -> (texture pack, tint, fade start cm, fade end cm, mesh height cm that the sway is scaled by)
KINDS = {
    "Lawn": ("grass_medium_01", (2.0, 2.3, 1.2), 1500.0, 2900.0, 14.0),
    "LawnDense": ("grass_medium_01", (2.0, 2.3, 1.2), 500.0, 780.0, 10.0),
    "Meadow": ("grass_medium_02", (2.2, 2.4, 1.2), 2400.0, 4400.0, 26.0),
    "MeadowTall": ("grass_medium_01", (2.2, 2.4, 1.2), 2400.0, 3900.0, 40.0),
    "Dandelion": ("dandelion_01", (1.0, 1.0, 1.0), 1500.0, 2400.0, 20.0),
    "Celandine": ("celandine_01", (1.0, 1.0, 1.0), 1500.0, 2400.0, 20.0),
    "Weeds": ("weed_plant_02", (1.4, 1.5, 1.0), 800.0, 1150.0, 10.0),
}

WIND_HLSL = """
float3 up = LocalUp;
float tuftHeight = length(up) * MeshHeight;
return DgGrassWind(WorldPosition, TuftPosition, Camera, Seconds, InstanceRandom, WindSpeed * WindStrength,
	float2(WindX, WindY), tuftHeight, FadeStart, FadeEnd);
"""

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary


def expression(material, cls, x, y, **properties):
    """Adds a material expression with the given properties."""
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in properties.items():
        node.set_editor_property(key, value)
    return node


def scalar(material, name, default, x, y):
    """A named scalar parameter."""
    return expression(material, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=default)


def texture(material, name, sampler, x, y):
    """A named texture parameter of the given sampler type."""
    return expression(material, unreal.MaterialExpressionTextureSampleParameter2D, x, y, parameter_name=name,
                      sampler_type=sampler)


def custom(material, code, inputs, x, y):
    """A Custom HLSL node returning a float3 with the wind include."""
    node = expression(material, unreal.MaterialExpressionCustom, x, y, code=code,
                      output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, include_file_paths=[INCLUDE])
    entries = []
    for name in inputs:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", name)
        entries.append(entry)
    node.set_editor_property("inputs", entries)
    return node


def build_master():
    """Creates or rebuilds the master material."""
    collection = unreal.load_asset(COLLECTION)
    if eal.does_asset_exist(MASTER):
        material = unreal.load_asset(MASTER)
        mel.delete_all_material_expressions(material)
    else:
        eal.make_directory(FOLDER)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset("M_GrassTuft", FOLDER, unreal.Material,
                                                                           unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    material.set_editor_property("used_with_nanite", True)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    material.set_editor_property("max_world_position_offset_displacement", 60.0)

    uv = expression(material, unreal.MaterialExpressionTextureCoordinate, -1500, 0)
    colour = texture(material, "Colour", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, -1200, 0)
    normal = texture(material, "Normal", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, -1200, 300)
    rough = texture(material, "Roughness", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, -1200, 600)
    # Defaults only so the master compiles with the right sampler types; the instances bring their own textures.
    folder = TEXTURES.format(pack="grass_medium_01")
    for sampler, name in ((colour, "grass_medium_01_diff-grass_medium_01_alpha"), (normal, "grass_medium_01_nor_gl"),
                          (rough, "grass_medium_01_rough")):
        sampler.set_editor_property("texture", unreal.load_asset(f"{folder}/{name}"))
        mel.connect_material_expressions(uv, "", sampler, "UVs")

    tint = expression(material, unreal.MaterialExpressionVectorParameter, -1200, -250, parameter_name="Tint",
                      default_value=unreal.LinearColor(0.6, 0.7, 0.4, 1.0))
    random = expression(material, unreal.MaterialExpressionPerInstanceRandom, -1200, -100)
    # Brightness varies between tufts by about +-12 %.
    variation = expression(material, unreal.MaterialExpressionLinearInterpolate, -950, -100)
    low = expression(material, unreal.MaterialExpressionConstant, -1100, -50, r=0.88)
    high = expression(material, unreal.MaterialExpressionConstant, -1100, -20, r=1.12)
    mel.connect_material_expressions(low, "", variation, "A")
    mel.connect_material_expressions(high, "", variation, "B")
    mel.connect_material_expressions(random, "", variation, "Alpha")
    tinted = expression(material, unreal.MaterialExpressionMultiply, -800, 0)
    mel.connect_material_expressions(colour, "RGB", tinted, "A")
    mel.connect_material_expressions(tint, "", tinted, "B")
    base = expression(material, unreal.MaterialExpressionMultiply, -650, 0)
    mel.connect_material_expressions(tinted, "", base, "A")
    mel.connect_material_expressions(variation, "", base, "B")
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    # Light passing through blades.
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    mel.connect_material_property(colour, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    mel.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(rough, "R", unreal.MaterialProperty.MP_ROUGHNESS)

    x = -1500
    world_position = expression(material, unreal.MaterialExpressionWorldPosition, x, 900,
                                world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    tuft_position = expression(material, unreal.MaterialExpressionObjectPositionWS, x, 1000)
    up = expression(material, unreal.MaterialExpressionConstant3Vector, x, 1100, constant=unreal.LinearColor(0, 0, 1, 0))
    world_up = expression(material, unreal.MaterialExpressionTransform, x + 150, 1100,
                          transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL,
                          transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    mel.connect_material_expressions(up, "", world_up, "")
    inputs = ["WorldPosition", "TuftPosition", "LocalUp", "Seconds", "Camera", "InstanceRandom", "WindSpeed", "WindX",
              "WindY", "WindStrength", "MeshHeight", "FadeStart", "FadeEnd"]
    sources = [
        world_position, tuft_position, world_up,
        expression(material, unreal.MaterialExpressionTime, x, 1200),
        expression(material, unreal.MaterialExpressionCameraPositionWS, x, 1300),
        random,
    ]
    for name in ("WindSpeed", "WindDirectionX", "WindDirectionY"):
        sources.append(expression(material, unreal.MaterialExpressionCollectionParameter, x, 1400 + 100 * len(sources),
                                  collection=collection, parameter_name=name))
    sources += [scalar(material, "WindStrength", 1.0, x, 1800), scalar(material, "MeshHeight", 14.0, x, 1900),
                scalar(material, "FadeStart", 900.0, x, 2000), scalar(material, "FadeEnd", 1450.0, x, 2100)]
    wind = custom(material, WIND_HLSL, inputs, -800, 1300)
    for name, source in zip(inputs, sources):
        mel.connect_material_expressions(source, "", wind, name)
    mel.connect_material_property(wind, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def build_instances(master):
    """One instance per kind of grass, with its texture pack, tint and fade distances."""
    for kind, (pack, tint, fade_start, fade_end, mesh_height) in KINDS.items():
        path = f"{FOLDER}/MI_Grass{kind}"
        if eal.does_asset_exist(path):
            eal.delete_asset(path)
        instance = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            f"MI_Grass{kind}", FOLDER, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        instance.set_editor_property("parent", master)
        folder = TEXTURES.format(pack=pack)
        for parameter, name in (("Colour", f"{pack}_diff-{pack}_alpha"), ("Normal", f"{pack}_nor_gl"),
                                ("Roughness", f"{pack}_rough")):
            loaded = unreal.load_asset(f"{folder}/{name}")
            if loaded is None:
                unreal.log_error(f"grass materials: texture {folder}/{name} missing")
                continue
            mel.set_material_instance_texture_parameter_value(instance, parameter, loaded)
        mel.set_material_instance_vector_parameter_value(instance, "Tint", unreal.LinearColor(*tint, 1.0))
        mel.set_material_instance_scalar_parameter_value(instance, "FadeStart", fade_start)
        mel.set_material_instance_scalar_parameter_value(instance, "FadeEnd", fade_end)
        mel.set_material_instance_scalar_parameter_value(instance, "MeshHeight", mesh_height)
        mel.update_material_instance(instance)
        eal.save_loaded_asset(instance)
        unreal.log_warning(f"grass materials: {path} built")


build_instances(build_master())
