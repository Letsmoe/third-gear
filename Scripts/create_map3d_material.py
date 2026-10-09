"""Creates the materials of the 3D map in /Game/UI (headless editor script).

  M_Map3D         every shape of the map: opaque, unlit, two-sided.
  M_Map3DOverlay  the car and the waypoint pin: the same look, but translucent with the depth test off, so a building in
                  front of the car never hides it (as on a navigation map). Without a depth test its shapes cannot sort
                  themselves, so it is one-sided and its meshes list the parts back to front.

The 3D map (UMap3DSubsystem) bakes no colours into its meshes. A vertex carries its palette slot in U and a brightness
factor in V (UV channel 0); the material looks the colour up in the 32 pixel wide texture "Palette" and multiplies it with
the brightness. The capture is scene radiance (SCS_SceneColorHDR), so the colour is emitted as it is, without exposure or
tone mapping. Unlit; the map's own shapes are two-sided, so triangle winding never matters for them.

Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

mel = unreal.MaterialEditingLibrary


def mask(material, channel, x, y):
    """A ComponentMask that passes one channel."""
    node = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, x, y)
    for name in ("r", "g", "b", "a"):
        node.set_editor_property(name, name == channel)
    return node


def create_material(destination, overlay):
    """Builds one material; the overlay variant is translucent and ignores the depth test."""
    if unreal.EditorAssetLibrary.does_asset_exist(destination):
        unreal.EditorAssetLibrary.delete_asset(destination)
    package_path, name = destination.rsplit("/", 1)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, package_path, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", not overlay)
    if overlay:
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        material.set_editor_property("disable_depth_test", True)

    texture_coordinate = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 0)
    slot_u = mask(material, "r", -700, -60)
    mel.connect_material_expressions(texture_coordinate, "", slot_u, "")
    brightness = mask(material, "g", -700, 120)
    mel.connect_material_expressions(texture_coordinate, "", brightness, "")

    row = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -700, -180)
    row.set_editor_property("r", 0.5)
    palette_uv = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, -500, -120)
    mel.connect_material_expressions(slot_u, "", palette_uv, "A")
    mel.connect_material_expressions(row, "", palette_uv, "B")

    palette = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -300, -120)
    palette.set_editor_property("parameter_name", "Palette")
    palette.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    mel.connect_material_expressions(palette_uv, "", palette, "UVs")

    shaded = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -100, 0)
    mel.connect_material_expressions(palette, "RGB", shaded, "A")
    mel.connect_material_expressions(brightness, "", shaded, "B")

    mel.connect_material_property(shaded, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if overlay:
        opaque = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -100, 200)
        opaque.set_editor_property("r", 1.0)
        mel.connect_material_property(opaque, "", unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log(f"Created {destination}")


create_material("/Game/UI/M_Map3D", overlay=False)
create_material("/Game/UI/M_Map3DOverlay", overlay=True)
