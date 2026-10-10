"""Creates /Game/World/Structures/L_StructureLineup: a copy of ProvingGround with every structure piece of
Scripts/import_structures.py standing in rows, for checking the import with Scripts/screenshot.sh.

  UnrealEditor-Cmd <project> -run=pythonscript -script=Scripts/create_structure_lineup.py -unattended -nosplash

Rows along x (east), one per family, 60 m apart along y (south): shelters at y = 0, petrol 60, greenhouses 120,
bridges 180, pylons 240, turbines 400 (assembled: tower, nacelle and rotor), and the Elbphilharmonie at y = 700.
The map lives in the data root with the meshes, not in git.
"""
import unreal

SOURCE, TARGET = "/Game/Maps/ProvingGround", "/Game/World/Structures/L_StructureLineup"
MESHES = "/Game/World/Structures/Meshes"
GAP_CM = 600.0
ROWS = [("shelters", 0.0), ("petrol", 6000.0), ("greenhouses", 12000.0), ("bridges", 18000.0), ("pylons", 24000.0)]
# Turbines: hub height, overhang (hub ahead of the tower axis) and tower height in metres, from kit/turbines.py.
TURBINES = {"NM48": (46.0, 2.6, 44.5), "E92": (104.0, 5.0, 101.0), "MM100": (100.0, 5.2, 97.7), "N117": (91.0, 5.6, 88.6)}
TURBINE_ROW_CM = 40000.0
ELBPHILHARMONIE_CM = (0.0, 70000.0, 0.0)

assets = unreal.EditorAssetLibrary
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def place(mesh_path, location, label, yaw=0.0):
    """Spawns one static mesh actor at a location in cm."""
    mesh = unreal.load_asset(mesh_path)
    actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(*location), unreal.Rotator(0, 0, yaw))
    actor.static_mesh_component.set_static_mesh(mesh)
    actor.set_actor_label(label)
    return mesh


def place_row(family, row_y):
    """Every mesh of a family side by side along x, each pushed right by its own width."""
    x = 0.0
    for path in sorted(assets.list_assets(f"{MESHES}/{family}", recursive=False, include_folder=False)):
        mesh = unreal.load_asset(path)
        box = mesh.get_bounding_box()
        x -= box.min.x
        place(path, (x, row_y, 0.0), mesh.get_name())
        unreal.log_warning(f"LINEUP {mesh.get_name()} at x={x / 100:.1f} y={row_y / 100:.1f} m")
        x += box.max.x + GAP_CM


def place_turbines():
    """The four turbines assembled 150 m apart, rotors facing south (+y, the kit's front is -y in Blender)."""
    for index, (name, (hub, overhang, tower)) in enumerate(TURBINES.items()):
        x = index * 15000.0
        place(f"{MESHES}/turbines/SM_turbines_{name}_Tower", (x, TURBINE_ROW_CM, 0.0), name + "_Tower")
        place(f"{MESHES}/turbines/SM_turbines_{name}_Nacelle", (x, TURBINE_ROW_CM, tower * 100.0), name + "_Nacelle")
        place(f"{MESHES}/turbines/SM_turbines_{name}_Rotor", (x, TURBINE_ROW_CM + overhang * 100.0, hub * 100.0),
              name + "_Rotor")


def main():
    if assets.does_asset_exist(TARGET):
        assets.delete_asset(TARGET)
    assets.duplicate_asset(SOURCE, TARGET)
    level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    level.load_level(TARGET)
    for family, row_y in ROWS:
        place_row(family, row_y)
    place_turbines()
    place(f"{MESHES}/landmarks/SM_landmarks_Elbphilharmonie", ELBPHILHARMONIE_CM, "Elbphilharmonie")
    level.save_current_level()
    unreal.log_warning("create_structure_lineup: done")


main()
