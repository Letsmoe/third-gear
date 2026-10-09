"""Creates the fallen leaf assets in /Game/Leaves: the atlas texture, the leaf card meshes and the material M_LeafCard.

  Tools/osmimport/.venv/bin/python -I Tools/leafatlas/build_leaf_atlas.py          (once, makes the atlas)
  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_leaf_assets.py -unattended -nosplash

The atlas holds 16 photographed leaves (4x4 cells). Each card is a square of CARD_SIZE_CM with a gentle curl, 48
triangles, built as three shapes (cup, fold, wave) in two variants: plain static meshes for instanced static mesh
components, and Nanite versions to compare their cost. The material takes the atlas cell from the per-instance
random value, dries and browns some leaves, and darkens and glosses all of them with the weather's wetness. Rebuilt
in place.
"""
import math
import os

import unreal

FOLDER = "/Game/Leaves"
ATLAS_NAME = "T_LeafAtlas"
MATERIAL_NAME = "M_LeafCard"
COLLECTION = "/Game/World/MPC_Weather"
ATLAS_GRID = 4
CARD_SIZE_CM = 12.0
CARD_QUADS_ALONG = 6
CARD_QUADS_ACROSS = 4
CURL_HEIGHT_CM = 1.3
SHAPES = ("Cup", "Fold", "Wave")

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary

ATLAS_UV_HLSL = """
float cell = min(floor(R * 16.0), 15.0);
float column = fmod(cell, 4.0);
float row = floor(cell / 4.0);
return (UV + float2(column, row)) * 0.25;
"""

LEAF_SHADE_HLSL = """
float variant = frac(R * 16.0);
float dry = smoothstep(0.4, 1.0, variant);
float luminance = dot(Atlas, float3(0.3, 0.59, 0.11));
float3 dried = luminance * float3(1.3, 0.85, 0.5) * 0.8;
float3 colour = lerp(Atlas, dried, dry * 0.75);
colour *= lerp(0.8, 1.15, frac(R * 97.3));
colour = lerp(colour, colour * colour * 1.4, saturate(Wet) * 0.75);
float roughness = lerp(0.62, 0.2, saturate(Wet));
return float4(colour, roughness);
"""


def shape_height(shape, along, across):
    """Height in cm of the card surface at along and across in -1..1: cup edges up, fold down the midrib, wave."""
    if shape == "Cup":
        return CURL_HEIGHT_CM * (across * across * 0.8 + along * along * 0.2)
    if shape == "Fold":
        return CURL_HEIGHT_CM * 1.2 * abs(across) + 0.3 * CURL_HEIGHT_CM * along * along
    return CURL_HEIGHT_CM * (0.5 * math.sin(along * 2.6) + 0.5 * across * across * math.cos(along * 1.4))


def build_description(description, shape):
    """The mesh description of one curled card: a grid of quads (two triangles each), X along the leaf, Y across."""
    half = CARD_SIZE_CM / 2.0
    instances = []
    for row in range(CARD_QUADS_ALONG + 1):
        for column in range(CARD_QUADS_ACROSS + 1):
            along = row / CARD_QUADS_ALONG * 2.0 - 1.0
            across = column / CARD_QUADS_ACROSS * 2.0 - 1.0
            vertex = description.create_vertex()
            description.set_vertex_position(vertex, unreal.Vector(along * half, across * half,
                                                                  shape_height(shape, along, across)))
            instance = description.create_vertex_instance(vertex)
            description.set_vertex_instance_uv(instance, unreal.Vector2D(column / CARD_QUADS_ACROSS,
                                                                         row / CARD_QUADS_ALONG), 0)
            instances.append(instance)
    group = description.create_polygon_group()
    description.set_polygon_group_material_slot_name(group, "Leaf")
    stride = CARD_QUADS_ACROSS + 1
    for row in range(CARD_QUADS_ALONG):
        for column in range(CARD_QUADS_ACROSS):
            a = row * stride + column
            b = a + 1
            c = a + stride
            d = c + 1
            description.create_triangle(group, [instances[a], instances[b], instances[c]])
            description.create_triangle(group, [instances[b], instances[d], instances[c]])


def raw_leaves_dir():
    """The leaves folder of the data root's raw assets."""
    root = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
    return os.path.join(root, "raw_assets", "leaves")


def import_file(path, destination, name):
    """Imports one file with a fixed asset name and returns the imported asset."""
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", path)
    task.set_editor_property("destination_path", destination)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("save", False)
    task.set_editor_property("replace_existing", True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return unreal.load_asset(f"{destination}/{name}")


def import_atlas():
    """Imports the leaf atlas as an sRGB texture with alpha."""
    texture = import_file(os.path.join(raw_leaves_dir(), "leaf_atlas.png"), FOLDER, ATLAS_NAME)
    texture.set_editor_property("srgb", True)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_DEFAULT)
    texture.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
    texture.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP)
    try:
        # Keep the cut-out area constant over the mip chain so distant leaves don't thin out.
        texture.set_editor_property("alpha_coverage_thresholds", unreal.Vector4(0.0, 0.0, 0.0, 0.5))
    except Exception as error:  # property name differs between engine versions
        unreal.log_warning(f"leaf assets: no alpha coverage preservation ({error})")
    eal.save_loaded_asset(texture)
    return texture


