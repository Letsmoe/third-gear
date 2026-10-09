"""Creates /Game/World/Materials/M_RainStreaks, the material that animates ARainEffect's streak mesh.

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


build()
