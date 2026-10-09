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
POM_FUNCTION = "/Engine/Functions/Engine_MaterialFunctions01/Texturing/ParallaxOcclusionMapping"

# Albedo calibration: the tint multiplies the scan's colour, so tint = wanted reflectance / the scan's mean reflectance
# (Asphalt015 0.175, concrete_pavement_02 0.207, PavingStones092 0.28, brick_wall_10 0.046). Wanted: worn asphalt 0.12 to
# 0.18, concrete slabs 0.25 to 0.35, red brick paving about 0.25, dark red-brown clinker 0.13 to 0.19.
# section -> (texture folder/set, tile size m, overrides)
SECTIONS = {
    "Road_Asphalt": ("Asphalt/Asphalt015", 2.5, {"tint": (0.85, 0.85), "roughness_scale": 1.4, "cracks": 0.5}),
    "Road_Cobble": ("Cobble/cobblestone_floor_08", 2.0, {"parallax": 0.03}),
    "Road_Pavers": ("Pavers/PavingStones092", 2.0, {"tint": (0.9, 0.9), "parallax": 0.015}),
    "Pavement": ("Pavers/concrete_pavement_02", 2.5, {"tint": (1.35, 1.35), "parallax": 0.012}),
    "Path_Paved": ("Pavers/brick_pavement_03", 2.0, {"parallax": 0.015}),
    "Path_Gravel": ("Ground/gravel_ground_01", 3.0, {}),
    "Kerb": ("Pavers/granite_tile_04", 1.0, {}),
    "Bridge_Concrete": ("Concrete/concrete_wall_001", 3.0, {}),
    "Facade_Brick": ("Brick/brick_wall_10", 2.2, {"windows": True, "tint": (2.8, 4.0), "parallax": 0.012}),
    "Facade_Plaster": ("Plaster/plastered_wall", 2.0, {"windows": True, "tint": (0.85, 1.1)}),
    "Facade_Concrete": ("Concrete/concrete_wall_004", 3.0, {"windows": True, "tint": (0.8, 1.1)}),
    "Facade_Metal": ("Metal/corrugated_iron", 2.0, {"tint": (0.7, 1.2)}),
    "Roof_Tiles": ("Roof/clay_roof_tiles_03", 2.5, {"tint": (0.75, 1.1), "parallax": 0.03}),
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

# Lit windows at night: about a third of the windows glow warm, each with its own colour and brightness, from the
# same window grid as WINDOW_HLSL. Returns luminance in cd/m2 before the glass mask.
LIT_WINDOW_HLSL = """
float h = -UV.y;
float spacing = lerp(2.6, 3.6, Variation);
float2 cell = floor(float2(UV.x / spacing, h / FloorHeight));
// Integer hash of the window cell and the building: a sin() hash amplifies the vertex colour's interpolation noise
// into a different random number per pixel, which speckled every window.
uint3 key = uint3(int2(cell) + 4096, uint(floor(Variation * 255.0 + 0.5)));
uint hash = key.x * 73856093u ^ key.y * 19349663u ^ key.z * 83492791u;
hash = (hash ^ (hash >> 13)) * 1274126177u;
float r = float(hash & 65535u) / 65535.0;
float r2 = float((hash >> 16) & 65535u) / 65535.0;
float lit = step(r, 0.28) * Night * step(0.5, h);
// Inside the pane: the ceiling lamp lights the top more than the sill, and some windows have half-drawn curtains.
float2 pane = frac(float2(UV.x / spacing, h / FloorHeight));
float vertical = saturate((pane.y - 0.32) / 0.48);
float interior = lerp(0.55, 1.0, vertical);
float curtain = step(r2, 0.5) * (1.0 - 0.6 * saturate((abs(pane.x - 0.5) - 0.08) / 0.04));
float3 warm = lerp(float3(1.0, 0.6, 0.32), float3(1.0, 0.8, 0.6), frac(r2 * 7.13));
return lit * warm * interior * lerp(1.0, 0.75, curtain) * lerp(0.8, 3.0, frac(r * 3.7 + r2));
"""

# Snow and fallen leaves from the season (/Game/World/MPC_Weather SnowCover, FallenLeaves), on level surfaces only.
# SnowKeep and LeafKeep (instance parameters) say how much a surface holds: traffic clears roads, facades hold none.
# Snow thins out in patches at its edges; leaves lie in drifts. x = snow, y = leaves.
SEASON_MASK_HLSL = """
float up = saturate((NormalZ - 0.6) / 0.3);
float2 p = WorldPos.xy / 100.0;
float n = DgValueNoise(p / 2.3) * 0.6 + DgValueNoise(p / 0.6 + 4.2) * 0.4;
float snow = saturate((Snow * SnowKeep * 1.25 - 0.25 * n) / 0.08) * up;
float drift = DgValueNoise(p / 1.1 + 9.7) * 0.7 + DgValueNoise(p / 0.25 + 1.3) * 0.3;
// Drifts, not a carpet: even at the peak of leaf fall about a third of a pavement or lawn is covered.
float leaves = saturate((Leaves * LeafKeep * 0.7 - drift + 0.05) / 0.08) * up * (1.0 - snow);
return float2(snow, leaves);
"""
# The ground's snow uses the scans only for grain: SNOW_TONE_HLSL brings their brightness to a physical albedo and adds
# soft undulation. Scan (mean linear brightness, target albedo) per set; snow_02 is a dull grey scan (0.38), fresh snow
# is 0.8-0.9, trodden snow on footways is greyer, gritted road snow stays dark.
SNOW_TONE = {"Snow/snow_02": (0.38, 0.86), "Snow/snow_01": (0.48, 0.74), "Snow/asphalt_snow": (0.216, 0.26)}
SNOW_TONE_HLSL = """
float2 p = WorldPos.xy / 100.0;
// Same tone and macro variation as the layer mesh (create_snow_material.py), so the layer's fade-out is invisible.
float3 relative = max(Scan, 0.001) / Mean;
float macro = lerp(0.93, 1.03, DgValueNoise(p / 5.0 + 11.0));
float3 Colour = min(Target * float3(0.975, 0.99, 1.0) * pow(relative, 0.6) * macro, 0.93);
// Soft undulation on top of the scanned grain: a tangent-space tilt from the gradient of two low-frequency noises.
float2 q = p / 1.7;
float step_size = 0.05;
float h = DgValueNoise(q) * 1.0 + DgValueNoise(p / 0.45 + 3.0) * 0.35;
float hx = DgValueNoise(q + float2(step_size, 0)) + DgValueNoise((p + float2(step_size, 0) * 1.7) / 0.45 + 3.0) * 0.35;
float hy = DgValueNoise(q + float2(0, step_size)) + DgValueNoise((p + float2(0, step_size) * 1.7) / 0.45 + 3.0) * 0.35;
float2 tilt = float2(hx - h, hy - h) / step_size * 0.035;
NormalOut = normalize(float3(ScanNormal.xy + tilt, max(ScanNormal.z, 0.2)));
// Snow is a very rough, matte surface whatever the scan says.
RoughOut = lerp(ScanRough, 1.0, 0.55);
return Colour;
"""
# Untrodden snow by default (lawns, roofs); footways get trodden snow and roads gritted asphalt (SEASON_SNOW_SETS).
SNOW_SET = "Snow/snow_02"
SEASON_SNOW_SETS = {"Road_": "Snow/asphalt_snow", "Bridge_": "Snow/asphalt_snow", "Pavement": "Snow/snow_01",
                    "Path_": "Snow/snow_01"}
# Section prefix -> (snow kept, leaves kept). Traffic clears roads of both; footways and paths hold everything.
SEASON_KEEP = {
    "Road_": (0.55, 0.35), "Bridge_": (0.7, 0.3), "Pavement": (0.9, 1.0), "Path_": (1.0, 1.0), "Kerb": (0.85, 0.8),
    "Roof_": (1.0, 0.35), "Facade_": (1.0, 0.0),
}
LEAF_SET = "Ground/forest_leaves_04"

# Wet surfaces from the weather (/Game/World/MPC_Weather). Ground gets fully wet, walls only damp. Standing water
# collects in a low-frequency noise mask on level ground once the wetness is high. x = wetness, y = puddle.
WET_MASK_HLSL = """
float up = saturate((NormalZ - 0.7) / 0.25);
float wet = Wetness * lerp(0.35, 1.0, up);
float2 p = WorldPos.xy / 100.0;
float n = DgValueNoise(p / 3.0) * 0.65 + DgValueNoise(p / 0.9 + 7.1) * 0.35;
// Puddles fill the lowest noise values: about 15 % of level ground when Puddles is 1.
float puddle = saturate((Puddles * 0.32 - n) / 0.04) * up;
// The film isn't even: higher spots and coarse patches dry first.
float film = lerp(0.65, 1.0, DgValueNoise(p / 1.7 + 3.3));
return float2(wet * film, puddle);
"""
# Water fills the pores: albedo drops by about 40 %, puddles a little more.
WET_COLOR_HLSL = "return Color * lerp(1.0, 0.6, Wet.x) * lerp(1.0, 0.8, Wet.y);"
# A water film is glossy; never make an already glossier surface (window glass) rougher.
WET_ROUGHNESS_HLSL = "return min(Roughness, lerp(lerp(Roughness, 0.25, Wet.x), 0.03, Wet.y));"
WET_NORMAL_HLSL = "return normalize(lerp(Normal, float3(0, 0, 1), Wet.y));"
WEATHER_COLLECTION = "/Game/World/MPC_Weather"

# Terrain layers (vertex colour weights from Tools/osmimport/osmimport/landcover.py): name -> (texture set, tile m, tint)
TERRAIN_MASTER = f"{FOLDER}/M_TerrainMaster"
TERRAIN_LAYERS = {
    "Lawn": ("Generated/lawn", 2.0, (1.0, 1.0, 1.0)),  # generated close-up lawn (Tools/texturegen); falls back to a scan
    "Meadow": ("Generated/lawn", 3.0, (1.0, 0.92, 0.75)),  # was the dry brown grass_ground scan (issue 1)
    "Field": ("Ground/farm_soil", 3.0, (1.0, 1.0, 1.0)),
    "Forest": ("Generated/lawn_leaf_litter", 2.5, (1.0, 1.0, 1.0)),  # grass with fallen leaves; the old scan was solid orange litter
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

# Road defects (issue 71): cracks, sealed cracks and patches from the generated atlas, scattered by cell hash so they
# never repeat (Plugins/MapRuntime/Shaders/Private/RoadCracks.ush). Wear sets how many cells hold a defect.
ROAD_CRACKS_INCLUDE = "/Plugin/MapRuntime/Private/RoadCracks.ush"
ROAD_CRACKS_SET = "Generated/road_cracks"
ROAD_CRACKS_HLSL = """
FDgRoadDefects D = DgRoadDefects(CrackMask, CrackMaskSampler, CrackNormal, CrackNormalSampler, UV, Wear);
float crack = D.Masks.x;
float seal = D.Masks.y;
float patch = D.Masks.z;
// Cracks hold dirt and shadow; the sealant is black bitumen with a dull sheen.
// Patches weather to a lighter grey than the road (tone 0) or stay fresh and black (tone 1).
float3 c = Color * lerp(1.0, lerp(1.45, 0.7, D.Masks.w), patch) * (1.0 - crack * 0.8);
c = lerp(c, float3(0.03, 0.029, 0.028), seal * 0.85);
Roughness = lerp(lerp(Rough * lerp(1.0, 0.92, patch), 0.5, seal), 1.0, crack);
// A patch's fresher surface shows less of its aggregate.
NormalOut = normalize(float3(Nrm.xy * (1.0 - seal * 0.7) * (1.0 - patch * 0.35) + D.NormalXY, Nrm.z));
return c;
"""

mel = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


# Scan used when a generated set has not been imported (Scripts/import_generated_textures.py): generated set -> scan
GENERATED_FALLBACKS = {"Generated/lawn": "Ground/rocky_terrain_02", "Generated/lawn_leaf_litter": "Ground/forest_leaves_04"}


def tex(set_path, kind):
    name = set_path.split("/")[-1]
    if set_path in GENERATED_FALLBACKS and not eal.does_asset_exist(f"{TEX}/{set_path}/T_{name}_BaseColor"):
        set_path = GENERATED_FALLBACKS[set_path]
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


def custom_output(name, kind):
    co = unreal.CustomOutput()
    co.set_editor_property("output_name", name)
    co.set_editor_property("output_type", kind)
    return co


def scalar(material, name, default, x, y):
    return expr(material, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=default)


def tex_param(material, name, texture, sampler, uv, x, y):
    e = expr(material, unreal.MaterialExpressionTextureSampleParameter2D, x, y, parameter_name=name, texture=texture,
             sampler_type=sampler)
    link(uv, "", e, "UVs")
    return e


def parallax_uv(material, uv, default_height):
    """Returns the UVs the colour, normal, roughness and AO maps are sampled with: the plain tiled UVs, or with the
    Parallax static switch on, UVs displaced by the Height map (screen-space parallax occlusion mapping, no geometry).
    HeightRatio is the depth as a fraction of one tile; set per instance together with the Height texture."""
    height = expr(material, unreal.MaterialExpressionTextureObjectParameter, -1700, 700, parameter_name="Height",
                  texture=default_height, sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    call = expr(material, unreal.MaterialExpressionMaterialFunctionCall, -1450, 700)
    call.set_editor_property("material_function", unreal.load_asset(POM_FUNCTION))
    link(height, "", call, "Heightmap Texture")
    link(scalar(material, "HeightRatio", 0.02, -1700, 850), "", call, "Height Ratio")
    link(expr(material, unreal.MaterialExpressionConstant, -1700, 950, r=8), "", call, "Min Steps")
    link(expr(material, unreal.MaterialExpressionConstant, -1700, 1000, r=32), "", call, "Max Steps")
    link(expr(material, unreal.MaterialExpressionConstant4Vector, -1700, 1050, constant=unreal.LinearColor(1, 0, 0, 0)),
         "", call, "Heightmap Channel")
    link(uv, "", call, "UVs")
    switch = expr(material, unreal.MaterialExpressionStaticSwitchParameter, -1250, 700, parameter_name="Parallax",
                  default_value=False)
    link(call, "Parallax UVs", switch, "True")
    link(uv, "", switch, "False")
    return switch


def world_uv(m, tile_cm, x, y):
    """Texture coordinates from world x and y, one repeat per tile_cm."""
    world = expr(m, unreal.MaterialExpressionWorldPosition, x - 300, y)
    xy = expr(m, unreal.MaterialExpressionComponentMask, x - 150, y, r=True, g=True, b=False, a=False)
    link(world, "", xy, "")
    uv = expr(m, unreal.MaterialExpressionDivide, x, y, const_b=tile_cm)
    link(xy, "", uv, "A")
    return uv


def season_samples(m, set_path, tile_cm, x, y, parameter_prefix=None):
    """Colour, normal and roughness samples of a texture set at world UVs; with a prefix they are texture
    parameters (<prefix>BaseColor ...) that instances can swap."""
    uv = world_uv(m, tile_cm, x - 200, y)
    samples = []
    for kind, sampler, offset in (("BaseColor", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR, 0),
                                  ("Normal", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, 150),
                                  ("Roughness", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE, 300)):
        if parameter_prefix:
            node = tex_param(m, parameter_prefix + kind, tex(set_path, kind), sampler, uv, x, y + offset)
        else:
            node = expr(m, unreal.MaterialExpressionTextureSample, x, y + offset, texture=tex(set_path, kind),
                        sampler_type=sampler)
            link(uv, "", node, "UVs")
        samples.append(node)
    return samples


def snow_toned(m, samples, x, y):
    """Colour, normal and roughness samples of the snow scan after SNOW_TONE_HLSL: physical albedo, soft undulation and
    high roughness. Returns three nodes in the same order, each read with the pin season_samples' results use."""
    tone = expr(m, unreal.MaterialExpressionCustom, x, y, code=SNOW_TONE_HLSL, description="SnowTone",
                output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                inputs=[custom_input(n) for n in ("Scan", "ScanNormal", "ScanRough", "WorldPos", "Mean", "Target")])
    tone.set_editor_property("additional_outputs", [custom_output("NormalOut", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
                                                    custom_output("RoughOut", unreal.CustomMaterialOutputType.CMOT_FLOAT1)])
    tone.set_editor_property("include_file_paths", [TERRAIN_INCLUDE])
    link(samples[0], "RGB", tone, "Scan")
    link(samples[1], "RGB", tone, "ScanNormal")
    link(samples[2], "R", tone, "ScanRough")
    link(expr(m, unreal.MaterialExpressionWorldPosition, x - 200, y + 300), "", tone, "WorldPos")
    link(scalar(m, "SnowMean", SNOW_TONE[SNOW_SET][0], x - 200, y + 400), "", tone, "Mean")
    link(scalar(m, "SnowTarget", SNOW_TONE[SNOW_SET][1], x - 200, y + 500), "", tone, "Target")
    return [(tone, ""), (tone, "NormalOut"), (tone, "RoughOut")]


def add_season(m, color, rough, normal, snow_keep, leaf_keep):
    """Lays snow and fallen leaves over the surface by the season; colour, roughness and normal are (node, pin)
    pairs, and so are the results. Without the weather collection the inputs pass through unchanged."""
    collection = unreal.load_asset(WEATHER_COLLECTION) if eal.does_asset_exist(WEATHER_COLLECTION) else None
    if collection is None or tex(SNOW_SET, "BaseColor") is None:
        unreal.log_warning("create_materials: no weather collection or snow textures, no seasons")
        return color, rough, normal
    mask = expr(m, unreal.MaterialExpressionCustom, 400, 1300, code=SEASON_MASK_HLSL,
                output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT2, description="SeasonMask",
                inputs=[custom_input(n) for n in ("Snow", "Leaves", "SnowKeep", "LeafKeep", "WorldPos", "NormalZ")])
    mask.set_editor_property("include_file_paths", [TERRAIN_INCLUDE])
    link(expr(m, unreal.MaterialExpressionCollectionParameter, 100, 1300, collection=collection,
              parameter_name="SnowCover"), "", mask, "Snow")
    link(expr(m, unreal.MaterialExpressionCollectionParameter, 100, 1400, collection=collection,
              parameter_name="FallenLeaves"), "", mask, "Leaves")
    link(scalar(m, "SnowKeep", snow_keep, 100, 1500), "", mask, "SnowKeep")
    link(scalar(m, "LeafKeep", leaf_keep, 100, 1600), "", mask, "LeafKeep")
    link(expr(m, unreal.MaterialExpressionWorldPosition, 100, 1700), "", mask, "WorldPos")
    vertex_normal = expr(m, unreal.MaterialExpressionVertexNormalWS, 0, 1800)
    normal_z = expr(m, unreal.MaterialExpressionComponentMask, 150, 1800, r=False, g=False, b=True, a=False)
    link(vertex_normal, "", normal_z, "")
    link(normal_z, "", mask, "NormalZ")
    snow_mask = expr(m, unreal.MaterialExpressionComponentMask, 600, 1300, r=True, g=False, b=False, a=False)
    leaf_mask = expr(m, unreal.MaterialExpressionComponentMask, 600, 1400, r=False, g=True, b=False, a=False)
    link(mask, "", snow_mask, "")
    link(mask, "", leaf_mask, "")
    snow = snow_toned(m, season_samples(m, SNOW_SET, 250.0, 400, 1900, parameter_prefix="Snow"), 600, 1900)
    leaves = season_samples(m, LEAF_SET, 150.0, 400, 2400)
    pins = ("RGB", "RGB", "R")
    results = []
    for index, (source, pin) in enumerate((color, normal, rough)):
        with_leaves = expr(m, unreal.MaterialExpressionLinearInterpolate, 800, 1300 + index * 200)
        link(source, pin, with_leaves, "A")
        link(leaves[index], pins[index], with_leaves, "B")
        link(leaf_mask, "", with_leaves, "Alpha")
        with_snow = expr(m, unreal.MaterialExpressionLinearInterpolate, 950, 1300 + index * 200)
        link(with_leaves, "", with_snow, "A")
        link(snow[index][0], snow[index][1], with_snow, "B")
        link(snow_mask, "", with_snow, "Alpha")
        results.append((with_snow, ""))
    return results[0], results[2], results[1]


def add_wetness(m, color, rough, normal):
    """Darkens and glosses the surface with the weather's wetness and adds puddles; returns new colour, roughness,
    normal. Without the weather collection (not created yet) the inputs pass through unchanged."""
    collection = unreal.load_asset(WEATHER_COLLECTION) if eal.does_asset_exist(WEATHER_COLLECTION) else None
    if collection is None:
        unreal.log_warning(f"create_materials: {WEATHER_COLLECTION} missing, no wet surfaces")
        return color, rough, normal
    wetness = expr(m, unreal.MaterialExpressionCollectionParameter, 100, 600, collection=collection,
                   parameter_name="Wetness")
    puddles = expr(m, unreal.MaterialExpressionCollectionParameter, 100, 700, collection=collection,
                   parameter_name="Puddles")
    world = expr(m, unreal.MaterialExpressionWorldPosition, 100, 800)
    vertex_normal = expr(m, unreal.MaterialExpressionVertexNormalWS, 100, 900)
    normal_z = expr(m, unreal.MaterialExpressionComponentMask, 250, 900, r=False, g=False, b=True, a=False)
    link(vertex_normal, "", normal_z, "")
    mask = expr(m, unreal.MaterialExpressionCustom, 400, 700, code=WET_MASK_HLSL,
                output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT2, description="WetMask",
                inputs=[custom_input("Wetness"), custom_input("Puddles"), custom_input("WorldPos"),
                        custom_input("NormalZ")])
    mask.set_editor_property("include_file_paths", [TERRAIN_INCLUDE])
    link(wetness, "", mask, "Wetness")
    link(puddles, "", mask, "Puddles")
    link(world, "", mask, "WorldPos")
    link(normal_z, "", mask, "NormalZ")

    def stage(code, input_name, source, output_type, y):
        node = expr(m, unreal.MaterialExpressionCustom, 650, y, code=code, output_type=output_type,
                    inputs=[custom_input(input_name), custom_input("Wet")])
        link(source, "", node, input_name)
        link(mask, "", node, "Wet")
        return node

    wet_color = stage(WET_COLOR_HLSL, "Color", color, unreal.CustomMaterialOutputType.CMOT_FLOAT3, -400)
    wet_rough = stage(WET_ROUGHNESS_HLSL, "Roughness", rough, unreal.CustomMaterialOutputType.CMOT_FLOAT1, 200)
    wet_normal = stage(WET_NORMAL_HLSL, "Normal", normal, unreal.CustomMaterialOutputType.CMOT_FLOAT3, -100)
    return wet_color, wet_rough, wet_normal


def add_lit_windows(m, texcoord, vertex_color, glass_mask):
    """Emissive glow in a share of the facade windows at night (instances with the Windows switch only)."""
    collection = unreal.load_asset(WEATHER_COLLECTION) if eal.does_asset_exist(WEATHER_COLLECTION) else None
    if collection is None:
        return
    night = expr(m, unreal.MaterialExpressionCollectionParameter, -1000, 1900, collection=collection,
                 parameter_name="Night")
    lit = expr(m, unreal.MaterialExpressionCustom, -800, 1900, code=LIT_WINDOW_HLSL,
               output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, description="LitWindows",
               inputs=[custom_input("UV"), custom_input("Variation"), custom_input("FloorHeight"),
                       custom_input("Night")])
    link(texcoord, "", lit, "UV")
    link(vertex_color, "G", lit, "Variation")
    link(scalar(m, "FloorHeight", 3.0, -1000, 2000), "", lit, "FloorHeight")
    link(night, "", lit, "Night")
    glowing = expr(m, unreal.MaterialExpressionMultiply, -550, 1900)
    link(lit, "", glowing, "A")
    link(glass_mask, "", glowing, "B")
    black = expr(m, unreal.MaterialExpressionConstant3Vector, -550, 2050, constant=unreal.LinearColor(0, 0, 0, 1))
    switch = expr(m, unreal.MaterialExpressionStaticSwitchParameter, -300, 1900, parameter_name="Windows",
                  default_value=False)
    link(glowing, "", switch, "True")
    link(black, "", switch, "False")
    mel.connect_material_property(switch, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)


def add_road_cracks(m, texcoord, color, rough, normal):
    """Behind the Cracks static switch (road instances only): lays the generated crack atlas over the surface.
    Returns colour, roughness and normal nodes; without the atlas imported they pass through unchanged."""
    path = f"{TEX}/{ROAD_CRACKS_SET}/T_RoadCracks_"
    if not eal.does_asset_exist(path + "Mask"):
        unreal.log_warning("create_materials: road crack atlas not imported (Scripts/import_road_cracks.py)")
        return color, rough, normal
    custom = expr(m, unreal.MaterialExpressionCustom, -1000, -800, description="RoadCracks",
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, code=ROAD_CRACKS_HLSL,
                  include_file_paths=[TERRAIN_INCLUDE, ROAD_CRACKS_INCLUDE],
                  inputs=[custom_input(n) for n in ("UV", "Wear", "Color", "Rough", "Nrm", "CrackMask", "CrackNormal")])
    outputs = []
    for name, kind in (("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
                       ("NormalOut", unreal.CustomMaterialOutputType.CMOT_FLOAT3)):
        output = unreal.CustomOutput()
        output.set_editor_property("output_name", name)
        output.set_editor_property("output_type", kind)
        outputs.append(output)
    custom.set_editor_property("additional_outputs", outputs)
    link(texcoord, "", custom, "UV")
    link(scalar(m, "Wear", 0.5, -1300, -700), "", custom, "Wear")
    link(color, "", custom, "Color")
    link(rough, "", custom, "Rough")
    link(normal, "RGB", custom, "Nrm")
    for name, sampler, y in (("Mask", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR, -650),
                             ("Normal", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, -600)):
        texture = expr(m, unreal.MaterialExpressionTextureObjectParameter, -1300, y, parameter_name=f"Crack{name}",
                       texture=unreal.load_asset(path + name), sampler_type=sampler)
        link(texture, "", custom, f"Crack{name}")
    results = []
    for pin, source, source_pin, y in (("", color, "", -900), ("Roughness", rough, "", -850), ("NormalOut", normal, "RGB", -800)):
        switch = expr(m, unreal.MaterialExpressionStaticSwitchParameter, -800, y, parameter_name="Cracks", default_value=False)
        link(custom, pin, switch, "True")
        link(source, source_pin, switch, "False")
        results.append(switch)
    return results[0], results[1], results[2]


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
    uv = parallax_uv(m, uv, tex(default_set, "Height"))
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
    tinted, rough_scaled, normal = add_road_cracks(m, texcoord, tinted, rough_scaled, normal)

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
    link(normal, "", normal_windows, "A")
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
    (out_color, _), (out_rough, _), (out_normal, _) = add_season(m, (out_color, ""), (out_rough, ""),
                                                                 (out_normal, ""), snow_keep=1.0, leaf_keep=0.0)
    out_color, out_rough, out_normal = add_wetness(m, out_color, out_rough, out_normal)
    add_lit_windows(m, texcoord, vcol, glass_mask)
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
    for kind in ("BaseColor", "Normal", "Roughness", "AO", "Height"):
        t = tex(set_path, kind)
        if t:
            mel.set_material_instance_texture_parameter_value(mi, kind, t)
        elif kind == "AO":
            mel.set_material_instance_texture_parameter_value(
                mi, kind, unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"))
    mel.set_material_instance_scalar_parameter_value(mi, "TileSize", tile_size)
    if "roughness_scale" in opts:
        mel.set_material_instance_scalar_parameter_value(mi, "RoughnessScale", opts["roughness_scale"])
    if "tint" in opts:
        lo, hi = opts["tint"]
        mel.set_material_instance_scalar_parameter_value(mi, "TintMin", lo)
        mel.set_material_instance_scalar_parameter_value(mi, "TintMax", hi)
    for prefix, snow_set in SEASON_SNOW_SETS.items():
        if section.startswith(prefix):
            for kind in ("BaseColor", "Normal", "Roughness"):
                mel.set_material_instance_texture_parameter_value(mi, "Snow" + kind, tex(snow_set, kind))
            mean, target = SNOW_TONE[snow_set]
            mel.set_material_instance_scalar_parameter_value(mi, "SnowMean", mean)
            mel.set_material_instance_scalar_parameter_value(mi, "SnowTarget", target)
    for prefix, (snow_keep, leaf_keep) in SEASON_KEEP.items():
        if section.startswith(prefix):
            mel.set_material_instance_scalar_parameter_value(mi, "SnowKeep", snow_keep)
            mel.set_material_instance_scalar_parameter_value(mi, "LeafKeep", leaf_keep)
    if "parallax" in opts:
        mel.set_material_instance_static_switch_parameter_value(mi, "Parallax", True)
        mel.set_material_instance_scalar_parameter_value(mi, "HeightRatio", opts["parallax"])
    else:
        mel.set_material_instance_static_switch_parameter_value(mi, "Parallax", False)
    if "cracks" in opts:
        mel.set_material_instance_static_switch_parameter_value(mi, "Cracks", True)
        mel.set_material_instance_scalar_parameter_value(mi, "Wear", opts["cracks"])
    else:
        mel.set_material_instance_static_switch_parameter_value(mi, "Cracks", False)
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
    color, rough, normal = add_season(m, (custom, ""), (custom, "Roughness"), (custom, "Normal"),
                                      snow_keep=1.0, leaf_keep=1.0)
    mel.connect_material_property(color[0], color[1], unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(normal[0], normal[1], unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(rough[0], rough[1], unreal.MaterialProperty.MP_ROUGHNESS)
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
    # Woods on the horizon as extruded blocks (build_horizon_world.py): kilometres away a canopy is a dark green mass.
    "Canopy_Far": ((0.20, 0.26, 0.12), 0.95, 0.0),
}


# Worn road paint, one profile for every marking material (zebra bars, lane, edge and stop lines): the masks come from
# Plugins/MapRuntime/Shaders/Private/MarkingWear.ush. Dirt greys the white, chips show the asphalt (more at the ragged
# edges), tyre polish darkens and smooths the wheel tracks, and a noise bump roughens the normal.
MARKING_WEAR_INCLUDE = "/Plugin/MapRuntime/Private/MarkingWear.ush"
MARKING_WEAR_PREAMBLE = """
float2 Tilt;
float3 Wear = DgMarkingWearMasks(WorldPos.xy, LineUV, LineColor.rg, Tilt);
"""
MARKING_WEAR_COLOR_HLSL = MARKING_WEAR_PREAMBLE + """
float3 paint = lerp(Base, float3(0.32, 0.30, 0.27), Wear.x);
paint = lerp(paint, float3(0.22, 0.22, 0.23), Wear.z * 0.55);
return lerp(paint, float3(0.06, 0.06, 0.065), Wear.y * 0.9);
"""
MARKING_WEAR_ROUGHNESS_HLSL = MARKING_WEAR_PREAMBLE + """
float rough = lerp(0.62, 0.88, saturate(Wear.x * 1.2));
rough = lerp(rough, 0.45, Wear.z * 0.6);
return lerp(rough, 0.92, Wear.y);
"""
MARKING_WEAR_NORMAL_HLSL = MARKING_WEAR_PREAMBLE + """
return normalize(float3(Tilt * 0.6, 1.0));
"""


def add_marking_wear(m, base):
    """Colour, roughness and normal expressions of worn road paint with the given base colour expression."""
    world = expr(m, unreal.MaterialExpressionWorldPosition, -900, 0)
    texcoord = expr(m, unreal.MaterialExpressionTextureCoordinate, -900, 150)
    vertex_color = expr(m, unreal.MaterialExpressionVertexColor, -900, 300)
    results = []
    for code, output, y in ((MARKING_WEAR_COLOR_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, -100),
                            (MARKING_WEAR_ROUGHNESS_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT1, 250),
                            (MARKING_WEAR_NORMAL_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, 450)):
        node = expr(m, unreal.MaterialExpressionCustom, -400, y, code=code, output_type=output, description="MarkingWear",
                    inputs=[custom_input("WorldPos"), custom_input("LineUV"), custom_input("LineColor"),
                            custom_input("Base")])
        node.set_editor_property("include_file_paths", [TERRAIN_INCLUDE, MARKING_WEAR_INCLUDE])
        link(world, "", node, "WorldPos")
        link(texcoord, "", node, "LineUV")
        link(vertex_color, "", node, "LineColor")
        link(base, "", node, "Base")
        results.append(node)
    return results


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
    if section.startswith("Marking_"):
        worn_color, worn_rough, worn_normal = add_marking_wear(m, base)
        wet_color, wet_rough, wet_normal = add_wetness(m, worn_color, worn_rough, worn_normal)
        mel.connect_material_property(wet_color, "", unreal.MaterialProperty.MP_BASE_COLOR)
        mel.connect_material_property(wet_rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
        mel.connect_material_property(wet_normal, "", unreal.MaterialProperty.MP_NORMAL)
    else:
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
