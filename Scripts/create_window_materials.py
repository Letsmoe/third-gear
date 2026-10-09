"""Creates the window materials: glass with an interior-mapped room behind it, and the painted frames.

  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_window_materials.py -unattended -nosplash

M_WindowGlass is the pane of a kit window unit (the Glass slot): the shader in
Plugins/MapRuntime/Shaders/Private/WindowInterior.ush draws net curtains, drapes or blinds just behind the glass and a
random room behind them, with lamps that switch on in the evening (hour and daylight come from /Game/World/MPC_Weather).
The pane's UVs must be in metres from the bay's origin, as the kit makes them. M_WindowFrame is the painted timber, PVC
or aluminium. Instances for the three Hamburg window types sit next to them, in /Game/World/Windows.
Re-running rebuilds the graphs in place; the instances keep their parent.
"""
import unreal

FOLDER = "/Game/World/Windows"
WEATHER_COLLECTION = "/Game/World/MPC_Weather"
ATLAS = "/Game/World/Windows/T_RoomAtlas"  # Scripts/import_room_atlas.py
NOISE_INCLUDE = "/Plugin/MapRuntime/Private/TerrainNoise.ush"
WINDOW_INCLUDE = "/Plugin/MapRuntime/Private/WindowInterior.ush"

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

GLASS_HLSL = """
// Whether V grows upwards depends on how the mesh was imported (kit glb files) or built (OSM meshes use -height):
// take the tangent frame's own V direction and make it point up.
float3 right = normalize(AxisX);
float3 up = normalize(AxisY);
float verticalSign = up.z >= 0.0 ? 1.0 : -1.0;
up *= verticalSign;
float2 pane = float2(UV.x, UV.y * verticalSign);
float3 outward = normalize(AxisZ);
float3 toEye = normalize(CameraVector);
// Direction of the view ray in the pane frame: x right, y up, z into the building.
float3 viewLocal = float3(dot(-toEye, right), dot(-toEye, up), dot(toEye, outward));
// The bay's origin identifies the window: every pane of one window unit shares it, whatever the UV's offset.
float3 bayOrigin = WorldPosition * 0.01 - right * pane.x - up * pane.y;
int3 cell = int3(floor(bayOrigin * 2.0 + 0.37)) + 100000;
uint seed = DgWinHash(uint(cell.x) * 73856093u ^ uint(cell.y) * 19349663u ^ uint(cell.z) * 83492791u);
float2 paneX = ddx(pane);
float2 paneY = ddy(pane);
float metresPerPixel = max(length(paneX), length(paneY));
FDgWindowGlass glass = DgWindowGlass(pane, viewLocal, seed, metresPerPixel, Night, TimeOfDay, Illuminance, Wetness,
	StoreyHeight, WindowCentre, WindowWidth, WindowBottom, WindowTop, Age, Dirt, DayRadiance, LampRadiance, LitShare,
	CurtainShare, FrontDepth, RoomAtlas, RoomAtlasSampler, AtlasStrength, Frosted, CurtainDayRadiance);
DiffuseOut = glass.Diffuse;
RoughOut = glass.Roughness;
NormalOut = normalize(float3(glass.NormalTilt, 1.0));
return glass.Emissive;
"""

FRAME_HLSL = """
float2 position = UV;
// Brush marks and an uneven coat: low frequency patches and fine grain along the member.
float patches = DgValueNoise(position * 6.0);
float grain = DgValueNoise(float2(position.x * 90.0, position.y * 9.0)) * 0.6 + DgValueNoise(float2(position.x * 9.0, position.y * 90.0)) * 0.4;
float dirt = saturate(Dirt * (0.3 + 0.7 * DgValueNoise(position * 2.3 + 4.0)));
float3 colour = Paint * lerp(0.93, 1.03, patches) * lerp(0.97, 1.03, grain);
colour = lerp(colour, colour * float3(0.62, 0.58, 0.5), dirt * 0.6);
RoughOut = lerp(BaseRoughness, min(BaseRoughness + 0.25, 1.0), dirt);
return colour;
"""


