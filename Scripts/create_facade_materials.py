"""Facade, roof and cladding materials from the Megascans sets (Scripts/import_megascans.py) and our CC0 scans.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_facade_materials.py -unattended -nosplash

One master, /Game/World/Facades/M_FacadeMaster, and one material instance per typology class (FACADES). Instances named
M_Facade_<Brick|Plaster|Concrete|Metal> and M_Roof_Tiles replace the plain section materials of the streamed world (the
streamer looks in this folder first, `-NoFacadeMaterials` skips it); the other names are for the kit buildings (#47, #48).

The master reads the vertex colour like M_SurfaceMaster: red is the building's brightness, green its variation (hue jitter,
texture offset, window layout). Colour comes from the scan after a hue/saturation/value shift or a recolour to a target colour
(see FacadeSurface.ush), roughness, normal and occlusion from the scan, depth from parallax occlusion mapping on the height map.
Season (snow) and wetness are the same layers as every other surface. Windows are the old painted ones behind a static switch,
off for kit pieces, which carry real glass. The weathering overlay (#84) plugs in at `add_weathering`.
"""
import importlib.util
import os

import unreal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("create_materials", os.path.join(SCRIPT_DIR, "create_materials.py"))
cm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cm)  # only the helpers: the build itself runs under __main__ there
mel, eal, expr, link, scalar, custom_input = cm.mel, cm.eal, cm.expr, cm.link, cm.scalar, cm.custom_input

FOLDER = "/Game/World/Facades"
MASTER = f"{FOLDER}/M_FacadeMaster"
BOARD_FOLDER = f"{FOLDER}/Board"
MEGASCANS = "/Game/Megascans/Textures"
FACADE_INCLUDE = "/Plugin/MapRuntime/Private/FacadeSurface.ush"

# Per-instance texture parameters and the Custom node inputs they feed.
MAP_KINDS = ("BaseColor", "Normal", "Roughness", "AO")

UV_HLSL = """
// One tile of the scan covers Tile metres. Each building starts at its own place in the texture. The material board
// (Board 1) has no wall UVs in metres, so it maps world x and height instead.
float2 Metres = lerp(UV, float2(WorldPos.x, -WorldPos.z) * 0.01, Board);
return Metres / Tile + frac(Variation * float2(7.31, 3.17)) * (1.0 - Board);
"""

SURFACE_HLSL = """
float2 Metres = float2(dot(WorldPos.xy, float2(0.7071, 0.7071)), WorldPos.z) * 0.01;
FDgFacadeSurface Surface = DgFacadeSampleSurface(TexColor, TexColorSampler, TexNormal, TexNormalSampler, TexRough, TexRoughSampler,
	TexAO, TexAOSampler, Uv, Metres, AntiTile);
float3 Color = DgFacadeShift(Surface.Color, HueShift + (Variation - 0.5) * HueJitter, Saturation, Value);
Color = DgFacadeRecolor(Color, ScanMean, Recolor, RecolorAmount);
Color *= DgFacadeBrightness(BuildingTint, TintMin, TintMax, Metres);
NormalOut = normalize(float3(Surface.NormalXY * NormalStrength, 1.0));
RoughOut = saturate(Surface.Roughness * RoughnessScale);
AOOut = Surface.AO;
return Color;
"""

# Set references: "ms:<folder>" is a Megascans set under /Game/Megascans/Textures, anything else a set under /Game/Textures.


def set_asset(set_ref, kind):
    """Texture of one map of a set, or None."""
    if set_ref.startswith("ms:"):
        folder = set_ref[3:]
        path = f"{MEGASCANS}/{folder}/T_{folder}_{kind}"
    else:
        name = set_ref.split("/")[-1]
        path = f"{cm.TEX}/{set_ref}/T_{name}_{kind}"
    return unreal.load_asset(path) if eal.does_asset_exist(path) else None


def srgb_to_linear(color):
    return tuple(channel ** 2.2 for channel in color)


