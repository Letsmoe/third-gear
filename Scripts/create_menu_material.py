"""Creates /Game/UI/M_MenuPanel, the material of the menu panel that floats in front of the driver in VR (headless editor script).

The panel is a UWidgetComponent; it feeds its render target into the texture parameter "SlateUI". The material is unlit and
translucent. The scene's exposure is locked (EV100 13 by day), which would make a plain emissive colour look black, so the
colour is multiplied by "Gain", the inverse of the exposure scale 1.2 * 2^EV100 (AGameMenuPanel sets it every frame from the
console variable dg.MenuPanelExposureEv). "Brightness" is the menu's white as a fraction of display white.
EyeAdaptationInverse was tried first and over-brightened the panel to white, so the gain is explicit.

Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

DST = "/Game/UI/M_MenuPanel"
BRIGHTNESS = 1.0

mel = unreal.MaterialEditingLibrary
if unreal.EditorAssetLibrary.does_asset_exist(DST):
    unreal.EditorAssetLibrary.delete_asset(DST)
package_path, name = DST.rsplit("/", 1)
material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, package_path, unreal.Material, unreal.MaterialFactoryNew())

material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property("two_sided", True)

slate_ui = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -700, 0)
slate_ui.set_editor_property("parameter_name", "SlateUI")
slate_ui.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

brightness = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 260)
brightness.set_editor_property("parameter_name", "Brightness")
brightness.set_editor_property("default_value", BRIGHTNESS)

scaled = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -420, 0)
mel.connect_material_expressions(slate_ui, "RGB", scaled, "A")
mel.connect_material_expressions(brightness, "", scaled, "B")

gain = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -700, 400)
gain.set_editor_property("parameter_name", "Gain")
gain.set_editor_property("default_value", 1.2 * 2 ** 13)

exposed = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, 0)
mel.connect_material_expressions(scaled, "", exposed, "A")
mel.connect_material_expressions(gain, "", exposed, "B")

mel.connect_material_property(exposed, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.connect_material_property(slate_ui, "A", unreal.MaterialProperty.MP_OPACITY)
mel.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log(f"Created {DST}")
