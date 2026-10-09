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
SEASON_INCLUDE = "/Plugin/MapRuntime/Private/LeafSeason.ush"

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
return DgTreeWind(WorldPosition, TreePosition, Camera, Seconds, InstanceRandom, WindSpeed * WindStrength,
	windDirection, TreeHeight, CrownRadius, worldHeight, TrunkBend, BranchSway, LeafFlutter, FadeDistance);
"""

# Autumn colour and leaf fall (LeafSeason.ush). Only foliage slots of deciduous meshes take part: Deciduous and IsLeaf
# are material instance parameters.
COLOUR_HLSL = """
float cellHash = DgLeafCellHash(TreePosition, InstanceRandom);
float3 turned = DgLeafAutumnColour(BaseColor, LeafColour, InstanceRandom, cellHash, Deciduous);
return lerp(BaseColor, turned, IsLeaf);
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


def custom_node(material, code, inputs, x, y, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                includes=(INCLUDE,)):
    """A Custom HLSL node with named inputs and include files, returning a float3 unless told otherwise."""
    node = expression(material, unreal.MaterialExpressionCustom, x, y, code=code,
                      output_type=output_type, include_file_paths=list(includes))
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

    inputs = ["WorldPosition", "TreePosition", "LocalUp", "Seconds", "Camera", "InstanceRandom", "WindSpeed", "WindX",
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
    add_season(material, collection, broken, made, world_position, random, x, tag)
    added = expression(material, unreal.MaterialExpressionAdd, x + 1000, 300, **tag)
    mel.connect_material_expressions(broken, "WorldPositionOffset", added, "A")
    mel.connect_material_expressions(wind, "", added, "B")
    mel.connect_material_expressions(added, "", made, "WorldPositionOffset")
    mel.connect_material_property(made, "", unreal.MaterialProperty.MP_MATERIAL_ATTRIBUTES)
    material.set_editor_property("max_world_position_offset_displacement", MAX_DISPLACEMENT_CM)


def add_season(material, collection, broken, made, world_position, random, x, tag):
    """Replaces the passed-through base colour by the autumn colour. Thinning is not done yet: the crown is a Nanite
    assembly of a few large leaf cards (about 2800 triangles) without a per-card identity or an honoured opacity mask."""
    shared = {
        "TreePosition": world_position, "InstanceRandom": random,
        "LeafDensity": collection_parameter(material, collection, "LeafDensity", x, 1700),
        "LeafColour": collection_parameter(material, collection, "LeafColour", x, 1800),
        "Deciduous": scalar_parameter(material, "Deciduous", 1.0, x, 1900),
        "IsLeaf": scalar_parameter(material, "IsLeaf", 0.0, x, 2000),
    }
    float1 = unreal.CustomMaterialOutputType.CMOT_FLOAT1
    float3 = unreal.CustomMaterialOutputType.CMOT_FLOAT3
    colour = custom_node(material, COLOUR_HLSL, ["BaseColor", "LeafColour", "TreePosition", "InstanceRandom", "Deciduous", "IsLeaf"],
                         x + 600, -800, float3, (SEASON_INCLUDE,))
    shared_nodes = (colour,)
    for node, source_name, attribute in ((colour, "BaseColor", "BaseColor"),):
        node.set_editor_property("desc", "DgSeason")
        if not mel.connect_material_expressions(broken, attribute, node, source_name):
            unreal.log_warning(f"tree wind: season node could not read {attribute}")
    for node in shared_nodes:
        for name, source in shared.items():
            if any(entry.get_editor_property("input_name") == name for entry in node.get_editor_property("inputs")):
                if not mel.connect_material_expressions(source, "", node, name):
                    unreal.log_warning(f"tree wind: season input {name} not connected")
    for node, attribute in ((colour, "BaseColor"),):
        if not mel.connect_material_expressions(node, "", made, attribute):
            unreal.log_warning(f"tree wind: season node not connected to {attribute}")


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
            is_leaf = 1.0 if kind == "foliage" else 0.0
            deciduous = 0.0 if "Conifer" in mesh_name else 1.0
            for name, value in (("IsLeaf", is_leaf), ("Deciduous", deciduous), ("TreeHeight", height), ("CrownRadius", crown), ("TrunkBend", trunk),
                                ("BranchSway", bough), ("LeafFlutter", leaf)):
                mel.set_material_instance_scalar_parameter_value(instance, name, value)
            mel.update_material_instance(instance)
            eal.save_loaded_asset(instance)
            unreal.log_warning(f"tree wind: {path} ({kind}) height {height:.0f} crown {crown:.0f}, parent was "
                               f"{source.get_class().get_name()}")


create_instances(create_master())