# Linear mean colour of each scan (measured on the 4K base colour), what a recolour divides by.
SCAN_MEAN = {
    "ms:brick_facade_56f913a0": (0.31, 0.118, 0.073),
    "ms:brick_facade_efa35b96": (0.251, 0.133, 0.10),
    "ms:brick_wall_5d318a8f": (0.15, 0.099, 0.078),
    "ms:brick_wall_e2865ee3": (0.121, 0.085, 0.068),
    "ms:brick_wall_worn_499d9ba7": (0.212, 0.104, 0.064),
    "ms:stucco_wall_e2f69285": (0.381, 0.384, 0.371),
    "ms:stucco_wall_8e1150b6": (0.714, 0.662, 0.563),
    "ms:stucco_facade_19ea388c": (0.625, 0.625, 0.626),
    "ms:stucco_facade_a78c4b5c": (0.582, 0.58, 0.589),
    "ms:wall_paint_cfdcd26b": (0.639, 0.589, 0.541),
    "ms:smooth_concrete_410ea2a8": (0.273, 0.241, 0.214),
    "ms:smooth_concrete_68db59f6": (0.28, 0.228, 0.167),
    "ms:weathered_concrete_wall_9485cd8b": (0.233, 0.204, 0.163),
    "ms:corrugated_metal_sheet_109b6f59": (0.072, 0.065, 0.058),
    "ms:painted_wooden_planks_71b1478e": (0.438, 0.407, 0.361),
    "ms:dirty_corrugated_metal_sheet_5b15681e": (0.203, 0.206, 0.204),
    "Plaster/white_stucco_02": (0.432, 0.427, 0.409),
    "Plaster/plastered_wall": (0.444, 0.40, 0.341),
    "Concrete/concrete_wall_004": (0.184, 0.173, 0.126),
}

