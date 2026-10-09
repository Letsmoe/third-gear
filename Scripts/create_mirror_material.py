"""Creates /Game/Vehicles/Materials/M_MirrorGlass, the glass of the car's mirrors (headless editor script).

The glass is an unlit masked quad that shows a scene capture (texture parameter "MirrorImage", written by
UCarMirrorsComponent). The capture holds scene radiance, so the quad is a plain emissive and gets the same exposure as
the world around it. A rounded-rectangle mask cuts the glass outline and a slight darkening towards the rim imitates the
bevelled edge; "Reflectivity" is the share of light a real mirror returns (about 0.9).

Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

DST = "/Game/Vehicles/Materials/M_MirrorGlass"

# Signed distance (cm) from the glass outline, negative inside. UV 0..1 over the quad of size Aspect*Height x Height.
OUTLINE_HLSL = """
float2 HalfSize = float2(Aspect * Height, Height) * 0.5;
float2 Position = (UV - 0.5) * HalfSize * 2.0;
float2 Corner = abs(Position) - HalfSize + Radius;
return length(max(Corner, 0.0)) + min(max(Corner.x, Corner.y), 0.0) - Radius;
"""

IMAGE_HLSL = """
float2 ImageUV = float2(lerp(UV.x, 1.0 - UV.x, FlipU), lerp(UV.y, 1.0 - UV.y, FlipV));
float3 Tint = float3(0.93, 0.97, 1.0);
float Rim = lerp(0.8, 1.0, saturate(-Outline / 0.8));
return Image.SampleLevel(ImageSampler, ImageUV, 0).rgb * Tint * Reflectivity * Rim * Gain;
"""

mel = unreal.MaterialEditingLibrary
if unreal.EditorAssetLibrary.does_asset_exist(DST):
    unreal.EditorAssetLibrary.delete_asset(DST)
package_path, name = DST.rsplit("/", 1)
material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, package_path, unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
material.set_editor_property("two_sided", True)


def expr(cls, x, y, **props):
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def scalar(parameter, default, y):
    return expr(unreal.MaterialExpressionScalarParameter, -900, y, parameter_name=parameter, default_value=default)


def link(source, source_output, target, target_input):
    if not mel.connect_material_expressions(source, source_output, target, target_input):
        raise RuntimeError(f"failed to connect {source.get_name()}.{source_output} -> {target.get_name()}.{target_input}")


def custom_input(input_name):
    entry = unreal.CustomInput()
    entry.set_editor_property("input_name", input_name)
    return entry


uv = expr(unreal.MaterialExpressionTextureCoordinate, -900, -200)
uv_mask = expr(unreal.MaterialExpressionComponentMask, -700, -200, r=True, g=True, b=False, a=False)
link(uv, "", uv_mask, "")
aspect = scalar("AspectRatio", 3.5, -50)
height = scalar("HeightCm", 5.0, 50)
radius = scalar("CornerRadiusCm", 1.5, 150)
outline = expr(unreal.MaterialExpressionCustom, -400, 0, code=OUTLINE_HLSL, description="GlassOutline",
               output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT1,
               inputs=[custom_input("UV"), custom_input("Aspect"), custom_input("Height"), custom_input("Radius")])
link(uv_mask, "", outline, "UV")
link(aspect, "", outline, "Aspect")
link(height, "", outline, "Height")
link(radius, "", outline, "Radius")

image = expr(unreal.MaterialExpressionTextureObjectParameter, -900, 300, parameter_name="MirrorImage",
             sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
flip_u = scalar("FlipU", 1.0, 400)
flip_v = scalar("FlipV", 0.0, 480)
reflectivity = scalar("Reflectivity", 0.9, 560)
gain = scalar("Gain", 1.0, 640)
colour = expr(unreal.MaterialExpressionCustom, -400, 300, code=IMAGE_HLSL, description="MirrorColour",
              output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
              inputs=[custom_input("Image"), custom_input("UV"), custom_input("FlipU"), custom_input("FlipV"),
                      custom_input("Reflectivity"), custom_input("Gain"), custom_input("Outline")])
link(image, "", colour, "Image")
link(uv_mask, "", colour, "UV")
link(flip_u, "", colour, "FlipU")
link(flip_v, "", colour, "FlipV")
link(reflectivity, "", colour, "Reflectivity")
link(gain, "", colour, "Gain")
link(outline, "", colour, "Outline")

# Opacity mask: 1 inside the outline, 0 outside (clip value 0.5 at the outline itself).
mask = expr(unreal.MaterialExpressionSaturate, -150, 0)
scaled = expr(unreal.MaterialExpressionMultiply, -300, 0, const_b=-20.0)
half = expr(unreal.MaterialExpressionAdd, -220, 0, const_b=0.5)
link(outline, "", scaled, "A")
link(scaled, "", half, "A")
link(half, "", mask, "")
mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.connect_material_property(mask, "", unreal.MaterialProperty.MP_OPACITY_MASK)
mel.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log(f"Created {DST}")
