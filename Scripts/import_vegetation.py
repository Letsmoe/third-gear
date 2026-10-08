"""Places the trees/shrubs of GeoData/build/<area>/vegetation.json into /Game/Maps/<area> (one AVegetationActor).

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script="Scripts/import_vegetation.py <area> [-only=broadleaf,shrub]" -unattended -nosplash

Re-running replaces the previous vegetation. Also called from import_osm_area.py.
"""
import json
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vegetation_models  # noqa: E402

PROJECT_DIR = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
LABEL = "Vegetation"
MAX_ANISOTROPY = 1.6       # limit height-vs-crown stretching of a model
SHRUB_CULL_CM = 40000      # shrubs vanish beyond 400 m (Nanite handles trees at any distance)
COLLIDER_HEIGHT_CM = 400


def place(actor_subsystem, area, only=None, limit=None, label=LABEL, colliders=True):
    """area: build folder name under GeoData/build (e.g. bergedorf_core, horizon)."""
    path = os.path.join(PROJECT_DIR, "GeoData", "build", area, "vegetation.json")
    with open(path) as f:
        models = json.load(f)["models"]

    for actor in actor_subsystem.get_all_level_actors():
        if actor.get_actor_label() == label:
            actor_subsystem.destroy_actor(actor)
    veg = actor_subsystem.spawn_actor_from_class(unreal.VegetationActor, unreal.Vector(0, 0, 0))
    veg.set_actor_label(label)

    collider_points, collider_diams = [], []
    total = 0
    for key, rows in models.items():
        if only and key not in only:
            continue
        if key not in vegetation_models.MODELS:
            unreal.log_warning(f"vegetation: no model for '{key}', skipping {len(rows)} instances")
            continue
        mesh = vegetation_models.load(key)
        native_crown, native_height = vegetation_models.native_size(key)
        transforms = []
        for x, y, z, yaw, crown, height, trunk in rows[:limit]:
            sxy = crown / native_crown
            sz = height / native_height
            sz = min(max(sz, sxy / MAX_ANISOTROPY), sxy * MAX_ANISOTROPY)
            transforms.append(unreal.Transform(location=unreal.Vector(x * 100, y * 100, z * 100),
                                               rotation=unreal.Rotator(roll=0, pitch=0, yaw=yaw),
                                               scale=unreal.Vector(sxy, sxy, sz)))
            if colliders and trunk > 0:
                collider_points.append(unreal.Vector(x * 100, y * 100, z * 100 - 50))
                collider_diams.append(trunk * 100)
        cull = SHRUB_CULL_CM if key == "shrub" else 0
        veg.add_static_instances(mesh, transforms, False, cull)
        total += len(transforms)
        unreal.log(f"vegetation: {key}: {len(transforms)} instances")
    if collider_points:
        veg.add_trunk_colliders(collider_points, collider_diams, COLLIDER_HEIGHT_CM)

    for actor in actor_subsystem.get_all_level_actors():
        if actor.get_actor_label() == "Wind":  # left over from the skinned-foliage attempt
            actor_subsystem.destroy_actor(actor)
    return total


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    area = args[0] if args else "bergedorf_core"
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    level_subsystem.load_level(f"/Game/Maps/{area}")
    only = next((a.split("=", 1)[1].split(",") for a in sys.argv[1:] if a.startswith("-only=")), None)
    limit = next((int(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("-limit=")), None)
    n = place(actor_subsystem, area, only, limit)
    level_subsystem.save_current_level()
    unreal.log_warning(f"import_vegetation: {n} plants placed in {area}")