# Instance name -> settings. Keys: set, tile (metres per repeat, u and v), hue, saturation, value (shift of the scan),
# recolor (sRGB target) with amount, parallax (depth as a fraction of a tile), normal, rough (scale), anti_tile,
# tint (lowest, highest brightness over the buildings), windows (painted windows for the old streamed buildings).
# Reflectances: Hamburg clinker 0.13 to 0.19, white render 0.6 to 0.75, beige 0.4 to 0.5, slab concrete 0.25 to 0.35.
FACADES = {
    # Instances the streamed world's section names resolve to.
    "Facade_Brick": dict(set="ms:brick_facade_efa35b96", tile=(2, 2), value=0.62, hue=-0.008, saturation=1.1, parallax=0.012,
                         tint=(0.8, 1.15), windows=True),
    "Facade_Plaster": dict(set="Plaster/white_stucco_02", tile=(2.5, 2.5), recolor=(0.90, 0.86, 0.76), amount=1.0, anti_tile=1.0,
                           normal=1.5, tint=(0.85, 1.05), parallax=0.006, windows=True),
    "Facade_Concrete": dict(set="Concrete/concrete_wall_004", tile=(3, 3), recolor=(0.60, 0.59, 0.56), amount=1.0,
                            tint=(0.75, 1.05), parallax=0.006, windows=True),
    "Facade_Metal": dict(set="ms:corrugated_metal_sheet_109b6f59", tile=(2, 2), recolor=(0.46, 0.48, 0.49), amount=1.0,
                         tint=(0.8, 1.15), parallax=0.02, windows=False),
    "Roof_Tiles": dict(set="Roof/clay_roof_tiles_03", tile=(2.5, 2.5), tint=(0.75, 1.1), parallax=0.03),
    # Kit materials by typology class (typology.md).
    "Facade_ClinkerDeepRed": dict(set="ms:brick_facade_efa35b96", tile=(2, 2), value=0.58, hue=-0.012, saturation=1.1,
                                  parallax=0.012, tint=(0.8, 1.15)),
    "Facade_ClinkerYellowBrown": dict(set="ms:brick_facade_56f913a0", tile=(2, 2), hue=0.05, saturation=0.72, value=0.8,
                                      parallax=0.006, tint=(0.8, 1.15)),
    "Facade_BrickGruenderzeit": dict(set="ms:brick_wall_5d318a8f", tile=(4, 2), value=1.0, saturation=1.1, parallax=0.012,
                                     tint=(0.75, 1.15)),
    "Facade_BrickSooty": dict(set="ms:brick_wall_e2865ee3", tile=(1, 2), value=1.2, parallax=0.012, tint=(0.8, 1.1)),
    "Facade_BrickPostwar": dict(set="ms:brick_facade_56f913a0", tile=(2, 2), value=0.74, hue=-0.008, saturation=0.9, parallax=0.006,
                                tint=(0.85, 1.1)),
    "Facade_RenderScratchWhite": dict(set="Plaster/white_stucco_02", tile=(2.5, 2.5), normal=1.5, recolor=(0.94, 0.93, 0.90), amount=1.0,
                                      anti_tile=1.0, parallax=0.004, tint=(0.88, 1.05)),
    "Facade_RenderScratchBeige": dict(set="Plaster/white_stucco_02", tile=(2.5, 2.5), normal=1.5, recolor=(0.83, 0.74, 0.58), amount=1.0,
                                      anti_tile=1.0, parallax=0.004, tint=(0.88, 1.05)),
    "Facade_RenderScratchPastel": dict(set="Plaster/white_stucco_02", tile=(2.5, 2.5), normal=1.5, recolor=(0.88, 0.80, 0.55), amount=1.0,
                                       anti_tile=1.0, parallax=0.004, tint=(0.88, 1.05)),
    "Facade_RenderScratchGrey": dict(set="Plaster/white_stucco_02", tile=(2.5, 2.5), normal=1.5, recolor=(0.62, 0.62, 0.61), amount=1.0,
                                     anti_tile=1.0, parallax=0.004, tint=(0.88, 1.05)),
    "Facade_RenderSmoothWhite": dict(set="ms:stucco_facade_a78c4b5c", tile=(2, 2), recolor=(0.95, 0.95, 0.93), amount=1.0,
                                     anti_tile=1.0, normal=0.5, tint=(0.9, 1.05)),
    "Facade_RenderSmoothBeige": dict(set="ms:stucco_facade_a78c4b5c", tile=(2, 2), recolor=(0.86, 0.78, 0.64), amount=1.0,
                                     anti_tile=1.0, normal=0.5, tint=(0.9, 1.05)),
    "Facade_RenderSmoothPastel": dict(set="ms:stucco_facade_a78c4b5c", tile=(2, 2), recolor=(0.80, 0.86, 0.80), amount=1.0,
                                      anti_tile=1.0, normal=0.5, tint=(0.9, 1.05)),
    "Facade_RenderSmoothGrey": dict(set="ms:stucco_facade_a78c4b5c", tile=(2, 2), recolor=(0.58, 0.60, 0.62), amount=1.0,
                                    anti_tile=1.0, normal=0.5, tint=(0.9, 1.05)),
    "Facade_ConcreteSlab": dict(set="Concrete/concrete_wall_004", tile=(3, 3), recolor=(0.60, 0.59, 0.56), amount=1.0, parallax=0.006,
                                tint=(0.8, 1.05)),
    "Facade_ConcreteStained": dict(set="ms:weathered_concrete_wall_9485cd8b", tile=(2, 1), parallax=0.01, tint=(0.8, 1.05)),
    "Facade_ConcreteSmooth": dict(set="ms:smooth_concrete_68db59f6", tile=(2, 2), anti_tile=1.0, normal=0.6, tint=(0.8, 1.05)),
    "Facade_Plinth": dict(set="ms:smooth_concrete_410ea2a8", tile=(1, 1), value=0.55, anti_tile=1.0, parallax=0.01, tint=(0.8, 1.0)),
    "Facade_TimberPainted": dict(set="ms:painted_wooden_planks_71b1478e", tile=(2, 2), recolor=(0.93, 0.93, 0.90), amount=1.0,
                                 parallax=0.012, tint=(0.9, 1.05)),
    "Facade_TimberBeam": dict(set="Wood/wood_planks", tile=(1.5, 1.5), value=0.35, saturation=0.85, parallax=0.01,
                              tint=(0.8, 1.1)),
    "Facade_MetalCladdingGrey": dict(set="ms:corrugated_metal_sheet_109b6f59", tile=(2, 2), recolor=(0.46, 0.48, 0.49), amount=1.0,
                                     parallax=0.02, tint=(0.8, 1.1)),
    "Facade_MetalCladdingGreen": dict(set="ms:corrugated_metal_sheet_109b6f59", tile=(2, 2), recolor=(0.30, 0.42, 0.33), amount=1.0,
                                      parallax=0.02, tint=(0.8, 1.1)),
    "Facade_MetalCladdingWhite": dict(set="ms:dirty_corrugated_metal_sheet_5b15681e", tile=(1, 1), value=1.1, parallax=0.02,
                                      tint=(0.85, 1.1)),
    "Roof_Clay": dict(set="Roof/clay_roof_tiles_03", tile=(2.5, 2.5), tint=(0.75, 1.1), parallax=0.03),
    "Roof_ClayOld": dict(set="Roof/clay_roof_tiles", tile=(2.5, 2.5), tint=(0.7, 1.1), parallax=0.03),
    "Roof_ConcreteAnthracite": dict(set="Generated/roof_tiles_anthracite", tile=(2.5, 2.5), tint=(0.8, 1.1), parallax=0.03),
    "Roof_ConcreteGrey": dict(set="Roof/grey_roof_tiles", tile=(2.5, 2.5), tint=(0.8, 1.1), parallax=0.03),
    "Roof_Slate": dict(set="Roof/roof_slates_02", tile=(2.5, 2.5), tint=(0.75, 1.05), parallax=0.025),
    "Roof_MetalGreen": dict(set="ms:painted_roof_764bd55b", tile=(2, 1), parallax=0.02, tint=(0.85, 1.1)),
}
SECTION_ALIASES = ("Facade_Brick", "Facade_Plaster", "Facade_Concrete", "Facade_Metal", "Roof_Tiles")