def expression(material, cls, x, y, **properties):
    """Adds a material expression with the given properties."""
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in properties.items():
        node.set_editor_property(key, value)
    return node


def custom(material, code, input_names, output_type, x, y):
    """A Custom HLSL node."""
    entries = []
    for name in input_names:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", name)
        entries.append(entry)
    return expression(material, unreal.MaterialExpressionCustom, x, y, code=code, output_type=output_type,
                      inputs=entries)


def build_material(atlas):
    """Creates M_LeafCard: masked, two-sided foliage, atlas cell and dryness from the instance random value."""
    path = f"{FOLDER}/{MATERIAL_NAME}"
    if eal.does_asset_exist(path):
        material = unreal.load_asset(path)
        mel.delete_all_material_expressions(material)
    else:
        material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            MATERIAL_NAME, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    material.set_editor_property("used_with_nanite", True)
    material.set_editor_property("used_with_instanced_static_meshes", True)
    material.set_editor_property("opacity_mask_clip_value", 0.4)

    uv = expression(material, unreal.MaterialExpressionTextureCoordinate, -1400, 0)
    random = expression(material, unreal.MaterialExpressionPerInstanceRandom, -1400, 150)
    atlas_uv = custom(material, ATLAS_UV_HLSL, ["UV", "R"], unreal.CustomMaterialOutputType.CMOT_FLOAT2, -1150, 0)
    mel.connect_material_expressions(uv, "", atlas_uv, "UV")
    mel.connect_material_expressions(random, "", atlas_uv, "R")

    sample = expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -900, 0, parameter_name="Atlas",
                        texture=atlas, sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    mel.connect_material_expressions(atlas_uv, "", sample, "UVs")

    collection = unreal.load_asset(COLLECTION)
    wetness = expression(material, unreal.MaterialExpressionCollectionParameter, -900, 300, collection=collection,
                         parameter_name="Wetness")
    shade = custom(material, LEAF_SHADE_HLSL, ["Atlas", "R", "Wet"], unreal.CustomMaterialOutputType.CMOT_FLOAT4,
                   -600, 100)
    mel.connect_material_expressions(sample, "RGB", shade, "Atlas")
    mel.connect_material_expressions(random, "", shade, "R")
    mel.connect_material_expressions(wetness, "", shade, "Wet")

    colour = expression(material, unreal.MaterialExpressionComponentMask, -350, 50, r=True, g=True, b=True, a=False)
    roughness = expression(material, unreal.MaterialExpressionComponentMask, -350, 200, r=False, g=False, b=False,
                           a=True)
    mel.connect_material_expressions(shade, "", colour, "")
    mel.connect_material_expressions(shade, "", roughness, "")
    # Light shining through the leaf comes out in its own colour, a little warmer.
    glow = expression(material, unreal.MaterialExpressionMultiply, -200, 400)
    warm = expression(material, unreal.MaterialExpressionConstant3Vector, -350, 400,
                      constant=unreal.LinearColor(1.0, 0.85, 0.5, 0.0))
    mel.connect_material_expressions(colour, "", glow, "A")
    mel.connect_material_expressions(warm, "", glow, "B")

    mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(glow, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    mel.connect_material_property(sample, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
    mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def build_mesh(shape, nanite, material):
    """Creates SM_LeafCard_<shape>[_Nanite] from the card description, with the leaf material."""
    suffix = "_Nanite" if nanite else ""
    name = f"SM_LeafCard_{shape}{suffix}"
    path = f"{FOLDER}/{name}"
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    mesh = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, FOLDER, unreal.StaticMesh,
                                                                   None)
    description = mesh.create_static_mesh_description(mesh)
    build_description(description, shape)
    mesh.build_from_static_mesh_descriptions([description], False, False)
    mesh.set_editor_property("static_materials", [unreal.StaticMaterial(material_interface=material,
                                                                         material_slot_name="Leaf")])
    nanite_settings = mesh.get_editor_property("nanite_settings")
    nanite_settings.set_editor_property("enabled", nanite)
    mesh.set_editor_property("nanite_settings", nanite_settings)
    eal.save_loaded_asset(mesh)
    unreal.log_warning(f"leaf assets: {name} bounds {mesh.get_bounds().box_extent}")


def main():
    """Builds everything."""
    eal.make_directory(FOLDER)
    atlas = import_atlas()
    material = build_material(atlas)
    for shape in SHAPES:
        build_mesh(shape, False, material)
        build_mesh(shape, True, material)


main()
