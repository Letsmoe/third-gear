"""Creates the materials of the cached cloud sky (see Source/DrivingGame/CloudSky.h) in /Game/World/Sky.

  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_cloud_sky.py -unattended

M_SkyDome is the sky material of the dome mesh: in the views it shows the sky light's real-time capture (sky atmosphere
and volumetric clouds, time-sliced by the engine) instead of tracing anything per view; in the capture itself it draws
the atmosphere.
Re-running rebuilds both materials in place.
"""
import unreal

FOLDER = "/Game/World/Sky"
DOME = "M_SkyDome"
mel = unreal.MaterialEditingLibrary


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


SUN_SOLID_ANGLE_STERADIANS = 6.8e-5
# The true luminance (over 1e9 cd/m2) would overflow the half-float scene colour after pre-exposure; the disk is
# white at a hundredth of it just as well.
SUN_DISK_GAIN = 0.01
# Cosines of the disk's soft edge: the sun is 0.53 degrees across.
SUN_EDGE_INNER_COS = 0.99999048  # 0.25 degrees
SUN_EDGE_OUTER_COS = 0.99998629  # 0.30 degrees


def add_sun_disk(material, direction, sky):
    """Adds the sun's disk to the sky. The capture leaves it out (it would double the sun's specular light), so the
    dome draws it: the atmosphere light's illuminance with its atmospheric colour, divided by the disk's solid
    angle, inside a small angle around SunDirection and scaled by SunVisibility (cloud in front of it)."""
    sun_direction = add(material, unreal.MaterialExpressionVectorParameter, 4, 3)
    sun_direction.set_editor_property("parameter_name", "SunDirection")
    sun_direction.set_editor_property("default_value", unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
    alignment = add(material, unreal.MaterialExpressionDotProduct, 5, 3)
    mel.connect_material_expressions(direction, "", alignment, "A")
    mel.connect_material_expressions(sun_direction, "", alignment, "B")
    inner = add(material, unreal.MaterialExpressionConstant, 5, 4)
    inner.set_editor_property("r", SUN_EDGE_INNER_COS)
    outer = add(material, unreal.MaterialExpressionConstant, 5, 5)
    outer.set_editor_property("r", SUN_EDGE_OUTER_COS)
    edge = add(material, unreal.MaterialExpressionSmoothStep, 6, 3)
    mel.connect_material_expressions(outer, "", edge, "Min")
    mel.connect_material_expressions(inner, "", edge, "Max")
    mel.connect_material_expressions(alignment, "", edge, "Value")

    camera_position = add(material, unreal.MaterialExpressionCameraPositionWS, 6, 4)
    illuminance = add(material, unreal.MaterialExpressionSkyAtmosphereLightIlluminance, 7, 4)
    mel.connect_material_expressions(camera_position, "", illuminance, "WorldPosition")
    inverse_angle = add(material, unreal.MaterialExpressionConstant, 7, 5)
    inverse_angle.set_editor_property("r", SUN_DISK_GAIN / SUN_SOLID_ANGLE_STERADIANS)
    visibility = add(material, unreal.MaterialExpressionScalarParameter, 7, 6)
    visibility.set_editor_property("parameter_name", "SunVisibility")
    visibility.set_editor_property("default_value", 1.0)

    disk = add(material, unreal.MaterialExpressionMultiply, 8, 3)
    mel.connect_material_expressions(edge, "", disk, "A")
    mel.connect_material_expressions(illuminance, "", disk, "B")
    disk_scaled = add(material, unreal.MaterialExpressionMultiply, 9, 3)
    mel.connect_material_expressions(disk, "", disk_scaled, "A")
    mel.connect_material_expressions(inverse_angle, "", disk_scaled, "B")
    disk_visible = add(material, unreal.MaterialExpressionMultiply, 10, 3)
    mel.connect_material_expressions(disk_scaled, "", disk_visible, "A")
    mel.connect_material_expressions(visibility, "", disk_visible, "B")
    total = add(material, unreal.MaterialExpressionAdd, 11, 0)
    mel.connect_material_expressions(sky, "", total, "A")
    mel.connect_material_expressions(disk_visible, "", total, "B")
    return total


DITHER_AMPLITUDE = 0.012


def add_dither(material, color):
    """Multiplies the colour by 1 plus a little per-pixel noise that changes every frame. The capture is stored in
    R11G11B10 (5 to 6 bit mantissas), so a smooth sky shows steps and hue bands; the dither hides them, and the
    temporal anti-aliasing averages it out."""
    screen = add(material, unreal.MaterialExpressionScreenPosition, 0, 4)
    pixel_scale = add(material, unreal.MaterialExpressionConstant, 0, 5)
    pixel_scale.set_editor_property("r", 3000.0)
    pixels = add(material, unreal.MaterialExpressionMultiply, 1, 4)
    mel.connect_material_expressions(screen, "ViewportUV", pixels, "A")
    mel.connect_material_expressions(pixel_scale, "", pixels, "B")
    time = add(material, unreal.MaterialExpressionTime, 0, 6)
    time_scale = add(material, unreal.MaterialExpressionConstant, 0, 7)
    time_scale.set_editor_property("r", 60.0)
    moving = add(material, unreal.MaterialExpressionMultiply, 1, 6)
    mel.connect_material_expressions(time, "", moving, "A")
    mel.connect_material_expressions(time_scale, "", moving, "B")
    position = add(material, unreal.MaterialExpressionAppendVector, 2, 4)
    mel.connect_material_expressions(pixels, "", position, "A")
    mel.connect_material_expressions(moving, "", position, "B")
    noise = add(material, unreal.MaterialExpressionNoise, 3, 4)
    noise.set_editor_property("scale", 1.0)
    noise.set_editor_property("quality", 1)
    noise.set_editor_property("noise_function", unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_ALU)
    noise.set_editor_property("output_min", -1.0)
    noise.set_editor_property("output_max", 1.0)
    mel.connect_material_expressions(position, "", noise, "Position")
    amplitude = add(material, unreal.MaterialExpressionConstant, 3, 5)
    amplitude.set_editor_property("r", DITHER_AMPLITUDE)
    swing = add(material, unreal.MaterialExpressionMultiply, 4, 4)
    mel.connect_material_expressions(noise, "", swing, "A")
    mel.connect_material_expressions(amplitude, "", swing, "B")
    one = add(material, unreal.MaterialExpressionConstant, 4, 5)
    one.set_editor_property("r", 1.0)
    factor = add(material, unreal.MaterialExpressionAdd, 5, 4)
    mel.connect_material_expressions(swing, "", factor, "A")
    mel.connect_material_expressions(one, "", factor, "B")
    result = add(material, unreal.MaterialExpressionMultiply, 6, 4)
    mel.connect_material_expressions(color, "", result, "A")
    mel.connect_material_expressions(factor, "", result, "B")
    return result


def build_dome():
    """Unlit two-sided sky material: emissive = captured sky in the view direction times a brightness parameter."""
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

    roughness = add(material, unreal.MaterialExpressionConstant, 2, 1)
    roughness.set_editor_property("r", 0.0)
    sample = add(material, unreal.MaterialExpressionSkyLightEnvMapSample, 3, 0)
    mel.connect_material_expressions(direction, "", sample, "Direction")
    mel.connect_material_expressions(roughness, "", sample, "Roughness")

    try:
        dithered = add_dither(material, sample)
    except Exception as error:  # a node class or property that differs between engine versions
        unreal.log_warning(f"create_cloud_sky: dither skipped: {error}")
        dithered = sample

    brightness = add(material, unreal.MaterialExpressionScalarParameter, 3, 1)
    brightness.set_editor_property("parameter_name", "Brightness")
    brightness.set_editor_property("default_value", 1.0)
    scaled = add(material, unreal.MaterialExpressionMultiply, 4, 0)
    mel.connect_material_expressions(dithered, "", scaled, "A")
    mel.connect_material_expressions(brightness, "", scaled, "B")
    sky_and_sun = add_sun_disk(material, direction, scaled)

    # With a sky mesh in the scene the engine no longer draws the atmosphere itself, neither in the view nor into the
    # sky light capture, so the dome has to: in the capture it shows the atmosphere (the clouds are traced over it
    # afterwards), in the view it shows the capture, atmosphere and clouds together.
    atmosphere = add(material, unreal.MaterialExpressionSkyAtmosphereViewLuminance, 11, 2)
    pass_switch = add(material, unreal.MaterialExpressionReflectionCapturePassSwitch, 12, 1)
    mel.connect_material_expressions(sky_and_sun, "", pass_switch, "Default")
    mel.connect_material_expressions(atmosphere, "", pass_switch, "Reflection")
    mel.connect_material_property(pass_switch, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)


build_dome()
unreal.log_warning("create_cloud_sky: done")
