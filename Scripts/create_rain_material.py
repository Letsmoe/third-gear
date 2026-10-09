"""Creates /Game/World/Materials/M_RainStreaks and M_FallingLeaves, the materials that animate ARainEffect's streak mesh.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_rain_material.py -unattended

Every streak vertex carries its corner (UV0), its start in the rain box (UV1 = x, y and UV2.x = z, as box fractions)
and a random number (UV2.y). The vertex shader moves the start down at the fall speed and with the wind, wraps it into
a box around the camera, and spans a thin quad along the fall direction facing the viewer. A streak shows when its
random number is below the rain intensity from /Game/World/MPC_Weather. Rebuilt in place.
"""
import unreal

PATH = "/Game/World/Materials/M_RainStreaks"
COLLECTION = "/Game/World/MPC_Weather"

# Box size (cm) must match RainEffectDetail in Source/DrivingGame/RainEffect.cpp.
POSITION_HLSL = """
float3 box = float3(2400.0, 2400.0, 1200.0);
// Raindrops of 2 to 3 mm fall at about 7 m/s and drift with the wind.
float fall = lerp(650.0, 850.0, StartZR.y);
float3 velocity = float3(WindX * WindSpeed * 90.0, WindY * WindSpeed * 90.0, -fall);
float3 start = float3(StartXY, StartZR.x) * box;
float3 relative = start + velocity * Seconds - Camera;
relative = (frac(relative / box + 0.5) - 0.5) * box;
float shown = step(StartZR.y, Rain);
float3 along = normalize(velocity);
float3 toCamera = normalize(-relative + float3(0.001, 0.0, 0.0));
float3 side = normalize(cross(along, toCamera));
// A drop smears over about 1/30 s in the eye. Real drops are 2 to 3 mm, but a streak that thin is far below a
// pixel in the headset and vanishes; 4 mm plus a little per metre of distance keeps it at about a pixel.
float streakLength = fall / 30.0;
float width = 0.4 + 0.004 * length(relative);
float3 world = Camera + relative + (side * Corner.x * width - along * Corner.y * streakLength) * shown;
return world - Position;
"""

OPACITY_HLSL = """
float edge = saturate(1.0 - abs(Corner.x) * 2.0);
float tail = 1.0 - Corner.y * 0.7;
return edge * tail * 0.3;
"""

LEAVES_PATH = "/Game/World/Materials/M_FallingLeaves"

# Autumn leaves around the viewer on the same streak mesh (ARainEffect with fewer quads): they fall at 0.8 to
# 1.5 m/s, drift strongly with the wind, flutter from side to side and tumble about a turning axis.
LEAVES_POSITION_HLSL = """
float3 box = float3(2400.0, 2400.0, 1200.0);
float r = StartZR.y;
float fall = lerp(80.0, 150.0, r);
float3 velocity = float3(WindX * WindSpeed * 70.0, WindY * WindSpeed * 70.0, -fall);
float phase = r * 40.0;
float3 flutter = float3(sin(Seconds * 2.1 + phase), cos(Seconds * 1.7 + phase * 1.3), 0.0) * 60.0;
float3 start = float3(StartXY, StartZR.x) * box;
float3 relative = start + velocity * Seconds + flutter - Camera;
relative = (frac(relative / box + 0.5) - 0.5) * box;
float shown = step(r, LeafFall);
float angle = Seconds * lerp(2.0, 6.0, frac(r * 13.0)) + phase;
float3 across = normalize(float3(cos(angle), sin(angle * 0.7), sin(angle)));
float3 along = normalize(cross(across, float3(sin(angle * 1.3), cos(angle), 0.4)));
float size = lerp(4.0, 7.0, frac(r * 31.0));
float3 world = Camera + relative + (across * Corner.x + along * (Corner.y - 0.5)) * size * shown;
return world - Position;
"""

# A leaf outline: a pointed ellipse with a stalk, cut out of the quad.
LEAVES_MASK_HLSL = """
float2 p = float2(Corner.x * 2.0, (Corner.y - 0.5) * 2.0);
float width = 0.62 * (1.0 - p.y * p.y) * (0.75 + 0.25 * cos(p.y * 3.0));
float blade = step(abs(p.x), width) * step(p.y, 0.92);
float stalk = step(abs(p.x), 0.04) * step(0.85, p.y);
return saturate(blade + stalk);
"""

# Beech, oak, maple and lime in October: yellow, orange, rust and brown, darker at the veins.
LEAVES_COLOUR_HLSL = """
float r = frac(Random * 57.3);
float3 colour = lerp(float3(0.45, 0.33, 0.05), float3(0.42, 0.14, 0.03), saturate(r * 2.0));
colour = lerp(colour, float3(0.2, 0.11, 0.05), saturate(r * 2.0 - 1.0));
float vein = 1.0 - 0.35 * saturate(1.0 - abs(Corner.x) * 30.0);
return colour * vein;
"""

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary


def expression(material, cls, x, y, **properties):
    """Adds a material expression with the given properties."""
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in properties.items():
        node.set_editor_property(key, value)
    return node


def custom(material, code, inputs, output_type, x, y):
    """A Custom HLSL node with named inputs."""
    node = expression(material, unreal.MaterialExpressionCustom, x, y, code=code, output_type=output_type)
    names = []
    for name in inputs:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", name)
        names.append(entry)
    node.set_editor_property("inputs", names)
    return node


def texcoord(material, index, x, y):
    """UV channel `index`."""
    return expression(material, unreal.MaterialExpressionTextureCoordinate, x, y, coordinate_index=index)


