"""Gives the baked trees (/Game/Vegetation/SM_*) wind: a copy of the Procedural Vegetation Editor's tree master
material with a world position offset added, plus one material instance per tree material slot.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_tree_wind_materials.py -unattended

The meshes themselves are not touched. UWorldStreamer puts the instances /Game/Vegetation/Wind/MI_<mesh>_<slot> on the
plants as component material overrides, so a checkout without them still shows the plain trees. The sway itself is
DgTreeWind in Plugins/MapRuntime/Shaders/Private/WindSway.ush; the weather's wind comes from /Game/World/MPC_Weather.
Re-running rebuilds the master's wind nodes and resets the instances' wind parameters.
"""
import unreal

SOURCE_MASTER = "/ProceduralVegetationEditor/SampleAssets/Materials/MasterMaterials/MA_Foliage_Trees"
FOLDER = "/Game/Vegetation/Wind"
MASTER = f"{FOLDER}/MA_Foliage_Trees_Wind"
COLLECTION = "/Game/World/MPC_Weather"
INCLUDE = "/Plugin/MapRuntime/Private/WindSway.ush"

# Sway is faded out and the engine stops evaluating it at this camera distance (cm); the streamer sets the matching
# component WorldPositionOffsetDisableDistance.
FADE_DISTANCE_CM = 9000.0
# The largest displacement the sway can produce (cm); Nanite widens its culling bounds by this.
MAX_DISPLACEMENT_CM = 250.0

# mesh -> weights per material slot (trunk bend, bough sway, leaf flutter). Bark follows the trunk and boughs; the
# foliage slot flutters. Conifers are stiffer and their needles barely flutter; shrubs have no trunk to speak of.
MESHES = {
    "SM_Broadleaf_01": {"bark": (1.0, 1.0, 0.0), "foliage": (1.0, 1.0, 1.0)},
    "SM_Conifer_01": {"bark": (0.8, 0.5, 0.0), "foliage": (0.8, 0.6, 0.4)},
    "SM_Shrub_01": {"bark": (0.4, 1.0, 0.0), "foliage": (0.4, 1.0, 1.0)},
}

TREE_HLSL = """
float3 localHeightAxis = LocalUp;
float worldHeight = length(localHeightAxis) * TreeHeight;
float2 windDirection = float2(WindX, WindY);
return DgTreeWind(WorldPosition, LocalPosition, Camera, Seconds, InstanceRandom, WindSpeed * WindStrength,
	windDirection, TreeHeight, CrownRadius, worldHeight, TrunkBend, BranchSway, LeafFlutter, FadeDistance);
"""

# Names of the material attributes passed through unchanged.
ATTRIBUTES = ["BaseColor", "Metallic", "Specular", "Roughness", "Anisotropy", "EmissiveColor", "Opacity", "OpacityMask",
              "Normal", "Tangent", "SubsurfaceColor", "ClearCoat", "ClearCoatRoughness", "AmbientOcclusion", "Refraction",
              "CustomizedUV0", "CustomizedUV1", "CustomizedUV2", "CustomizedUV3", "CustomizedUV4", "CustomizedUV5",
              "CustomizedUV6", "CustomizedUV7", "PixelDepthOffset", "ShadingModel", "Displacement"]

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary


def expression(material, cls, x, y, **properties):
    """Adds a material expression with the given properties."""
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in properties.items():
        node.set_editor_property(key, value)
    return node


def scalar_parameter(material, name, default, x, y):
    """A named scalar parameter."""
    return expression(material, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=default)


def collection_parameter(material, collection, name, x, y):
    """A scalar from the weather parameter collection."""
    return expression(material, unreal.MaterialExpressionCollectionParameter, x, y, collection=collection,
                      parameter_name=name)


def custom_node(material, code, inputs, x, y):
    """A Custom HLSL node returning a float3, with named inputs and the wind include."""
    node = expression(material, unreal.MaterialExpressionCustom, x, y, code=code,
                      output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, include_file_paths=[INCLUDE])
    entries = []
    for name in inputs:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", name)
        entries.append(entry)
    node.set_editor_property("inputs", entries)
    return node


def duplicate(asset, path):
    """Copies an asset (possibly from an engine plugin) to a project path."""
    folder, name = path.rsplit("/", 1)
    return unreal.AssetToolsHelpers.get_asset_tools().duplicate_asset(name, folder, asset)


