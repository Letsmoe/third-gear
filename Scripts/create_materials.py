"""Creates the master surface material and one material instance per mesh section (M_<Section>).

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_materials.py -unattended -nosplash

Mesh UVs are in metres (see Tools/osmimport/osmimport/mesh.py): horizontal surfaces use world x/y, walls use
u = distance along the wall, v = -(height above the building base). TileSize converts metres to texture repeats.
Re-run Scripts/import_osm_area.py afterwards so meshes pick up the new materials.
"""
import unreal

FOLDER = "/Game/World/Materials"
MASTER = f"{FOLDER}/M_SurfaceMaster"
TEX = "/Game/Textures"

# section -> (texture folder/set, tile size m, overrides)
SECTIONS = {
    "Road_Asphalt": ("Asphalt/Asphalt015", 2.5, {"tint": (0.55, 0.55)}),
    "Road_Cobble": ("Cobble/cobblestone_floor_08", 2.0, {}),
    "Road_Pavers": ("Pavers/brick_pavement", 2.0, {}),
    "Pavement": ("Pavers/concrete_pavement_02", 2.5, {"tint": (0.8, 0.8)}),
    "Path_Paved": ("Pavers/brick_pavement_03", 2.0, {}),
    "Path_Gravel": ("Ground/gravel_ground_01", 3.0, {}),
    "Kerb": ("Pavers/granite_tile_04", 1.0, {}),
    "Bridge_Concrete": ("Concrete/concrete_wall_001", 3.0, {}),
    "Facade_Brick": ("Brick/brick_wall_006", 2.2, {"windows": True, "tint": (0.7, 1.15)}),
    "Facade_Plaster": ("Plaster/plastered_wall", 2.0, {"windows": True, "tint": (0.85, 1.1)}),
    "Facade_Concrete": ("Concrete/concrete_wall_004", 3.0, {"windows": True, "tint": (0.8, 1.1)}),
    "Facade_Metal": ("Metal/corrugated_iron", 2.0, {"tint": (0.7, 1.2)}),
    "Roof_Tiles": ("Roof/clay_roof_tiles_03", 2.5, {"tint": (0.75, 1.1)}),
    "Roof_Flat": ("Asphalt/Asphalt031", 3.0, {"tint": (0.45, 0.6)}),
}

WINDOW_HLSL = """
float h = -UV.y;
float spacing = lerp(2.6, 3.6, Variation);
float2 g = float2(UV.x / spacing, h / FloorHeight);
float2 f = frac(g);
float2 w = fwidth(g) * 1.5 + 1e-4;
float inner = saturate((f.x - 0.30) / w.x) * saturate((0.70 - f.x) / w.x) * saturate((f.y - 0.32) / w.y) * saturate((0.80 - f.y) / w.y);
float outer = saturate((f.x - 0.27) / w.x) * saturate((0.73 - f.x) / w.x) * saturate((f.y - 0.29) / w.y) * saturate((0.83 - f.y) / w.y);
float above = saturate((h - 0.5) / 0.05);
return float2(inner, saturate(outer - inner)) * above;
"""

# Terrain layers (vertex colour weights from Tools/osmimport/osmimport/landcover.py): name -> (texture set, tile m, tint)
TERRAIN_MASTER = f"{FOLDER}/M_TerrainMaster"
TERRAIN_LAYERS = {
    "Lawn": ("Ground/rocky_terrain_02", 8.0, (0.9, 1.0, 0.85)),
    "Meadow": ("Ground/grass_ground", 3.5, (0.72, 1.0, 0.55)),
    "Field": ("Ground/farm_soil", 3.0, (1.0, 1.0, 1.0)),
    "Forest": ("Ground/forest_leaves_04", 2.5, (0.8, 0.8, 0.8)),
}

# Blends four ground layers by vertex colour (R meadow, G field, B forest, none = lawn). Each layer is sampled at two
# scales/rotations mixed by low-frequency noise (breaks visible tiling) and gets a large-scale brightness variation.
TERRAIN_HLSL = """
float2 p = UV;
float n1 = DgValueNoise(p / 23.0);
float n2 = DgValueNoise(p / 61.0 + 17.3);
float macro = lerp(0.86, 1.12, DgValueNoise(p / 140.0 + 3.1));
float detailMix = smoothstep(0.35, 0.65, n1);
float2x2 rot = float2x2(0.809, -0.588, 0.588, 0.809);
float3 w = saturate(VC.rgb + (n2 - 0.5) * 0.5 * saturate(VC.rgb * (1 - VC.rgb) * 4));
float wl = saturate(1 - w.r - w.g - w.b);
float sumw = wl + w.r + w.g + w.b + 1e-4;
float4 lw = float4(wl, w.r, w.g, w.b) / sumw;

float3 col = 0; float3 nrm = 0; float rough = 0;
DG_LAYER(LawnC, LawnN, LawnR, TileLawn, TintLawn, lw.x)
DG_LAYER(MeadowC, MeadowN, MeadowR, TileMeadow, TintMeadow, lw.y)
DG_LAYER(FieldC, FieldN, FieldR, TileField, TintField, lw.z)
DG_LAYER(ForestC, ForestN, ForestR, TileForest, TintForest, lw.w)
Normal = normalize(float3(nrm.xy, 1));
Roughness = rough;
return col * macro;
"""
TERRAIN_INCLUDE = "/Plugin/MapRuntime/Private/TerrainNoise.ush"  # DgValueNoise, DG_LAYER

