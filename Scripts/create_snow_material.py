"""Creates M_Snow_Layer, the material of the snow layer meshes (Plugins/MapRuntime WorldSnow.cpp), in /Game/World/Materials.

  Scripts/lock.sh gpu $UE/Engine/Binaries/Linux/UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript \
      -script=Scripts/create_snow_material.py -unattended -nosplash

The snow meshes store the snow surface at full depth. This material lowers the vertices to the ground by
(1 - SnowCover) times the depth in the vertex colour (R, 0.5 cm steps), so the layer grows and melts without a
rebuild. G is the road weight, B the footway weight, A the distance from the nearest road or footway edge (2 cm
steps). From these the pixel shader draws wheel tracks and the ploughed ridge on roads, a trodden strip on footways,
fine surface grain, sparkle in clear sun, and patchy melting at low cover. Snow is bright but never above what the
locked exposure can show: albedo 0.86 (as the ground's snow), with a faint cool subsurface colour for the soft light inside the snow.
"""
import unreal

FOLDER = "/Game/World/Materials"
NAME = "M_Snow_Layer"
TEX = "/Game/Textures"
WEATHER_COLLECTION = "/Game/World/MPC_Weather"
TRACK_MAP = "/Game/World/SnowTracks/RT_SnowTracks"  # Scripts/create_snow_track_map.py
TERRAIN_INCLUDE = "/Plugin/MapRuntime/Private/TerrainNoise.ush"

mel = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary

# Vertex shader: from the full-depth surface down to the ground. Depth in metres = R * 255 * 0.005, offset in cm.
WPO_HLSL = """
float depth_cm = VertexDepth * 127.5;
float lowering_cm = (1.0 - saturate(Cover)) * depth_cm;
// Towards the draw distance of the layer (180 m, WorldTileActor.cpp) it sinks onto the ground, which has its own snow, so it fades out without a pop.
float distance_cm = length(WorldPos - CameraPos);
float sink = smoothstep(13000.0, 17500.0, distance_cm);
lowering_cm = lerp(lowering_cm, max(depth_cm - 0.5, 0.0), sink);
return float3(0, 0, -lowering_cm);
"""

