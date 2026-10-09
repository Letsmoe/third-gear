"""Creates /Game/Vehicles/Materials/M_ClusterScreen, the material of the instrument cluster's display (headless editor script).

The cluster is painted with Slate into a render target (UInstrumentClusterComponent) that arrives as the texture parameter
"ClusterImage". The material is unlit and opaque. The scene's exposure is locked (EV100 13 by day), so the colour is
multiplied by "Gain", the inverse of the exposure scale 1.2 * 2^EV100, which the component sets every frame from the
console variable dg.MenuPanelExposureEv, the same way as the VR menu panel. "Brightness" is the display's white as a
fraction of display white.

Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

DST = "/Game/Vehicles/Materials/M_ClusterScreen"
mel = unreal.MaterialEditingLibrary
if unreal.EditorAssetLibrary.does_asset_exist(DST):
    unreal.EditorAssetLibrary.delete_asset(DST)
package_path, name = DST.rsplit("/", 1)
material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, package_path, unreal.Material, unreal.MaterialFactoryNew())
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property("two_sided", True)


def expr(cls, x, y, **props):
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def link(a, a_out, b, b_in):
    if not mel.connect_material_expressions(a, a_out, b, b_in):
        raise RuntimeError(f"failed to connect {a.get_name()}.{a_out} -> {b.get_name()}.{b_in}")


uv = expr(unreal.MaterialExpressionTextureCoordinate, -1100, 0)
flip_u = expr(unreal.MaterialExpressionScalarParameter, -1100, 200, parameter_name="FlipU", default_value=0.0)
flip_v = expr(unreal.MaterialExpressionScalarParameter, -1100, 300, parameter_name="FlipV", default_value=0.0)
mask_u = expr(unreal.MaterialExpressionComponentMask, -900, -50, r=True, g=False, b=False, a=False)
mask_v = expr(unreal.MaterialExpressionComponentMask, -900, 50, r=False, g=True, b=False, a=False)
link(uv, "", mask_u, "")
link(uv, "", mask_v, "")
one_minus_u = expr(unreal.MaterialExpressionOneMinus, -700, -50)
one_minus_v = expr(unreal.MaterialExpressionOneMinus, -700, 50)
link(mask_u, "", one_minus_u, "")
link(mask_v, "", one_minus_v, "")
lerp_u = expr(unreal.MaterialExpressionLinearInterpolate, -500, -50)
lerp_v = expr(unreal.MaterialExpressionLinearInterpolate, -500, 50)
link(mask_u, "", lerp_u, "A")
link(one_minus_u, "", lerp_u, "B")
link(flip_u, "", lerp_u, "Alpha")
link(mask_v, "", lerp_v, "A")
link(one_minus_v, "", lerp_v, "B")
link(flip_v, "", lerp_v, "Alpha")
append = expr(unreal.MaterialExpressionAppendVector, -300, 0)
link(lerp_u, "", append, "A")
link(lerp_v, "", append, "B")

image = expr(unreal.MaterialExpressionTextureSampleParameter2D, -100, 0, parameter_name="ClusterImage",
             sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
link(append, "", image, "UVs")
brightness = expr(unreal.MaterialExpressionScalarParameter, -100, 250, parameter_name="Brightness", default_value=0.9)
gain = expr(unreal.MaterialExpressionScalarParameter, -100, 350, parameter_name="Gain", default_value=1.2 * 2 ** 13)
scaled = expr(unreal.MaterialExpressionMultiply, 150, 0)
link(image, "RGB", scaled, "A")
link(brightness, "", scaled, "B")
exposed = expr(unreal.MaterialExpressionMultiply, 350, 0)
link(scaled, "", exposed, "A")
link(gain, "", exposed, "B")
mel.connect_material_property(exposed, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log(f"Created {DST}")
