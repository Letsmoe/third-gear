"""Creates /Game/Maps/WindowTest: a short street of kit facades (brick town houses with timber windows on one side,
plastered houses with PVC windows and a block with aluminium windows on the other) for judging the window materials.

  Scripts/lock.sh gpu UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_window_test_map.py -unattended -nosplash

The kit pieces come from the building kit's glb files (Tools/buildingkit/README.md) and are imported into
/Game/WindowTest/Kit. Run create_window_materials.py first. The street runs along x; the brick row stands at y = 0 and
faces +y (north side), the other row at y = 20 faces -y. Eye height 1.6 m at y = 10 is a pavement view.
"""
import math
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import world_lighting  # noqa: E402

KIT = "/mnt/storage/third-gear/building_kit/glb"
KIT_FOLDER = "/Game/WindowTest/Kit"
# WINDOW_TEST_FLAT=1 builds /Game/Maps/WindowTestFlat with plain dark glass, the baseline for GPU timings.
FLAT = os.environ.get("WINDOW_TEST_FLAT") == "1"
MAP = "/Game/Maps/WindowTestFlat" if FLAT else "/Game/Maps/WindowTest"
WINDOWS = "/Game/World/Windows"
MODULE_CM = 200.0

eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def import_piece(style, piece):
    """Imports one kit glb as a static mesh and returns it (the later runs find it already imported)."""
    name = f"{style}_{piece}"
    destination = f"{KIT_FOLDER}/{name}"
    existing = [path for path in eal.list_assets(destination, recursive=True, include_folder=False) if "/StaticMeshes/" in path]
    if not existing:
        task = unreal.AssetImportTask()
        task.set_editor_property("filename", f"{KIT}/{style}/{name}.glb")
        task.set_editor_property("destination_path", destination)
        task.set_editor_property("automated", True)
        task.set_editor_property("save", True)
        asset_tools.import_asset_tasks([task])
        existing = [path for path in task.get_editor_property("imported_object_paths") if "/StaticMeshes/" in path]
    if not existing:
        raise RuntimeError(f"importing {name} produced no static mesh")
    return unreal.load_asset(existing[0])


def wall_instance(source_name, name):
    """A copy of a facade instance without the painted windows (the kit walls have real openings)."""
    path = f"{WINDOWS}/{name}"
    if not eal.does_asset_exist(path):
        eal.duplicate_asset(f"/Game/World/Materials/{source_name}", path)
    instance = unreal.load_asset(path)
    unreal.MaterialEditingLibrary.set_material_instance_static_switch_parameter_value(instance, "Windows", False)
    eal.save_loaded_asset(instance)
    return instance


def style_materials(style):
    """Material per kit slot name for a style: wall material, sills, and the frame and glass of its window type."""
    brick = wall_instance("M_Facade_Brick", "MI_KitWall_Brick")
    plaster = wall_instance("M_Facade_Plaster", "MI_KitWall_Plaster")
    concrete = wall_instance("M_Facade_Concrete", "MI_KitWall_Concrete")
    window_type = {"brick": "Timber", "plaster": "PVC", "block": "Aluminium"}[style]
    wall = {"brick": brick, "plaster": plaster, "block": concrete}[style]
    return {
        "Brick": brick, "Plaster": plaster, "Concrete": concrete, "Sill": concrete, "Timber": brick, "Metal": concrete,
        "Frame": unreal.load_asset(f"{WINDOWS}/MI_WindowFrame_{window_type}"),
        "Glass": unreal.load_asset(f"{WINDOWS}/M_WindowGlassFlat" if FLAT else f"{WINDOWS}/MI_WindowGlass_{window_type}"),
        "_wall": wall,
    }


def place(actor_subsystem, mesh, location_cm, yaw=0.0, materials=None):
    """Spawns a static mesh actor; materials maps slot names to the materials this actor uses (the mesh asset stays as
    imported, so the flat baseline map can share it)."""
    actor = actor_subsystem.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(*location_cm), unreal.Rotator(0, 0, yaw))
    component = actor.static_mesh_component
    component.set_static_mesh(mesh)
    for index, slot in enumerate(mesh.static_materials):
        name = str(slot.material_slot_name)
        if materials and name in materials:
            component.set_material(index, materials[name])
    return actor