# Pixel shader. Heights below are in cm, positions in cm; p is in metres.
SNOW_HLSL = """
// Size of a pixel on the surface, cm: from the depth, not from screen-space derivatives, which are wrong along triangle edges.
float pixel_cm = PixelDepth * 0.001;
// Tyre tracks from the deformation map (SnowTrackSubsystem; R compaction, G ridge, BA direction of travel). The 1 m mesh
// cannot hold a 20 cm rut, so the rut is a height field in the pixel shader: a parallax ray march finds where the view ray
// meets the rut floor or wall (so the walls hold up at grazing angles and in stereo), and the colour, tread and normal
// are taken there. The map is bilinear 8 bit; its 0.5 iso-line is the rut edge, which stays crisp at pixel resolution.
float map_fade = 1.0 - smoothstep(3000.0, 4200.0, length(WorldPos.xy - CameraPos.xy));
float depth_cm = Depth * 100.0;
float rut_scale = map_fade * saturate(depth_cm / 1.5) * saturate(Cover * 2.0);
float rut_depth_cm = 0.7 * depth_cm;
#define RUT_UV(q_) frac((q_) / 10240.0)
#define RUT_MASK(q_) smoothstep(0.46, 0.54, Texture2DSampleLevel(TrackMap, TrackMapSampler, RUT_UV(q_), 0).r)

// Scan textures are sampled at the unshifted position: the ray march moves the position discontinuously, which would wreck their mip selection.
float2 surface_p = WorldPos.xy / 100.0;
float2 hit_xy = WorldPos.xy;
float hit_drop = 0.0;
if (rut_scale > 0.01 && rut_depth_cm > 0.3 && PixelDepth < 4000.0)
{
    float3 to_eye = normalize(CameraVector);
    float2 shift_per_cm = -to_eye.xy / max(to_eye.z, 0.12);
    float step_drop = rut_depth_cm / 24.0;
    float previous_gap = 0.0;
    float previous_drop = 0.0;
    float2 previous_xy = WorldPos.xy;
    bool found = false;
    [loop] for (int i = 0; i <= 24; i++)
    {
        float drop = i * step_drop;
        float2 q = WorldPos.xy + shift_per_cm * drop;
        // Gap between the floor of the rut and the ray: positive while the ray is still in the air above it.
        float gap = RUT_MASK(q) * rut_scale * rut_depth_cm - drop;
        if (gap <= 0.0)
        {
            float low_drop = previous_drop;
            float high_drop = drop;
            // Bisection on the crossing between the last two samples sharpens the wall edge beyond the step size.
            [unroll] for (int j = 0; j < 4; j++)
            {
                float mid_drop = 0.5 * (low_drop + high_drop);
                float mid_gap = RUT_MASK(WorldPos.xy + shift_per_cm * mid_drop) * rut_scale * rut_depth_cm - mid_drop;
                if (mid_gap > 0.0) { low_drop = mid_drop; } else { high_drop = mid_drop; }
            }
            hit_drop = 0.5 * (low_drop + high_drop);
            hit_xy = WorldPos.xy + shift_per_cm * hit_drop;
            found = true;
            break;
        }
        previous_gap = gap;
        previous_drop = drop;
        previous_xy = q;
    }
    if (!found)
    {
        hit_drop = rut_depth_cm;
        hit_xy = WorldPos.xy + shift_per_cm * rut_depth_cm;
    }
}
// Grain and ripples stay at the surface position: at the hit point a wall would smear them into streaks.
float2 p = surface_p;
float compaction = RUT_MASK(hit_xy) * rut_scale;
float4 rut_sample = Texture2DSampleLevel(TrackMap, TrackMapSampler, RUT_UV(hit_xy), 0);
float2 travel = normalize((rut_sample.ba - 0.5) * 2.0 + float2(1e-4, 0.0));
float2 across = float2(-travel.y, travel.x);
// Tread: a regular print of transverse blocks, 4.5 cm pitch, with a groove down the middle of the tyre. It is drawn only where
// the stored direction of travel is coherent (bilinear blending of directions shortens the vector where it changes fast) and
// only within a few metres, because beyond that the 4.5 cm pitch would alias into waves.
float direction_length = length((rut_sample.ba - 0.5) * 2.0);
float tread_on = smoothstep(0.9, 0.98, direction_length) * (1.0 - smoothstep(250.0, 450.0, PixelDepth));
float fade_tread = saturate(1.0 - pixel_cm / 1.4) * tread_on;
#define TREAD_H(q_) (0.5 * fade_tread * smoothstep(0.1, 0.2, frac(dot((q_) / 100.0, travel) / 0.045)) * (1.0 - smoothstep(0.55, 0.65, frac(dot((q_) / 100.0, travel) / 0.045))) * smoothstep(0.02, 0.05, abs(frac(dot((q_) / 100.0, across) / 0.2) - 0.5)))
#define RUT_H(q_) (-RUT_MASK(q_) * rut_scale * rut_depth_cm + Texture2DSampleLevel(TrackMap, TrackMapSampler, RUT_UV(q_), 0).g * rut_scale * 1.8 + RUT_MASK(q_) * rut_scale * TREAD_H(q_))
float rut_h = RUT_H(hit_xy);
float2 rut_slope = float2(RUT_H(hit_xy + float2(3.0, 0)) - rut_h, RUT_H(hit_xy + float2(0, 3.0)) - rut_h) / 3.0;
// A wall is steep but never vertical; capping the slope keeps single texel steps from turning into dark spikes.
rut_slope *= min(1.0, 3.0 / max(length(rut_slope), 1e-4));
float rut_dx = rut_slope.x;
float rut_dy = rut_slope.y;

float road = VertexRoad;
float footway = VertexFootway;
float edge = VertexEdge * 5.1;

// Wheel tracks: two per lane, at 0.8 m and 2.4 m from the road edge (lanes 3.2 m wide), and the ploughed ridge.
float t1 = (edge - 0.8) / 0.3;
float t2 = (edge - 2.4) / 0.3;
float tracks = max(exp(-t1 * t1), exp(-t2 * t2)) * road;
tracks *= 0.4 * saturate(0.1 + 1.3 * DgValueNoise(p / 3.0 + 8.0));
float ridge_x = (edge - 0.45) / 0.35;
float ridge = exp(-ridge_x * ridge_x) * road;

// Trodden strip in the middle of footways, patchy.
float trodden = footway * saturate((edge - 0.25) / 0.3) * saturate(0.35 + 1.1 * DgValueNoise(p / 2.5 + 3.0));

// Surface grain and drift ripples as a height in cm, a function of position only so the normal can come from
// finite differences (screen-space derivatives showed seams along triangle edges). Fine octaves fade out before they alias.
float fade_fine = saturate(1.0 - pixel_cm / 1.2);
float fade_grain = saturate(1.0 - pixel_cm / 0.5);
float fade_ripple = saturate(1.0 - pixel_cm / 6.0);
#define SNOW_H(q) (0.35 * DgValueNoise((q) * 14.0) * fade_fine + 0.18 * DgValueNoise((q) * 38.0) * fade_grain + 0.5 * DgValueNoise(float2((q).x * 1.5 + (q).y * 0.5, (q).y * 4.0)) * fade_ripple + 3.0 * DgValueNoise((q) / 1.7))
float h = SNOW_H(p);
float step_m = 0.02;
float dhx = (SNOW_H(p + float2(step_m, 0)) - h) / (step_m * 100.0);
float dhy = (SNOW_H(p + float2(0, step_m)) - h) / (step_m * 100.0);
dhx += rut_dx;
dhy += rut_dy;

// Colour. Fresh snow is bright and slightly cool; trodden and road snow is greyer and dirtier.
float macro = DgValueNoise(p / 5.0 + 11.0);
// Same albedo as the ground's snow (SNOW_TONE in create_materials.py) so the layer fades into it unseen.
float3 fresh = float3(0.86, 0.855, 0.865) * float3(0.975, 0.99, 1.0) * lerp(0.93, 1.03, macro);
float3 trodden_scan = Texture2DSample(Snow1Color, Snow1ColorSampler, surface_p / 2.5).rgb;
float3 road_scan = Texture2DSample(AsphaltSnowColor, AsphaltSnowColorSampler, surface_p / 2.5).rgb;
float3 trodden_color = fresh * (0.62 + 0.45 * dot(trodden_scan, 0.333));
float3 road_snow = lerp(fresh * 0.86, trodden_color * 0.8, 0.5);
float3 wet_road = road_scan * 1.3;
float road_cover = saturate(0.55 + 0.7 * ridge - 0.9 * tracks);
float3 road_color = lerp(wet_road, road_snow, road_cover);
float3 color = lerp(fresh, trodden_color, trodden);
color = lerp(color, road_color, road);
// Packed snow is denser: greyer and darker, and on thin road snow the wet asphalt shows through.
float3 packed = color * float3(0.93, 0.95, 0.98);
color = lerp(color, packed, compaction * (0.5 + 0.5 * road));
color *= lerp(1.0, 0.88, saturate(hit_drop / 6.0) * compaction);
float reveal = compaction * road * (1.0 - saturate((depth_cm - 2.0) / 4.0));
color = lerp(color, wet_road * 0.8, reveal * 0.5);

// Rough and soft: snow is a diffuse scatterer; tracks are compacted and a little glossier.
float rough = lerp(0.95, 0.7, saturate(tracks + trodden * 0.5));

// World-space normal tilted by the height gradient (x east, y south).
Normal = normalize(N + float3(-dhx, -dhy, 0) * 1.0);
Roughness = lerp(rough, 0.6, compaction);

// Patchy melting at low cover: thin, trodden and road snow goes first.
float patch = DgValueNoise(p / 1.3 + 5.0) * 0.6 + DgValueNoise(p / 0.35) * 0.4;
float thin = 1.0 - saturate(Depth * 8.0);
float threshold = patch * 0.55 + thin * 0.2 + road * 0.15;
Mask = saturate((Cover * 1.35 - threshold) / 0.08);

// Sparkle: a few facets mirror the sun toward the eye, only in clear weather and close up.
float3 cell = floor(WorldPos / 0.45);
float facet = DgHash(cell.xy + cell.z * 17.0);
float phase = frac(facet * 13.7 + dot(CameraVector, float3(7.0, 5.0, 3.0)));
float twinkle = pow(saturate(1.0 - abs(phase - 0.5) * 7.0), 3.0);
float near_fade = saturate(1.0 - PixelDepth / 1500.0) * saturate(PixelDepth / 60.0);
float sun_visible = (1.0 - Night) * lerp(1.0, 0.1, saturate(Clouds));
Glint = float3(1, 1, 1) * step(0.965, facet) * twinkle * near_fade * sun_visible * 2500.0 * (1.0 - road * 0.7);
return color;
"""


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


