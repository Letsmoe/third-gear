"""Creates the dome material of the cached cloud sky (see Source/DrivingGame/CloudSky.h) in /Game/World/Sky.

  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_cloud_sky.py -unattended

M_SkyDome is the sky material of the dome mesh. In the views it shows the six faces the game renders on demand (sky and
volumetric clouds in `UCloudSkyRig`), fading between two sets of faces while a refresh comes in. In the sky light's
real-time capture it draws the atmosphere instead, because with a sky mesh in the scene the engine no longer does.
Re-running rebuilds the material in place.
"""
import math

import unreal

FOLDER = "/Game/World/Sky"
DOME = "M_SkyDome"
FACE_FIELD_OF_VIEW_DEGREES = 92.0
mel = unreal.MaterialEditingLibrary

# Face order matches UCloudSkyRig: +X, -X, +Y, -Y, +Z, -Z. Each face is a camera looking along that axis; the texture
# coordinate of a direction is its position on the face plane, (right, -up) over the forward distance.
FACE_SAMPLING_HLSL = """
float3 d = normalize(Dir);
float3 a = abs(d);
float scale = %(scale)f;
float2 uv = float2(0, 0);
float4 c = float4(0, 0, 0, 0);
if (a.x >= a.y && a.x >= a.z)
{
    if (d.x > 0) { uv = float2(d.y, -d.z) / d.x * scale + 0.5; c = T0.SampleLevel(T0Sampler, uv, 0); }
    else         { uv = float2(d.y,  d.z) / d.x * scale + 0.5; c = T1.SampleLevel(T1Sampler, uv, 0); }
}
else if (a.y >= a.z)
{
    if (d.y > 0) { uv = float2(-d.x, -d.z) / d.y * scale + 0.5; c = T2.SampleLevel(T2Sampler, uv, 0); }
    else         { uv = float2(-d.x,  d.z) / d.y * scale + 0.5; c = T3.SampleLevel(T3Sampler, uv, 0); }
}
else
{
    if (d.z > 0) { uv = float2(d.y, d.x) / d.z * scale + 0.5; c = T4.SampleLevel(T4Sampler, uv, 0); }
    else         { uv = float2(-d.y, d.x) / d.z * scale + 0.5; c = T5.SampleLevel(T5Sampler, uv, 0); }
}
return c.rgb;
"""


def get_or_create_material(name):
    """Returns the material asset, emptied so the graph can be rebuilt."""
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        material = unreal.load_asset(path)
        mel.delete_all_material_expressions(material)
        return material
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    return tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


def add(material, expression_class, x, y):
    """Adds an expression node at a grid position."""
    return mel.create_material_expression(material, expression_class, x * 250, y * 160)


def add_face_set(material, prefix, direction, row):
    """Adds a custom node sampling the six faces named <prefix>0 to <prefix>5 in a view direction; returns it."""
    scale = 0.5 / math.tan(math.radians(FACE_FIELD_OF_VIEW_DEGREES) / 2.0)
    custom = add(material, unreal.MaterialExpressionCustom, 3, row)
    custom.set_editor_property("code", FACE_SAMPLING_HLSL % {"scale": scale})
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", f"Sky faces {prefix}")
    inputs = []
    for input_name in ["Dir"] + [f"T{face}" for face in range(6)]:
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", input_name)
        inputs.append(custom_input)
    custom.set_editor_property("inputs", inputs)
    mel.connect_material_expressions(direction, "", custom, "Dir")
    placeholder = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    for face in range(6):
        parameter = add(material, unreal.MaterialExpressionTextureObjectParameter, 1, row * 3 + face)
        parameter.set_editor_property("parameter_name", f"{prefix}{face}")
        parameter.set_editor_property("texture", placeholder)
        mel.connect_material_expressions(parameter, "", custom, f"T{face}")
    return custom


def build_dome():
    """Unlit two-sided sky material: the shown faces in the views, the atmosphere in the sky light capture."""
    material = get_or_create_material(DOME)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("is_sky", True)

    world_position = add(material, unreal.MaterialExpressionWorldPosition, 0, 0)
    camera_position = add(material, unreal.MaterialExpressionCameraPositionWS, 0, 1)
    difference = add(material, unreal.MaterialExpressionSubtract, 1, 0)
    mel.connect_material_expressions(world_position, "", difference, "A")
    mel.connect_material_expressions(camera_position, "", difference, "B")
    direction = add(material, unreal.MaterialExpressionNormalize, 2, 0)
    mel.connect_material_expressions(difference, "", direction, "")

    faces_a = add_face_set(material, "A", direction, 1)
    faces_b = add_face_set(material, "B", direction, 3)
    blend = add(material, unreal.MaterialExpressionScalarParameter, 4, 0)
    blend.set_editor_property("parameter_name", "Blend")
    blend.set_editor_property("default_value", 0.0)
    mixed = add(material, unreal.MaterialExpressionLinearInterpolate, 5, 1)
    mel.connect_material_expressions(faces_a, "", mixed, "A")
    mel.connect_material_expressions(faces_b, "", mixed, "B")
    mel.connect_material_expressions(blend, "", mixed, "Alpha")
    brightness = add(material, unreal.MaterialExpressionScalarParameter, 5, 0)
    brightness.set_editor_property("parameter_name", "Brightness")
    brightness.set_editor_property("default_value", 1.0)
    shown = add(material, unreal.MaterialExpressionMultiply, 6, 1)
    mel.connect_material_expressions(mixed, "", shown, "A")
    mel.connect_material_expressions(brightness, "", shown, "B")

    # With a sky mesh in the scene the engine no longer draws the atmosphere itself, neither in the view nor into the
    # sky light capture, so the dome draws it there.
    atmosphere = add(material, unreal.MaterialExpressionSkyAtmosphereViewLuminance, 6, 3)
    pass_switch = add(material, unreal.MaterialExpressionReflectionCapturePassSwitch, 7, 2)
    mel.connect_material_expressions(shown, "", pass_switch, "Default")
    mel.connect_material_expressions(atmosphere, "", pass_switch, "Reflection")
    mel.connect_material_property(pass_switch, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)


build_dome()
unreal.log_warning("create_cloud_sky: done")
