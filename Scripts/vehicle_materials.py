"""Creates materials used by the player car in /Game/Vehicles/Materials (headless editor script).

  M_DashboardText: unlit text for the speed/rpm readout (UTextRenderComponent). Its emissive goes through
                   "EyeAdaptationInverse", so it has the same on-screen brightness (parameter "Brightness", 0..1 of
                   display white) under any exposure - daylight EV 13, auto exposure or night.

Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

DST = "/Game/Vehicles/Materials/M_DashboardText"
BRIGHTNESS = 0.7

mel = unreal.MaterialEditingLibrary
if unreal.EditorAssetLibrary.does_asset_exist(DST):
    unreal.EditorAssetLibrary.delete_asset(DST)
source = unreal.load_asset("/Engine/EngineMaterials/UnlitText")
package_path, name = DST.rsplit("/", 1)
material = unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, package_path, source)

vertex_color = mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -150, -200)
brightness = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -350, -120)
brightness.set_editor_property("parameter_name", "Brightness")
brightness.set_editor_property("default_value", BRIGHTNESS)
mel.connect_material_expressions(vertex_color, "", multiply, "A")
mel.connect_material_expressions(brightness, "", multiply, "B")
inverse = mel.create_material_expression(material, unreal.MaterialExpressionEyeAdaptationInverse, 0, -200)
mel.connect_material_expressions(multiply, "", inverse, "LightValue")
mel.connect_material_property(inverse, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
mel.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)
unreal.log(f"Created {DST}")