def build_row(actor_subsystem, style, bays, storeys, origin_cm, yaw, door_bay=None):
    """A facade of the style: per storey a row of window bays (a door bay on the ground floor), string courses and a
    plinth. The row runs along its local x; yaw 180 turns it to face the other way."""
    storey_cm = {"brick": 325.0, "plaster": 275.0, "block": 275.0}[style]
    materials = style_materials(style)
    pieces = {}

    def piece(name):
        if name not in pieces:
            pieces[name] = import_piece(style, name)
        return pieces[name]

    cos_yaw, sin_yaw = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))

    def at(x_cm, z_cm):
        return (origin_cm[0] + x_cm * cos_yaw, origin_cm[1] + x_cm * sin_yaw, origin_cm[2] + z_cm)

    for storey in range(storeys):
        for bay in range(bays):
            is_door = storey == 0 and door_bay is not None and bay == door_bay
            wall = "Wall_Door" if is_door else "Wall_Window"
            location = at(bay * MODULE_CM, storey * storey_cm)
            place(actor_subsystem, piece(wall), location, yaw, materials)
            if is_door:
                # Door leaves and fanlights get frosted stairwell glass, not a room.
                door_materials = dict(materials, Glass=unreal.load_asset(f"{WINDOWS}/MI_WindowGlass_Door"))
                place(actor_subsystem, piece("Door_Glazed" if style == "block" else "Door"), location, yaw, door_materials)
                continue
            place(actor_subsystem, piece("Window"), location, yaw, materials)
            place(actor_subsystem, piece("Sill"), location, yaw, materials)
            if style == "brick":
                place(actor_subsystem, piece("Lintel_Arch_Window"), location, yaw, materials)
        for bay in range(bays):
            if style == "brick":
                place(actor_subsystem, piece("StringCourse_M"), at(bay * MODULE_CM, storey * storey_cm), yaw, materials)
    for bay in range(bays):
        place(actor_subsystem, piece("Plinth_M"), at(bay * MODULE_CM, 0), yaw, materials)
        if style == "brick":
            place(actor_subsystem, piece("Cornice_M"), at(bay * MODULE_CM, (storeys - 1) * storey_cm), yaw, materials)


def build_ground(actor_subsystem):
    """Street and pavements as scaled planes."""
    plane = unreal.load_asset("/Engine/BasicShapes/Plane")
    for label, centre_y, width_m, material_name, height in (("Road", 10.0, 9.0, "M_Road_Asphalt", 0.0),
                                                            ("PavementNorth", 2.5, 4.0, "M_Pavement", 6.0),
                                                            ("PavementSouth", 17.5, 4.0, "M_Pavement", 6.0)):
        actor = place(actor_subsystem, plane, (1500.0, centre_y * 100.0, height), 0.0)
        actor.set_actor_scale3d(unreal.Vector(40.0, width_m, 1.0))
        actor.static_mesh_component.set_material(0, unreal.load_asset(f"/Game/World/Materials/{material_name}"))
        actor.set_actor_label(label)


def main():
    """Rebuilds the test map."""
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if eal.does_asset_exist(MAP):
        level_subsystem.load_level(MAP)
        for actor in actor_subsystem.get_all_level_actors():
            actor_subsystem.destroy_actor(actor)
    else:
        level_subsystem.new_level(MAP)
    world_lighting.setup(actor_subsystem)
    build_ground(actor_subsystem)
    # The player starts in the middle of the street looking along it, for the GPU profile.
    actor_subsystem.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(-800.0, 1000.0, 30.0), unreal.Rotator(0, 0, 0))
    # North side: brick town houses (timber windows), then a plastered house row.
    build_row(actor_subsystem, "brick", 8, 3, (0.0, 0.0, 6.0), 0.0, door_bay=3)
    build_row(actor_subsystem, "plaster", 6, 3, (1600.0, 0.0, 6.0), 0.0, door_bay=2)
    # South side: block with aluminium windows, facing back (rotated half a turn, so it runs towards -x).
    build_row(actor_subsystem, "block", 10, 4, (2800.0, 2000.0, 6.0), 180.0, door_bay=4)
    build_row(actor_subsystem, "brick", 6, 3, (800.0, 2000.0, 6.0), 180.0, door_bay=None)
    level_subsystem.save_current_level()
    unreal.log_warning(f"create_window_test_map: {MAP} built")


main()