def add_weathering(m, color, rough, normal, ao):
    """Hook for the facade weathering overlay (#84): rain streaks, plinth dirt, moss, repairs, faded paint. Takes and returns
    (node, pin) pairs for colour, roughness, normal and occlusion; for now it passes them through."""
    return color, rough, normal, ao


def texture_object(m, name, set_ref, kind, sampler, x, y):
    """Texture object parameter named after the map kind, defaulting to the given set."""
    return expr(m, unreal.MaterialExpressionTextureObjectParameter, x, y, parameter_name=name,
                texture=set_asset(set_ref, kind), sampler_type=sampler)


def build_master():
    """Rebuilds M_FacadeMaster in place (instances keep their parent)."""
    if eal.does_asset_exist(MASTER):
        m = unreal.load_asset(MASTER)
        mel.delete_all_material_expressions(m)
    else:
        m = cm.asset_tools.create_asset("M_FacadeMaster", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("used_with_nanite", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)  # the kit buildings are instanced meshes
    default_set = "ms:brick_facade_efa35b96"

    texcoord = expr(m, unreal.MaterialExpressionTextureCoordinate, -2400, 0)
    vertex_color = expr(m, unreal.MaterialExpressionVertexColor, -2400, 200)
    tile = expr(m, unreal.MaterialExpressionVectorParameter, -2400, 400, parameter_name="TileMetres",
                default_value=unreal.LinearColor(2, 2, 0, 0))
    tile_xy = expr(m, unreal.MaterialExpressionComponentMask, -2200, 400, r=True, g=True, b=False, a=False)
    link(tile, "", tile_xy, "")
    uv_node = expr(m, unreal.MaterialExpressionCustom, -2000, 100, code=UV_HLSL, description="FacadeUV",
                   output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT2,
                   inputs=[custom_input("UV"), custom_input("Tile"), custom_input("Variation"), custom_input("WorldPos"),
                           custom_input("Board")])
    link(texcoord, "", uv_node, "UV")
    link(expr(m, unreal.MaterialExpressionWorldPosition, -2200, 600), "", uv_node, "WorldPos")
    link(scalar(m, "Board", 0.0, -2200, 700), "", uv_node, "Board")
    link(tile_xy, "", uv_node, "Tile")
    link(vertex_color, "G", uv_node, "Variation")
    uv = cm.parallax_uv(m, uv_node, set_asset(default_set, "Height"))

    surface = expr(m, unreal.MaterialExpressionCustom, -800, 0, code=SURFACE_HLSL, description="FacadeSurface",
                   output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                   include_file_paths=[cm.TERRAIN_INCLUDE, FACADE_INCLUDE])
    names = ["Uv", "WorldPos", "AntiTile", "HueShift", "HueJitter", "Saturation", "Value", "ScanMean", "Recolor",
             "RecolorAmount", "BuildingTint", "TintMin", "TintMax", "Variation", "NormalStrength", "RoughnessScale"]
    surface.set_editor_property("inputs", [custom_input(n) for n in ("TexColor", "TexNormal", "TexRough", "TexAO", *names)])
    surface.set_editor_property("additional_outputs", [
        cm.custom_output("NormalOut", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
        cm.custom_output("RoughOut", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
        cm.custom_output("AOOut", unreal.CustomMaterialOutputType.CMOT_FLOAT1)])
    samplers = {"BaseColor": unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, "Normal": unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL,
                "Roughness": unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE,
                "AO": unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE}
    pins = {"BaseColor": "TexColor", "Normal": "TexNormal", "Roughness": "TexRough", "AO": "TexAO"}
    for index, kind in enumerate(MAP_KINDS):
        link(texture_object(m, kind, default_set, kind, samplers[kind], -1300, -300 + index * 130), "", surface, pins[kind])
    link(uv, "", surface, "Uv")
    link(expr(m, unreal.MaterialExpressionWorldPosition, -1300, 300), "", surface, "WorldPos")
    link(vertex_color, "R", surface, "BuildingTint")
    link(vertex_color, "G", surface, "Variation")
    defaults = dict(AntiTile=0.0, HueShift=0.0, HueJitter=0.03, Saturation=1.0, Value=1.0, RecolorAmount=0.0, TintMin=1.0,
                    TintMax=1.0, NormalStrength=1.0, RoughnessScale=1.0)
    y = 400
    for name, default in defaults.items():
        link(scalar(m, name, default, -1300, y), "", surface, name)
        y += 100
    for name, default in (("ScanMean", (0.3, 0.3, 0.3)), ("Recolor", (0.5, 0.5, 0.5))):
        vector = expr(m, unreal.MaterialExpressionVectorParameter, -1500, y, parameter_name=name,
                      default_value=unreal.LinearColor(*default, 1))
        rgb = expr(m, unreal.MaterialExpressionComponentMask, -1300, y, r=True, g=True, b=True, a=False)
        link(vector, "", rgb, "")
        link(rgb, "", surface, name)
        y += 100

    color, rough, normal, ao = (surface, ""), (surface, "RoughOut"), (surface, "NormalOut"), (surface, "AOOut")
    color, rough, normal, ao = add_weathering(m, color, rough, normal, ao)

    # Old painted windows for the streamed buildings, a static switch so kit pieces pay nothing.
    window = expr(m, unreal.MaterialExpressionCustom, -800, 1300, code=cm.WINDOW_HLSL, description="WindowMask",
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT2,
                  inputs=[custom_input("UV"), custom_input("Variation"), custom_input("FloorHeight")])
    link(texcoord, "", window, "UV")
    link(vertex_color, "G", window, "Variation")
    link(scalar(m, "FloorHeight", 3.0, -1100, 1500), "", window, "FloorHeight")
    glass_mask = expr(m, unreal.MaterialExpressionComponentMask, -600, 1300, r=True, g=False, b=False, a=False)
    frame_mask = expr(m, unreal.MaterialExpressionComponentMask, -600, 1450, r=False, g=True, b=False, a=False)
    link(window, "", glass_mask, "")
    link(window, "", frame_mask, "")
    frame_color = expr(m, unreal.MaterialExpressionVectorParameter, -600, 1600, parameter_name="FrameColor",
                       default_value=unreal.LinearColor(0.75, 0.75, 0.72, 1))
    glass_color = expr(m, unreal.MaterialExpressionVectorParameter, -600, 1750, parameter_name="GlassColor",
                       default_value=unreal.LinearColor(0.012, 0.014, 0.016, 1))
    with_frame = expr(m, unreal.MaterialExpressionLinearInterpolate, -300, -500)
    link(*color, with_frame, "A")
    link(frame_color, "", with_frame, "B")
    link(frame_mask, "", with_frame, "Alpha")
    with_glass = expr(m, unreal.MaterialExpressionLinearInterpolate, -150, -500)
    link(with_frame, "", with_glass, "A")
    link(glass_color, "", with_glass, "B")
    link(glass_mask, "", with_glass, "Alpha")
    rough_frame = expr(m, unreal.MaterialExpressionLinearInterpolate, -300, 200, const_b=0.45)
    link(*rough, rough_frame, "A")
    link(frame_mask, "", rough_frame, "Alpha")
    rough_glass = expr(m, unreal.MaterialExpressionLinearInterpolate, -150, 200, const_b=0.04)
    link(rough_frame, "", rough_glass, "A")
    link(glass_mask, "", rough_glass, "Alpha")
    flat = expr(m, unreal.MaterialExpressionConstant3Vector, -500, 0, constant=unreal.LinearColor(0, 0, 1, 1))
    any_window = expr(m, unreal.MaterialExpressionAdd, -500, 100)
    link(glass_mask, "", any_window, "A")
    link(frame_mask, "", any_window, "B")
    normal_windows = expr(m, unreal.MaterialExpressionLinearInterpolate, -300, -100)
    link(*normal, normal_windows, "A")
    link(flat, "", normal_windows, "B")
    link(any_window, "", normal_windows, "Alpha")

    def windows_switch(true_in, false_node, x, y):
        node = expr(m, unreal.MaterialExpressionStaticSwitchParameter, x, y, parameter_name="Windows", default_value=False)
        link(true_in, "", node, "True")
        link(false_node[0], false_node[1], node, "False")
        return node

    out_color = windows_switch(with_glass, color, 50, -400)
    out_rough = windows_switch(rough_glass, rough, 50, 200)
    out_normal = windows_switch(normal_windows, normal, 50, -100)
    (out_color, _), (out_rough, _), (out_normal, _) = cm.add_season(
        m, (out_color, ""), (out_rough, ""), (out_normal, ""), snow_keep=1.0, leaf_keep=0.0)
    out_color, out_rough, out_normal = cm.add_wetness(m, out_color, out_rough, out_normal)
    cm.add_lit_windows(m, texcoord, vertex_color, glass_mask)
    mel.connect_material_property(out_color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(out_rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(out_normal, "", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(ao[0], ao[1], unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    return m


def build_instance(master, name, settings, board=False):
    """M_<name> as an instance of the master with the settings of FACADES. With board, MB_<name> in the Board folder, mapped
    by world x and height for the flat panels of Scripts/create_material_board.py."""
    path = f"{BOARD_FOLDER}/MB_{name}" if board else f"{FOLDER}/M_{name}"
    existing = unreal.load_asset(path) if eal.does_asset_exist(path) else None
    mi = existing or cm.asset_tools.create_asset(path.split("/")[-1], path.rsplit("/", 1)[0], unreal.MaterialInstanceConstant,
                                                 unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_scalar_parameter_value(mi, "Board", 1.0 if board else 0.0)
    mel.set_material_instance_parent(mi, master)
    set_ref = settings["set"]
    for kind in (*MAP_KINDS, "Height"):
        texture = set_asset(set_ref, kind)
        if texture is None and kind == "AO":
            texture = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
        if texture is not None:
            mel.set_material_instance_texture_parameter_value(mi, kind, texture)
    tile_u, tile_v = settings["tile"]
    mel.set_material_instance_vector_parameter_value(mi, "TileMetres", unreal.LinearColor(tile_u, tile_v, 0, 0))
    mean = SCAN_MEAN.get(set_ref, (0.3, 0.3, 0.3))
    mel.set_material_instance_vector_parameter_value(mi, "ScanMean", unreal.LinearColor(*mean, 1))
    if "recolor" in settings:
        mel.set_material_instance_vector_parameter_value(mi, "Recolor", unreal.LinearColor(*srgb_to_linear(settings["recolor"]), 1))
        mel.set_material_instance_scalar_parameter_value(mi, "RecolorAmount", settings.get("amount", 1.0))
    scalars = {"HueShift": settings.get("hue", 0.0), "Saturation": settings.get("saturation", 1.0),
               "Value": settings.get("value", 1.0), "NormalStrength": settings.get("normal", 1.0),
               "RoughnessScale": settings.get("rough", 1.0), "AntiTile": settings.get("anti_tile", 0.0),
               "TintMin": settings.get("tint", (1.0, 1.0))[0], "TintMax": settings.get("tint", (1.0, 1.0))[1]}
    for parameter, value in scalars.items():
        mel.set_material_instance_scalar_parameter_value(mi, parameter, value)
    parallax = settings.get("parallax") if set_asset(set_ref, "Height") else None  # some scans have no height
    mel.set_material_instance_static_switch_parameter_value(mi, "Parallax", parallax is not None)
    if parallax is not None:
        mel.set_material_instance_scalar_parameter_value(mi, "HeightRatio", parallax)
    mel.set_material_instance_static_switch_parameter_value(mi, "Windows", bool(settings.get("windows")))
    snow_keep, leaf_keep = (1.0, 0.35) if name.startswith("Roof_") else (1.0, 0.0)
    mel.set_material_instance_scalar_parameter_value(mi, "SnowKeep", snow_keep)
    mel.set_material_instance_scalar_parameter_value(mi, "LeafKeep", leaf_keep)
    mel.update_material_instance(mi)
    eal.save_loaded_asset(mi)


def main():
    master = build_master()
    for name, settings in FACADES.items():
        missing = [kind for kind in ("BaseColor", "Normal", "Roughness") if set_asset(settings["set"], kind) is None]
        if missing:
            unreal.log_warning(f"create_facade_materials: {name} skipped, set {settings['set']} lacks {missing}")
            continue
        build_instance(master, name, settings)
        build_instance(master, name, settings, board=True)
    unreal.log_warning(f"create_facade_materials: master + {len(FACADES)} instances done")


main()
