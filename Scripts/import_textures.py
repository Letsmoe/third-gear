"""Imports the downloaded photo-scanned PBR texture sets from RawAssets/ into /Game/Textures/<Category>/<SetName>/
as T_<SetName>_<Map> with correct compression / sRGB / LOD-group settings. No materials are created.

Run headless (editor must be closed):
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script="<this file> [-only=set1,set2]" -unattended -nosplash

Re-running re-imports (replace_existing) and re-applies the settings.
"""
import glob
import os
import unreal

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
RAW_DIR = os.path.join(PROJECT_DIR, "RawAssets")
DEST_ROOT = "/Game/Textures"

# (category, source, set name). Kept as a pure literal list: Tools/asset_fetch/contact_sheet.py parses it.
SETS = [
    # Asphalt
    ("Asphalt", "polyhaven", "asphalt_01"),
    ("Asphalt", "polyhaven", "asphalt_02"),
    ("Asphalt", "polyhaven", "asphalt_04"),
    ("Asphalt", "polyhaven", "asphalt_pit_lane"),
    ("Asphalt", "polyhaven", "asphalt_track"),
    ("Asphalt", "polyhaven", "aerial_asphalt_01"),
    ("Asphalt", "ambientcg", "Asphalt015"),
    ("Asphalt", "ambientcg", "Asphalt031"),
    # Cobblestone / setts
    ("Cobble", "polyhaven", "cobblestone_floor_02"),
    ("Cobble", "polyhaven", "cobblestone_floor_08"),
    ("Cobble", "polyhaven", "cobblestone_floor_09"),
    ("Cobble", "polyhaven", "cobblestone_03"),
    ("Cobble", "ambientcg", "PavingStones070"),
    ("Cobble", "ambientcg", "PavingStones115A"),
    # Pavers / sidewalk slabs / granite
    ("Pavers", "polyhaven", "brick_pavement_03"),
    ("Pavers", "polyhaven", "concrete_pavement"),
    ("Pavers", "polyhaven", "concrete_pavement_02"),
    ("Pavers", "polyhaven", "concrete_pavers"),
    ("Pavers", "polyhaven", "concrete_pavers_02"),
    ("Pavers", "polyhaven", "large_square_pattern_01"),
    ("Pavers", "polyhaven", "brick_floor_003"),
    ("Pavers", "polyhaven", "brick_pavement"),
    ("Pavers", "polyhaven", "granite_tile_04"),
    ("Pavers", "ambientcg", "PavingStones092"),
    # Ground / grass / gravel / dirt / forest floor
    ("Ground", "polyhaven", "dirt_floor"),
    ("Ground", "polyhaven", "dry_mud_field_001"),
    ("Ground", "polyhaven", "farm_soil"),
    ("Ground", "polyhaven", "forest_ground_05"),
    ("Ground", "polyhaven", "forest_leaves_04"),
    ("Ground", "polyhaven", "forrest_ground_01"),
    ("Ground", "polyhaven", "grass_ground"),
    ("Ground", "polyhaven", "leafy_grass"),
    ("Ground", "polyhaven", "gravel_floor_02"),
    ("Ground", "polyhaven", "gravel_ground_01"),
    ("Ground", "polyhaven", "gravel_road"),
    ("Ground", "ambientcg", "Ground032"),
    ("Ground", "ambientcg", "Ground037"),
    ("Ground", "ambientcg", "Ground038"),
    ("Ground", "ambientcg", "Ground048"),
    ("Ground", "ambientcg", "Ground066"),
    ("Ground", "polyhaven", "rocky_terrain_02"),
    ("Ground", "polyhaven", "aerial_grass_rock"),
    ("Ground", "polyhaven", "brown_mud_leaves_01"),
    ("Ground", "polyhaven", "grass_path_2"),
    ("Ground", "polyhaven", "farm_furrows"),
    # Brick (Klinker)
    ("Brick", "polyhaven", "brick_4"),
    ("Brick", "polyhaven", "brick_wall_001"),
    ("Brick", "polyhaven", "brick_wall_006"),
    ("Brick", "polyhaven", "brick_wall_09"),
    ("Brick", "polyhaven", "brick_wall_10"),
    ("Brick", "polyhaven", "exterior_wall_cladding_02"),
    # Plaster / render
    ("Plaster", "polyhaven", "beige_wall_001"),
    ("Plaster", "polyhaven", "beige_wall_002"),
    ("Plaster", "polyhaven", "plastered_wall"),
    ("Plaster", "polyhaven", "white_plaster_02"),
    ("Plaster", "polyhaven", "white_stucco"),
    ("Plaster", "polyhaven", "white_stucco_02"),
    # Concrete
    ("Concrete", "polyhaven", "concrete_slab_wall"),
    ("Concrete", "polyhaven", "concrete_wall_001"),
    ("Concrete", "polyhaven", "concrete_wall_004"),
    # Roof tiles / slate
    ("Roof", "polyhaven", "grey_roof_tiles"),
    ("Roof", "polyhaven", "roof_slates_02"),
    ("Roof", "polyhaven", "clay_roof_tiles"),
    ("Roof", "polyhaven", "clay_roof_tiles_02"),
    ("Roof", "polyhaven", "clay_roof_tiles_03"),
    ("Roof", "polyhaven", "roof_tiles"),
    # Corrugated metal
    ("Metal", "polyhaven", "corrugated_iron"),
    ("Metal", "polyhaven", "corrugated_iron_02"),
    ("Metal", "polyhaven", "rusty_corrugated_iron"),
    # Wood planks
    ("Wood", "polyhaven", "wood_floor_deck"),
    ("Wood", "polyhaven", "wood_planks"),
    ("Wood", "polyhaven", "wood_planks_grey"),
]