def build_wind_graph(material):
    """Adds the sway nodes and routes the original material attributes through a break and make pair."""
    collection = unreal.load_asset(COLLECTION)
    original = mel.get_material_property_input_node(material, unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES)
    original_output = mel.get_material_property_input_node_output_name(material, unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES)
    if original is None:
        raise RuntimeError("master has no material attributes input")
    unreal.log_warning(f"tree wind: original output node {original.get_name()} pin '{original_output}'")

    x = -3000
    tag = {"desc": "DgWind"}
    world_position = expression(material, unreal.MaterialExpressionWorldPosition, x, 0, **tag,
                                world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    local_position = expression(material, unreal.MaterialExpressionTransformPosition, x, 100, **tag,
                                transform_source_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_WORLD,
                                transform_type=unreal.MaterialPositionTransformSource.TRANSFORMPOSSOURCE_LOCAL)
    mel.connect_material_expressions(world_position, "", local_position, "")
    up = expression(material, unreal.MaterialExpressionConstant3Vector, x, 200, **tag, constant=unreal.LinearColor(0, 0, 1, 0))
    world_up = expression(material, unreal.MaterialExpressionTransform, x + 150, 200, **tag,
                          transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL,
                          transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    mel.connect_material_expressions(up, "", world_up, "")
    seconds = expression(material, unreal.MaterialExpressionTime, x, 300, **tag)
    camera = expression(material, unreal.MaterialExpressionCameraPositionWS, x, 400, **tag)
    random = expression(material, unreal.MaterialExpressionPerInstanceRandom, x, 500, **tag)

    inputs = ["WorldPosition", "LocalPosition", "LocalUp", "Seconds", "Camera", "InstanceRandom", "WindSpeed", "WindX",
              "WindY", "WindStrength", "TreeHeight", "CrownRadius", "TrunkBend", "BranchSway", "LeafFlutter", "FadeDistance"]
    sources = [
        world_position, local_position, world_up, seconds, camera, random,
        collection_parameter(material, collection, "WindSpeed", x, 600),
        collection_parameter(material, collection, "WindDirectionX", x, 700),
        collection_parameter(material, collection, "WindDirectionY", x, 800),
        scalar_parameter(material, "WindStrength", 1.0, x, 900),
        scalar_parameter(material, "TreeHeight", 1200.0, x, 1000),
        scalar_parameter(material, "CrownRadius", 450.0, x, 1100),
        scalar_parameter(material, "TrunkBend", 1.0, x, 1200),
        scalar_parameter(material, "BranchSway", 1.0, x, 1300),
        scalar_parameter(material, "LeafFlutter", 1.0, x, 1400),
        scalar_parameter(material, "FadeDistance", FADE_DISTANCE_CM, x, 1500),
    ]
    wind = custom_node(material, TREE_HLSL, inputs, x + 600, 600)
    wind.set_editor_property("desc", "DgWind")
    for name, source in zip(inputs, sources):
        if source.get_editor_property("desc") != "DgWind" and not isinstance(source, unreal.MaterialExpressionScalarParameter):
            source.set_editor_property("desc", "DgWind")
        mel.connect_material_expressions(source, "", wind, name)

    broken = expression(material, unreal.MaterialExpressionBreakMaterialAttributes, x + 600, -400, **tag)
    made = expression(material, unreal.MaterialExpressionMakeMaterialAttributes, x + 1200, -400, **tag)
    mel.connect_material_expressions(original, original_output, broken, "")
    for name in ATTRIBUTES:
        if not mel.connect_material_expressions(broken, name, made, name):
            unreal.log_warning(f"tree wind: could not pass {name} through")
    added = expression(material, unreal.MaterialExpressionAdd, x + 1000, 300, **tag)
    mel.connect_material_expressions(broken, "WorldPositionOffset", added, "A")
    mel.connect_material_expressions(wind, "", added, "B")
    mel.connect_material_expressions(added, "", made, "WorldPositionOffset")
    mel.connect_material_property(made, "", unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES)
    material.set_editor_property("max_world_position_offset_displacement", MAX_DISPLACEMENT_CM)


def create_master():
    """Duplicates the engine master into the project once and adds the wind to it."""
    if eal.does_asset_exist(MASTER):
        eal.delete_asset(MASTER)
    eal.make_directory(FOLDER)
    material = duplicate(unreal.load_asset(SOURCE_MASTER), MASTER)
    build_wind_graph(material)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def create_instances(master):
    """One instance per tree material slot, copied from the slot's current material with the master swapped."""
    for mesh_name, weights in MESHES.items():
        mesh = unreal.load_asset(f"/Game/Vegetation/{mesh_name}")
        box = mesh.get_bounding_box()
        height = box.max.z - box.min.z
        crown = max(box.max.x - box.min.x, box.max.y - box.min.y) * 0.5
        for slot, static_material in enumerate(mesh.static_materials):
            source = static_material.material_interface
            path = f"{FOLDER}/MI_{mesh_name}_{slot}"
            if eal.does_asset_exist(path):
                eal.delete_asset(path)
            instance = duplicate(source, path)
            instance.set_editor_property("parent", master)
            # Slots that carry leaf textures flutter; bark slots do not.
            kind = "foliage" if "oliage" in str(static_material.material_slot_name) else "bark"
            trunk, bough, leaf = weights[kind]
            for name, value in (("TreeHeight", height), ("CrownRadius", crown), ("TrunkBend", trunk),
                                ("BranchSway", bough), ("LeafFlutter", leaf)):
                mel.set_material_instance_scalar_parameter_value(instance, name, value)
            mel.update_material_instance(instance)
            eal.save_loaded_asset(instance)
            unreal.log_warning(f"tree wind: {path} ({kind}) height {height:.0f} crown {crown:.0f}, parent was "
                               f"{source.get_class().get_name()}")


create_instances(create_master())