def texture_object(m, name, set_path, y):
    base = set_path.split("/")[-1]
    path = f"{TEX}/{set_path}/T_{base}_BaseColor"
    return expr(m, unreal.MaterialExpressionTextureObjectParameter, -1200, y, parameter_name=name,
                texture=unreal.load_asset(path), sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)


def build():
    path = f"{FOLDER}/{NAME}"
    if eal.does_asset_exist(path):
        m = unreal.load_asset(path)
        mel.delete_all_material_expressions(m)
    else:
        m = asset_tools.create_asset(NAME, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_SUBSURFACE)
    m.set_editor_property("tangent_space_normal", False)
    collection = unreal.load_asset(WEATHER_COLLECTION)

    def mpc(name, y):
        return expr(m, unreal.MaterialExpressionCollectionParameter, -1200, y, collection=collection, parameter_name=name)

    cover = mpc("SnowCover", -400)
    night = mpc("Night", -300)
    clouds = mpc("CloudCover", -200)
    vertex_color = expr(m, unreal.MaterialExpressionVertexColor, -1200, -600)

    wpo = expr(m, unreal.MaterialExpressionCustom, -600, -600, code=WPO_HLSL, description="SnowDepth",
               output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
               inputs=[custom_input("VertexDepth"), custom_input("Cover"), custom_input("WorldPos"), custom_input("CameraPos")])
    link(vertex_color, "R", wpo, "VertexDepth")
    link(cover, "", wpo, "Cover")
    link(expr(m, unreal.MaterialExpressionWorldPosition, -1200, -800), "", wpo, "WorldPos")
    link(expr(m, unreal.MaterialExpressionCameraPositionWS, -1200, -900), "", wpo, "CameraPos")
    mel.connect_material_property(wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    pixel = expr(m, unreal.MaterialExpressionCustom, -600, 0, code=SNOW_HLSL, description="SnowSurface",
                 output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                 inputs=[custom_input(n) for n in ("VertexRoad", "VertexFootway", "VertexEdge", "WorldPos", "N", "Cover", "Night", "Clouds", "CameraVector",
                                                   "PixelDepth", "Depth", "CameraPos", "Snow1Color", "AsphaltSnowColor", "TrackMap")])
    pixel.set_editor_property("additional_outputs", [
        custom_output("Normal", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
        custom_output("Roughness", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
        custom_output("Mask", unreal.CustomMaterialOutputType.CMOT_FLOAT1),
        custom_output("Glint", unreal.CustomMaterialOutputType.CMOT_FLOAT3),
    ])
    pixel.set_editor_property("include_file_paths", [TERRAIN_INCLUDE])
    link(vertex_color, "G", pixel, "VertexRoad")
    link(vertex_color, "B", pixel, "VertexFootway")
    link(vertex_color, "A", pixel, "VertexEdge")
    link(expr(m, unreal.MaterialExpressionWorldPosition, -1200, -100), "", pixel, "WorldPos")
    link(expr(m, unreal.MaterialExpressionVertexNormalWS, -1200, 0), "", pixel, "N")
    link(cover, "", pixel, "Cover")
    link(night, "", pixel, "Night")
    link(clouds, "", pixel, "Clouds")
    link(expr(m, unreal.MaterialExpressionCameraVectorWS, -1200, 100), "", pixel, "CameraVector")
    link(expr(m, unreal.MaterialExpressionPixelDepth, -1200, 200), "", pixel, "PixelDepth")
    depth_metres = expr(m, unreal.MaterialExpressionMultiply, -800, -600, const_b=1.275)
    link(vertex_color, "R", depth_metres, "A")
    link(depth_metres, "", pixel, "Depth")
    link(expr(m, unreal.MaterialExpressionCameraPositionWS, -1200, 250), "", pixel, "CameraPos")
    link(expr(m, unreal.MaterialExpressionTextureObjectParameter, -1200, 600, parameter_name="TrackMap",
              texture=unreal.load_asset(TRACK_MAP), sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR),
         "", pixel, "TrackMap")
    link(texture_object(m, "TroddenSnow", "Snow/snow_01", 300), "", pixel, "Snow1Color")
    link(texture_object(m, "RoadSnow", "Snow/asphalt_snow", 450), "", pixel, "AsphaltSnowColor")

    mel.connect_material_property(pixel, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(pixel, "Normal", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(pixel, "Roughness", unreal.MaterialProperty.MP_ROUGHNESS)
    mel.connect_material_property(pixel, "Mask", unreal.MaterialProperty.MP_OPACITY_MASK)
    mel.connect_material_property(pixel, "Glint", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.connect_material_property(expr(m, unreal.MaterialExpressionConstant3Vector, -300, 600,
                                       constant=unreal.LinearColor(0.75, 0.88, 1.0, 1)), "",
                                  unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    mel.connect_material_property(expr(m, unreal.MaterialExpressionConstant, -300, 800, r=0.25), "",
                                  unreal.MaterialProperty.MP_SPECULAR)
    mel.connect_material_property(expr(m, unreal.MaterialExpressionConstant, -300, 700, r=0.35), "",
                                  unreal.MaterialProperty.MP_OPACITY)
    mel.recompile_material(m)
    eal.save_loaded_asset(m)
    unreal.log_warning(f"create_snow_material: {NAME} done")


build()