mel = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def tex(set_path, kind):
    name = set_path.split("/")[-1]
    path = f"{TEX}/{set_path}/T_{name}_{kind}"
    return unreal.load_asset(path) if eal.does_asset_exist(path) else None


def expr(material, cls, x, y, **props):
    e = mel.create_material_expression(material, cls, x, y)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def link(a, a_out, b, b_in):
    if not mel.connect_material_expressions(a, a_out, b, b_in):
        raise RuntimeError(f"failed to connect {a.get_name()}.{a_out} -> {b.get_name()}.{b_in}")


def custom_input(name):
    ci = unreal.CustomInput()
    ci.set_editor_property("input_name", name)
    return ci


def scalar(material, name, default, x, y):
    return expr(material, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=default)


def tex_param(material, name, texture, sampler, uv, x, y):
    e = expr(material, unreal.MaterialExpressionTextureSampleParameter2D, x, y, parameter_name=name, texture=texture,
             sampler_type=sampler)
    link(uv, "", e, "UVs")
    return e


def build_master():
    if eal.does_asset_exist(MASTER):
        m = unreal.load_asset(MASTER)
        mel.delete_all_material_expressions(m)  # rebuild the graph in place (instances keep their parent)
    else:
        m = asset_tools.create_asset("M_SurfaceMaster", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("used_with_nanite", True)

    texcoord = expr(m, unreal.MaterialExpressionTextureCoordinate, -1800, 0)
    tile = scalar(m, "TileSize", 2.0, -1800, 150)
    uv = expr(m, unreal.MaterialExpressionDivide, -1600, 50)
    link(texcoord, "", uv, "A")
    link(tile, "", uv, "B")

    default_set = "Asphalt/Asphalt015"
    color = tex_param(m, "BaseColor", tex(default_set, "BaseColor"), unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, uv, -1300, -400)
    normal = tex_param(m, "Normal", tex(default_set, "Normal"), unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, uv, -1300, -100)
    rough = tex_param(m, "Roughness", tex(default_set, "Roughness"), unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE, uv, -1300, 200)
    ao = tex_param(m, "AO", tex("Asphalt/asphalt_02", "AO"), unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE, uv, -1300, 500)

    # Per-object brightness variation from vertex colour R (buildings), off by default (min = max = 1).
    vcol = expr(m, unreal.MaterialExpressionVertexColor, -1300, 800)
    tint = expr(m, unreal.MaterialExpressionLinearInterpolate, -1000, 800)
    link(scalar(m, "TintMin", 1.0, -1300, 950), "", tint, "A")
    link(scalar(m, "TintMax", 1.0, -1300, 1050), "", tint, "B")
    link(vcol, "R", tint, "Alpha")
    tinted = expr(m, unreal.MaterialExpressionMultiply, -800, -400)
    link(color, "RGB", tinted, "A")
    link(tint, "", tinted, "B")

    rough_scaled = expr(m, unreal.MaterialExpressionMultiply, -800, 200)
    link(rough, "R", rough_scaled, "A")
    link(scalar(m, "RoughnessScale", 1.0, -1000, 300), "", rough_scaled, "B")

    # Procedural windows (static switch so non-facade instances pay nothing).
    custom = expr(m, unreal.MaterialExpressionCustom, -1000, 1300, code=WINDOW_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT2, description="WindowMask",
                  inputs=[custom_input("UV"), custom_input("Variation"), custom_input("FloorHeight")])
    link(texcoord, "", custom, "UV")
    link(vcol, "G", custom, "Variation")
    link(scalar(m, "FloorHeight", 3.0, -1300, 1500), "", custom, "FloorHeight")
    glass_mask = expr(m, unreal.MaterialExpressionComponentMask, -800, 1300, r=True, g=False, b=False, a=False)
    frame_mask = expr(m, unreal.MaterialExpressionComponentMask, -800, 1450, r=False, g=True, b=False, a=False)
    link(custom, "", glass_mask, "")
    link(custom, "", frame_mask, "")

    frame_color = expr(m, unreal.MaterialExpressionVectorParameter, -800, 1600, parameter_name="FrameColor",
                       default_value=unreal.LinearColor(0.75, 0.75, 0.72, 1))
    glass_color = expr(m, unreal.MaterialExpressionVectorParameter, -800, 1750, parameter_name="GlassColor",
                       default_value=unreal.LinearColor(0.012, 0.014, 0.016, 1))
    with_frame = expr(m, unreal.MaterialExpressionLinearInterpolate, -500, -500)
    link(tinted, "", with_frame, "A")
    link(frame_color, "", with_frame, "B")
    link(frame_mask, "", with_frame, "Alpha")
    with_glass = expr(m, unreal.MaterialExpressionLinearInterpolate, -350, -500)
    link(with_frame, "", with_glass, "A")
    link(glass_color, "", with_glass, "B")
    link(glass_mask, "", with_glass, "Alpha")

    rough_frame = expr(m, unreal.MaterialExpressionLinearInterpolate, -500, 200, const_b=0.45)
    link(rough_scaled, "", rough_frame, "A")
    link(frame_mask, "", rough_frame, "Alpha")
    rough_glass = expr(m, unreal.MaterialExpressionLinearInterpolate, -350, 200, const_b=0.04)
    link(rough_frame, "", rough_glass, "A")
    link(glass_mask, "", rough_glass, "Alpha")

    flat = expr(m, unreal.MaterialExpressionConstant3Vector, -700, 0, constant=unreal.LinearColor(0, 0, 1, 1))
    any_window = expr(m, unreal.MaterialExpressionAdd, -700, 100)
    link(glass_mask, "", any_window, "A")
    link(frame_mask, "", any_window, "B")
    normal_windows = expr(m, unreal.MaterialExpressionLinearInterpolate, -500, -100)
    link(normal, "RGB", normal_windows, "A")
    link(flat, "", normal_windows, "B")
    link(any_window, "", normal_windows, "Alpha")

    def switch(true_in, false_in, x, y):
        s = expr(m, unreal.MaterialExpressionStaticSwitchParameter, x, y, parameter_name="Windows", default_value=False)
        link(true_in, "", s, "True")
        link(false_in, "", s, "False")
        return s

    out_color = switch(with_glass, tinted, -150, -400)
    out_rough = switch(rough_glass, rough_scaled, -150, 200)
    out_normal = switch(normal_windows, normal, -150, -100)
    mel.connect_material_property(out_color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(out_rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(out_normal, "", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(ao, "R", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    return m


def build_instance(master, section, set_path, tile_size, opts):
    path = f"{FOLDER}/M_{section}"
    existing = unreal.load_asset(path) if eal.does_asset_exist(path) else None
    if existing is not None and not isinstance(existing, unreal.MaterialInstanceConstant):
        eal.delete_asset(path)  # old placeholder material
        existing = None
    # Update instances in place so meshes keep their references.
    mi = existing or asset_tools.create_asset(f"M_{section}", FOLDER, unreal.MaterialInstanceConstant,
                                              unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(mi, master)
    for kind in ("BaseColor", "Normal", "Roughness", "AO"):
        t = tex(set_path, kind)
        if t:
            mel.set_material_instance_texture_parameter_value(mi, kind, t)
        elif kind == "AO":
            mel.set_material_instance_texture_parameter_value(
                mi, kind, unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"))
    mel.set_material_instance_scalar_parameter_value(mi, "TileSize", tile_size)
    if "tint" in opts:
        lo, hi = opts["tint"]
        mel.set_material_instance_scalar_parameter_value(mi, "TintMin", lo)
        mel.set_material_instance_scalar_parameter_value(mi, "TintMax", hi)
    if opts.get("windows"):
        mel.set_material_instance_static_switch_parameter_value(mi, "Windows", True)
    mel.update_material_instance(mi)
    eal.save_loaded_asset(mi)


def build_terrain_master():
    if eal.does_asset_exist(TERRAIN_MASTER):
        m = unreal.load_asset(TERRAIN_MASTER)
        mel.delete_all_material_expressions(m)
    else:
        m = asset_tools.create_asset("M_TerrainMaster", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("used_with_nanite", True)

    inputs = [custom_input("UV"), custom_input("VC")]
    custom = expr(m, unreal.MaterialExpressionCustom, -600, 0, code=TERRAIN_HLSL,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="TerrainLayers")
    outs = []
    for name, kind in (("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
                       ("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1)):
        o = unreal.CustomOutput()
        o.set_editor_property("output_name", name)
        o.set_editor_property("output_type", kind)
        outs.append(o)
    sources = []
    y = -900
    for layer, (set_path, tile, tint) in TERRAIN_LAYERS.items():
        for suffix, kind, sampler in (("C", "BaseColor", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR),
                                      ("N", "Normal", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL),
                                      ("R", "Roughness", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)):
            t = expr(m, unreal.MaterialExpressionTextureObjectParameter, -1100, y, parameter_name=f"{layer}{kind}",
                     texture=tex(set_path, kind), sampler_type=sampler)
            inputs.append(custom_input(f"{layer}{suffix}"))
            sources.append((t, f"{layer}{suffix}"))
            y += 120
        sources.append((scalar(m, f"Tile{layer}", tile, -1100, y), f"Tile{layer}"))
        inputs.append(custom_input(f"Tile{layer}"))
        y += 120
        tint_param = expr(m, unreal.MaterialExpressionVectorParameter, -1100, y, parameter_name=f"Tint{layer}",
                          default_value=unreal.LinearColor(*tint, 1))
        tint_rgb = expr(m, unreal.MaterialExpressionComponentMask, -900, y, r=True, g=True, b=True, a=False)
        link(tint_param, "", tint_rgb, "")
        sources.append((tint_rgb, f"Tint{layer}"))
        inputs.append(custom_input(f"Tint{layer}"))
        y += 120
    custom.set_editor_property("inputs", inputs)
    custom.set_editor_property("additional_outputs", outs)
    custom.set_editor_property("include_file_paths", [TERRAIN_INCLUDE])
    custom.set_editor_property("code", TERRAIN_HLSL)
    link(expr(m, unreal.MaterialExpressionTextureCoordinate, -900, -300), "", custom, "UV")
    link(expr(m, unreal.MaterialExpressionVertexColor, -900, -200), "", custom, "VC")
    for src, pin in sources:
        link(src, "", custom, pin)
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(custom, "Normal", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(custom, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)

    # The terrain section's material becomes an instance of the terrain master.
    path = f"{FOLDER}/M_Terrain_Grass"
    existing = unreal.load_asset(path) if eal.does_asset_exist(path) else None
    mi = existing or asset_tools.create_asset("M_Terrain_Grass", FOLDER, unreal.MaterialInstanceConstant,
                                              unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(mi, m)
    mel.update_material_instance(mi)
    eal.save_loaded_asset(mi)
    return m


# Sections without a texture set: (base colour sRGB 0..1, roughness, metallic). Same look as the old placeholders.
PLAIN = {
    "Marking_White": ((0.85, 0.85, 0.85), 0.6, 0.0),
    "Facade_Glass": ((0.6, 0.7, 0.7), 0.1, 0.0),
    "Roof_Glass": ((0.6, 0.7, 0.7), 0.1, 0.0),
}


def build_plain(section, color, roughness, metallic):
    """Single-colour material M_<section>, rebuilt in place."""
    path = f"{FOLDER}/M_{section}"
    if eal.does_asset_exist(path):
        m = unreal.load_asset(path)
        mel.delete_all_material_expressions(m)
    else:
        m = asset_tools.create_asset(f"M_{section}", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("used_with_nanite", True)
    base = expr(m, unreal.MaterialExpressionConstant3Vector, -400, 0,
                constant=unreal.LinearColor(*[c ** 2.2 for c in color], 1.0))  # sRGB -> linear
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(expr(m, unreal.MaterialExpressionConstant, -400, 200, r=roughness), "",
                                  unreal.MaterialProperty.MP_ROUGHNESS)
    if metallic:
        mel.connect_material_property(expr(m, unreal.MaterialExpressionConstant, -400, 300, r=metallic), "",
                                      unreal.MaterialProperty.MP_METALLIC)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)


def build_water():
    path = f"{FOLDER}/M_Water"
    if eal.does_asset_exist(path):
        m = unreal.load_asset(path)
        mel.delete_all_material_expressions(m)
    else:
        m = asset_tools.create_asset("M_Water", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("used_with_nanite", True)
    col = expr(m, unreal.MaterialExpressionConstant3Vector, -400, 0, constant=unreal.LinearColor(0.008, 0.012, 0.010, 1))
    mel.connect_material_property(col, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = expr(m, unreal.MaterialExpressionConstant, -400, 200, r=0.03)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)


master = build_master()
for section, (set_path, tile_size, opts) in SECTIONS.items():
    build_instance(master, section, set_path, tile_size, opts)
for section, (color, roughness, metallic) in PLAIN.items():
    build_plain(section, color, roughness, metallic)
build_water()
build_terrain_master()
unreal.log_warning(f"create_materials: master + {len(SECTIONS)} instances + water done")