def collection_parameter(material, collection, name, x, y):
    """A scalar from the weather parameter collection."""
    return expression(material, unreal.MaterialExpressionCollectionParameter, x, y, collection=collection,
                      parameter_name=name)


def build():
    """Creates or rebuilds the material."""
    collection = unreal.load_asset(COLLECTION)
    if collection is None:
        raise RuntimeError(f"{COLLECTION} missing; run Scripts/create_weather_parameters.py first")
    if eal.does_asset_exist(PATH):
        material = unreal.load_asset(PATH)
        mel.delete_all_material_expressions(material)
    else:
        folder, name = PATH.rsplit("/", 1)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Material,
                                                                           unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("translucency_lighting_mode",
                                 unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL)
    material.set_editor_property("two_sided", True)

    corner = texcoord(material, 0, -1400, 0)
    start_xy = texcoord(material, 1, -1400, 100)
    start_zr = texcoord(material, 2, -1400, 200)
    seconds = expression(material, unreal.MaterialExpressionTime, -1400, 300)
    camera = expression(material, unreal.MaterialExpressionCameraPositionWS, -1400, 400)
    position = expression(material, unreal.MaterialExpressionWorldPosition, -1400, 500,
                          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    rain = collection_parameter(material, collection, "RainIntensity", -1400, 600)
    wind_x = collection_parameter(material, collection, "WindDirectionX", -1400, 700)
    wind_y = collection_parameter(material, collection, "WindDirectionY", -1400, 800)
    wind_speed = collection_parameter(material, collection, "WindSpeed", -1400, 900)

    inputs = ["Corner", "StartXY", "StartZR", "Seconds", "Camera", "Position", "Rain", "WindX", "WindY", "WindSpeed"]
    offset = custom(material, POSITION_HLSL, inputs, unreal.CustomMaterialOutputType.CMOT_FLOAT3, -900, 300)
    sources = [corner, start_xy, start_zr, seconds, camera, position, rain, wind_x, wind_y, wind_speed]
    for index, source in enumerate(sources):
        mel.connect_material_expressions(source, "", offset, inputs[index])
    mel.connect_material_property(offset, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    opacity = custom(material, OPACITY_HLSL, ["Corner"], unreal.CustomMaterialOutputType.CMOT_FLOAT1, -900, -100)
    mel.connect_material_expressions(corner, "", opacity, "Corner")
    mel.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    # Water lit by the sky: near white, glossy, so streaks catch the light against dark backgrounds.
    colour = expression(material, unreal.MaterialExpressionConstant3Vector, -400, -300,
                        constant=unreal.LinearColor(0.8, 0.82, 0.85, 1.0))
    mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = expression(material, unreal.MaterialExpressionConstant, -400, -200, r=0.1)
    mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    unreal.log_warning(f"create_rain_material: {PATH} built")


def build_leaves():
    """Creates or rebuilds the falling leaves material (masked, two-sided, lit)."""
    collection = unreal.load_asset(COLLECTION)
    if eal.does_asset_exist(LEAVES_PATH):
        material = unreal.load_asset(LEAVES_PATH)
        mel.delete_all_material_expressions(material)
    else:
        folder, name = LEAVES_PATH.rsplit("/", 1)
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, folder, unreal.Material,
                                                                           unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("two_sided", True)

    corner = texcoord(material, 0, -1400, 0)
    start_xy = texcoord(material, 1, -1400, 100)
    start_zr = texcoord(material, 2, -1400, 200)
    seconds = expression(material, unreal.MaterialExpressionTime, -1400, 300)
    camera = expression(material, unreal.MaterialExpressionCameraPositionWS, -1400, 400)
    position = expression(material, unreal.MaterialExpressionWorldPosition, -1400, 500,
                          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    fall = collection_parameter(material, collection, "LeafFall", -1400, 600)
    wind_x = collection_parameter(material, collection, "WindDirectionX", -1400, 700)
    wind_y = collection_parameter(material, collection, "WindDirectionY", -1400, 800)
    wind_speed = collection_parameter(material, collection, "WindSpeed", -1400, 900)
    inputs = ["Corner", "StartXY", "StartZR", "Seconds", "Camera", "Position", "LeafFall", "WindX", "WindY",
              "WindSpeed"]
    offset = custom(material, LEAVES_POSITION_HLSL, inputs, unreal.CustomMaterialOutputType.CMOT_FLOAT3, -900, 300)
    for index, source in enumerate([corner, start_xy, start_zr, seconds, camera, position, fall, wind_x, wind_y,
                                    wind_speed]):
        mel.connect_material_expressions(source, "", offset, inputs[index])
    mel.connect_material_property(offset, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    mask = custom(material, LEAVES_MASK_HLSL, ["Corner"], unreal.CustomMaterialOutputType.CMOT_FLOAT1, -900, -100)
    mel.connect_material_expressions(corner, "", mask, "Corner")
    mel.connect_material_property(mask, "", unreal.MaterialProperty.MP_OPACITY_MASK)

    random = expression(material, unreal.MaterialExpressionComponentMask, -1150, -300, r=False, g=True, b=False,
                        a=False)
    mel.connect_material_expressions(start_zr, "", random, "")
    colour = custom(material, LEAVES_COLOUR_HLSL, ["Corner", "Random"], unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                    -900, -300)
    mel.connect_material_expressions(corner, "", colour, "Corner")
    mel.connect_material_expressions(random, "", colour, "Random")
    mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    roughness = expression(material, unreal.MaterialExpressionConstant, -400, -200, r=0.6)
    mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    unreal.log_warning(f"create_rain_material: {LEAVES_PATH} built")


build()
build_leaves()