def expr(material, cls, x, y, **props):
    node = mel.create_material_expression(material, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def link(source, source_pin, target, target_pin):
    if not mel.connect_material_expressions(source, source_pin, target, target_pin):
        raise RuntimeError(f"failed to connect {source.get_name()}.{source_pin} -> {target.get_name()}.{target_pin}")


def custom_input(name):
    custom = unreal.CustomInput()
    custom.set_editor_property("input_name", name)
    return custom


def custom_output(name, kind):
    output = unreal.CustomOutput()
    output.set_editor_property("output_name", name)
    output.set_editor_property("output_type", kind)
    return output


def scalar(material, name, default, x, y):
    return expr(material, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=default)


def fresh_material(name):
    """An empty material asset: the existing one with its graph cleared, or a new one."""
    path = f"{FOLDER}/{name}"
    if eal.does_asset_exist(path):
        material = unreal.load_asset(path)
        mel.delete_all_material_expressions(material)
        return material
    return asset_tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


def tangent_axis(material, x, y, axis):
    """Direction of the tangent frame's axis (0 = U, 1 = V, 2 = normal) in world space."""
    vector = [unreal.LinearColor(1, 0, 0, 0), unreal.LinearColor(0, 1, 0, 0), unreal.LinearColor(0, 0, 1, 0)][axis]
    constant = expr(material, unreal.MaterialExpressionConstant3Vector, x, y, constant=vector)
    transform = expr(material, unreal.MaterialExpressionTransform, x + 150, y,
                     transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_TANGENT,
                     transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    link(constant, "", transform, "")
    return transform


def build_glass():
    """M_WindowGlass: opaque, dark glass with the room as emission, so Lumen reflections add the sky and the street."""
    material = fresh_material("M_WindowGlass")
    material.set_editor_property("two_sided", True)
    collection = unreal.load_asset(WEATHER_COLLECTION)

    texcoord = expr(material, unreal.MaterialExpressionTextureCoordinate, -1500, 0)
    world_position = expr(material, unreal.MaterialExpressionWorldPosition, -1500, 150,
                          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
    camera = expr(material, unreal.MaterialExpressionCameraVectorWS, -1500, 300)

    names = ["UV", "WorldPosition", "CameraVector", "AxisX", "AxisY", "AxisZ", "Night", "TimeOfDay", "Illuminance",
             "Wetness"]
    parameters = {"StoreyHeight": 3.25, "WindowCentre": 1.0, "WindowWidth": 1.0, "WindowBottom": 0.85,
                  "WindowTop": 2.75, "Age": 0.5, "Dirt": 0.5, "RoomAtlas": None, "DayRadiance": 45.0, "LampRadiance": 3.6,
                  "LitShare": 0.55, "CurtainShare": 0.9, "FrontDepth": 0.22, "AtlasStrength": 1.0, "Frosted": 0.0, "CurtainDayRadiance": 1800.0}
    custom = expr(material, unreal.MaterialExpressionCustom, -700, 0, code=GLASS_HLSL, description="WindowGlass",
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                  inputs=[custom_input(n) for n in names + list(parameters)])
    custom.set_editor_property("additional_outputs", [
        custom_output("DiffuseOut", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
        custom_output("RoughOut", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
        custom_output("NormalOut", unreal.CustomMaterialOutputType.CMOT_FLOAT3)])
    custom.set_editor_property("include_file_paths", [NOISE_INCLUDE, WINDOW_INCLUDE])

    link(texcoord, "", custom, "UV")
    link(world_position, "", custom, "WorldPosition")
    link(camera, "", custom, "CameraVector")
    for axis, name in enumerate(("AxisX", "AxisY", "AxisZ")):
        link(tangent_axis(material, -1500, 500 + axis * 150, axis), "", custom, name)
    for row, (collection_name, input_name) in enumerate((("Night", "Night"), ("TimeOfDay", "TimeOfDay"),
                                                          ("GroundIlluminance", "Illuminance"), ("Wetness", "Wetness"))):
        node = expr(material, unreal.MaterialExpressionCollectionParameter, -1500, 1000 + row * 120,
                    collection=collection, parameter_name=collection_name)
        link(node, "", custom, input_name)
    for row, (name, default) in enumerate(parameters.items()):
        if name == "RoomAtlas":
            continue
        link(scalar(material, name, default, -1500, 1600 + row * 100), "", custom, name)
    atlas = unreal.load_asset(ATLAS) if eal.does_asset_exist(ATLAS) else unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
    atlas_node = expr(material, unreal.MaterialExpressionTextureObjectParameter, -1500, 3000, parameter_name="RoomAtlas",
                      texture=atlas, sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    link(atlas_node, "", custom, "RoomAtlas")

    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(custom, "DiffuseOut", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(custom, "RoughOut", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(custom, "NormalOut", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -300, 300, r=1.0), "",
                                  unreal.MaterialProperty.MP_SPECULAR)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def build_flat_glass():
    """M_WindowGlassFlat: the plain dark reflective pane the facade material uses, the baseline for profiling."""
    material = fresh_material("M_WindowGlassFlat")
    material.set_editor_property("two_sided", True)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant3Vector, -400, 0,
                                       constant=unreal.LinearColor(0.012, 0.014, 0.016, 1)), "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(material, unreal.MaterialExpressionConstant, -400, 150, r=0.04), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)


def build_frame():
    """M_WindowFrame: painted frame and glazing bars with an uneven coat and dirt, Paint and BaseRoughness per instance."""
    material = fresh_material("M_WindowFrame")
    texcoord = expr(material, unreal.MaterialExpressionTextureCoordinate, -900, 0)
    paint = expr(material, unreal.MaterialExpressionVectorParameter, -900, 150, parameter_name="Paint",
                 default_value=unreal.LinearColor(0.78, 0.78, 0.74, 1))
    custom = expr(material, unreal.MaterialExpressionCustom, -500, 0, code=FRAME_HLSL, description="WindowFrame",
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                  inputs=[custom_input(n) for n in ("UV", "Paint", "Dirt", "BaseRoughness")])
    custom.set_editor_property("additional_outputs", [custom_output("RoughOut", unreal.CustomMaterialOutputType.CMOT_FLOAT1)])
    custom.set_editor_property("include_file_paths", [NOISE_INCLUDE])
    link(texcoord, "", custom, "UV")
    link(paint, "", custom, "Paint")
    link(scalar(material, "Dirt", 0.3, -900, 300), "", custom, "Dirt")
    link(scalar(material, "BaseRoughness", 0.45, -900, 400), "", custom, "BaseRoughness")
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(custom, "RoughOut", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(material)
    eal.save_loaded_asset(material)
    return material


def build_instance(parent, name, scalars, vectors=None):
    """A material instance of the parent with the given scalar and colour overrides."""
    path = f"{FOLDER}/{name}"
    if eal.does_asset_exist(path):
        instance = unreal.load_asset(path)
    else:
        instance = asset_tools.create_asset(name, FOLDER, unreal.MaterialInstanceConstant,
                                            unreal.MaterialInstanceConstantFactoryNew())
    instance.set_editor_property("parent", parent)
    for key, value in scalars.items():
        mel.set_material_instance_scalar_parameter_value(instance, key, value)
    for key, value in (vectors or {}).items():
        mel.set_material_instance_vector_parameter_value(instance, key, unreal.LinearColor(*value))
    eal.save_loaded_asset(instance)


def main():
    """Builds both masters and the instances for old timber, refurbished PVC and modern aluminium windows."""
    if not eal.does_asset_exist(WEATHER_COLLECTION):
        raise RuntimeError("run create_weather_parameters.py first")
    glass = build_glass()
    frame = build_frame()
    build_flat_glass()
    # Old timber casements: wavy glass, much dirt, curtains on most. PVC: flat glass, tidy. Aluminium: shop and office.
    build_instance(glass, "MI_WindowGlass_Timber", {"Age": 0.9, "Dirt": 0.45})
    build_instance(glass, "MI_WindowGlass_PVC", {"Age": 0.1, "Dirt": 0.3})
    build_instance(glass, "MI_WindowGlass_Door", {"Frosted": 1.0, "Age": 0.3})
    build_instance(glass, "MI_WindowGlass_Aluminium", {"Age": 0.0, "Dirt": 0.2, "CurtainShare": 0.55, "LitShare": 0.7})
    build_instance(frame, "MI_WindowFrame_Timber", {"Dirt": 0.5, "BaseRoughness": 0.45}, {"Paint": (0.74, 0.74, 0.70, 1)})
    build_instance(frame, "MI_WindowFrame_PVC", {"Dirt": 0.25, "BaseRoughness": 0.3}, {"Paint": (0.80, 0.81, 0.80, 1)})
    build_instance(frame, "MI_WindowFrame_Aluminium", {"Dirt": 0.15, "BaseRoughness": 0.35}, {"Paint": (0.022, 0.024, 0.027, 1)})
    unreal.log_warning("create_window_materials: done")


main()