# Map -> glob patterns (first match wins; the normal map lists DX first, OpenGL as a fallback that needs a green flip).
# ambientCG's "<AssetId>.png" preview is never matched because every pattern contains a map suffix.
PATTERNS = {
    "polyhaven": {
        "BaseColor": ["{s}_diff_*.jpg", "{s}_diff_*.png", "{s}_diffuse_*.jpg", "{s}_diffuse_*.png"],
        "Normal": ["{s}_nor_dx_*.png", "{s}_nor_gl_*.png"],
        "Roughness": ["{s}_rough_*.jpg", "{s}_rough_*.png"],
        "AO": ["{s}_ao_*.jpg", "{s}_ao_*.png"],
        "Height": ["{s}_disp_*.png", "{s}_disp_*.jpg", "{s}_displacement_*.png", "{s}_displacement_*.jpg"],
    },
    "ambientcg": {
        "BaseColor": ["{s}_*_Color.*"],
        "Normal": ["{s}_*_NormalDX.*", "{s}_*_NormalGL.*"],
        "Roughness": ["{s}_*_Roughness.*"],
        "AO": ["{s}_*_AmbientOcclusion.*"],
        "Height": ["{s}_*_Displacement.*"],
    },
}
MAPS = ["BaseColor", "Normal", "Roughness", "AO", "Height"]

CS = unreal.TextureCompressionSettings
TG = unreal.TextureGroup
# map -> (compression, srgb, lod group)
TEXTURE_SETTINGS = {
    "BaseColor": (CS.TC_DEFAULT, True, TG.TEXTUREGROUP_WORLD),
    "Normal": (CS.TC_NORMALMAP, False, TG.TEXTUREGROUP_WORLD_NORMAL_MAP),
    "Roughness": (CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
    "AO": (CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
    "Height": (CS.TC_GRAYSCALE, False, TG.TEXTUREGROUP_WORLD_SPECULAR),
}


def find_file(set_dir, source, set_name, map_name):
    """Returns (path, is_opengl_normal) or (None, False)."""
    for pattern in PATTERNS[source][map_name]:
        hits = sorted(glob.glob(os.path.join(set_dir, pattern.format(s=set_name))))
        if hits:
            is_gl = map_name == "Normal" and ("_nor_gl_" in hits[0] or "_NormalGL" in hits[0])
            return hits[0], is_gl
    return None, False


def import_set(category, source, set_name):
    set_dir = os.path.join(RAW_DIR, source, set_name)
    dest = "{}/{}/{}".format(DEST_ROOT, category, set_name)
    jobs = []  # (map, task, is_gl)
    for map_name in MAPS:
        path, is_gl = find_file(set_dir, source, set_name, map_name)
        if path is None:
            unreal.log_warning("[import_textures] {}: no {} map".format(set_name, map_name))
            continue
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", path)
        task.set_editor_property("destination_path", dest)
        task.set_editor_property("destination_name", "T_{}_{}".format(set_name, map_name))
        task.set_editor_property("automated", True)
        task.set_editor_property("replace_existing", True)
        task.set_editor_property("replace_existing_settings", True)
        task.set_editor_property("save", True)
        jobs.append((map_name, task, is_gl))

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([j[1] for j in jobs])

    ok, failed = [], []
    for map_name, task, is_gl in jobs:
        asset_path = "{}/T_{}_{}".format(dest, set_name, map_name)
        tex = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not isinstance(tex, unreal.Texture2D):
            failed.append(map_name)
            unreal.log_error("[import_textures] FAILED to import {} ({})".format(asset_path, task.get_editor_property("filename")))
            continue
        compression, srgb, lod_group = TEXTURE_SETTINGS[map_name]
        tex.set_editor_property("compression_settings", compression)
        tex.set_editor_property("srgb", srgb)
        tex.set_editor_property("lod_group", lod_group)
        if map_name == "Normal":
            tex.set_editor_property("flip_green_channel", bool(is_gl))
        unreal.EditorAssetLibrary.save_loaded_asset(tex)
        ok.append(map_name)
    return ok, failed


def main():
    import sys
    only = next((a.split("=", 1)[1].split(",") for a in sys.argv[1:] if a.startswith("-only=")), None)
    stats = {}
    failures = []
    for index, (category, source, set_name) in enumerate(SETS):
        if only and set_name not in only:
            continue
        unreal.log("[import_textures] ({}/{}) {}/{}".format(index + 1, len(SETS), category, set_name))
        try:
            ok, failed = import_set(category, source, set_name)
        except Exception as exc:  # keep going with the other sets
            unreal.log_error("[import_textures] {} raised {}".format(set_name, exc))
            failures.append((set_name, "exception"))
            continue
        entry = stats.setdefault(category, [0, 0])
        entry[0] += 1
        entry[1] += len(ok)
        failures.extend((set_name, m) for m in failed)
    unreal.log("[import_textures] ===== SUMMARY =====")
    for category, (n_sets, n_tex) in stats.items():
        unreal.log("[import_textures] {}: {} sets, {} textures".format(category, n_sets, n_tex))
    unreal.log("[import_textures] total: {} sets, {} textures, {} failures {}".format(
        sum(v[0] for v in stats.values()), sum(v[1] for v in stats.values()), len(failures), failures))


main()
