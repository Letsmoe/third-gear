"""Imports an area built by Tools/osmimport/build_area.py into Unreal and creates a level for it.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script="Scripts/import_osm_area.py <area> [-nohorizon]" -unattended -nosplash

Creates /Game/World/<area>/SM_tile_*.uasset, placeholder materials in /Game/World/Materials (only if missing, so
hand-made/real materials are never overwritten) and the level /Game/Maps/<area>.
"""
import json
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_vegetation  # noqa: E402
import world_lighting  # noqa: E402

PROJECT_DIR = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
MATERIAL_FOLDER = "/Game/World/Materials"
EYE_HEIGHT_M = 1.2

# Placeholder look per mesh section: (base colour sRGB 0..1, roughness, metallic)
PLACEHOLDERS = {
    "Terrain_Grass": ((0.20, 0.30, 0.12), 0.95, 0.0),
    "Road_Asphalt": ((0.08, 0.08, 0.085), 0.85, 0.0),
    "Road_Pavers": ((0.32, 0.22, 0.20), 0.85, 0.0),
    "Road_Cobble": ((0.25, 0.24, 0.23), 0.8, 0.0),
    "Pavement": ((0.45, 0.43, 0.40), 0.85, 0.0),
    "Kerb": ((0.55, 0.55, 0.55), 0.75, 0.0),
    "Marking_White": ((0.85, 0.85, 0.85), 0.6, 0.0),
    "Bridge_Concrete": ((0.5, 0.5, 0.48), 0.85, 0.0),
    "Facade_Brick": ((0.42, 0.16, 0.10), 0.85, 0.0),
    "Facade_Plaster": ((0.80, 0.76, 0.66), 0.9, 0.0),
    "Facade_Concrete": ((0.55, 0.55, 0.53), 0.9, 0.0),
    "Facade_Metal": ((0.45, 0.48, 0.50), 0.5, 0.8),
    "Facade_Glass": ((0.6, 0.7, 0.7), 0.1, 0.0),
    "Roof_Tiles": ((0.45, 0.13, 0.08), 0.8, 0.0),
    "Roof_Flat": ((0.18, 0.18, 0.18), 0.9, 0.0),
    "Roof_Glass": ((0.6, 0.7, 0.7), 0.1, 0.0),
    "Canopy_Far": ((0.13, 0.20, 0.08), 0.95, 0.0),  # distant forest silhouettes (build_horizon.py)
}

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary
level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def ensure_placeholder_material(section):
    path = f"{MATERIAL_FOLDER}/M_{section}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return
    color, roughness, metallic = PLACEHOLDERS.get(section, ((0.5, 0.5, 0.5), 0.8, 0.0))
    material = asset_tools.create_asset(f"M_{section}", MATERIAL_FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    base = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -400, 0)
    # sRGB -> linear so the colours match what you'd pick in a colour picker
    base.set_editor_property("constant", unreal.LinearColor(*[c ** 2.2 for c in color], 1.0))
    mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 200)
    rough.set_editor_property("r", roughness)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    if metallic:
        metal = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -400, 300)
        metal.set_editor_property("r", metallic)
        mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    material.set_editor_property("used_with_nanite", True)
    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)


def spawn(actor_class, location, rotation=unreal.Rotator(0, 0, 0), label=None):
    actor = actor_subsystem.spawn_actor_from_class(actor_class, location, rotation)
    if label:
        actor.set_actor_label(label)
    return actor


def import_tiles(area, build_dir, manifest):
    meshes = []
    for tile in manifest["tiles"]:
        mesh = unreal.DgMeshImporter.import_dg_mesh(os.path.join(build_dir, tile["file"]),
                                                     f"/Game/World/{area}/SM_{tile['name']}", MATERIAL_FOLDER, True, True)
        if mesh is None:
            unreal.log_error(f"Import failed for {tile['file']}")
            continue
        meshes.append((tile, mesh))
    return meshes


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    area = args[0] if args else "bergedorf_test"
    build_dir = os.path.join(PROJECT_DIR, "GeoData", "build", area)
    with open(os.path.join(build_dir, "manifest.json")) as f:
        manifest = json.load(f)

    sections = sorted({s for t in manifest["tiles"] for s in t["sections"]})
    for section in sections:
        ensure_placeholder_material(section)

    meshes = import_tiles(area, build_dir, manifest)
    # Low-detail surroundings built for this area by Tools/osmimport/build_horizon.py
    horizon_dir = os.path.join(PROJECT_DIR, "GeoData", "build", "horizon")
    horizon = None
    if os.path.exists(os.path.join(horizon_dir, "manifest.json")):
        with open(os.path.join(horizon_dir, "manifest.json")) as f:
            horizon = json.load(f)
        if horizon.get("detail") != area or "-nohorizon" in sys.argv:
            horizon = None
    if horizon:
        for section in sorted({s for t in horizon["tiles"] for s in t["sections"]}):
            ensure_placeholder_material(section)
        meshes += import_tiles("horizon", horizon_dir, horizon)

    map_path = f"/Game/Maps/{area}"
    if unreal.EditorAssetLibrary.does_asset_exist(map_path):
        level_subsystem.load_level(map_path)
        for actor in actor_subsystem.get_all_level_actors():
            actor_subsystem.destroy_actor(actor)
    else:
        level_subsystem.new_level(map_path)
    world_lighting.setup(actor_subsystem)
    for tile, mesh in meshes:
        x, y, z = tile["pivot_cm"]
        actor = spawn(unreal.StaticMeshActor, unreal.Vector(x, y, z), label=tile["name"])
        actor.static_mesh_component.set_static_mesh(mesh)
        actor.static_mesh_component.set_mobility(unreal.ComponentMobility.STATIC)
    start = manifest["start"]
    spawn(unreal.PlayerStart, unreal.Vector(start["x"] * 100, start["y"] * 100, (start["z"] + EYE_HEIGHT_M) * 100),
          unreal.Rotator(roll=0, pitch=0, yaw=start["yaw"]), "PlayerStart")
    if os.path.exists(os.path.join(build_dir, "vegetation.json")):
        n = import_vegetation.place(actor_subsystem, area)
        unreal.log(f"Placed {n} plants")
    if horizon and os.path.exists(os.path.join(horizon_dir, "vegetation.json")):
        n = import_vegetation.place(actor_subsystem, "horizon", label="Vegetation_Far", colliders=False)
        unreal.log(f"Placed {n} distant trees")
    level_subsystem.save_current_level()
    unreal.log(f"Imported area {area}: {len(meshes)} tiles, start on {start.get('road', '?')}")


main()
