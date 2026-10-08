"""Creates /Game/Maps/CarLineup: a copy of ProvingGround with the City Sample cars parked in a row (facing east,
2 m apart along y, starting at the origin), for choosing/checking car models with Scripts/screenshot.sh.

  UnrealEditor-Cmd <project> -run=pythonscript -script=Scripts/create_car_lineup.py -unattended -nosplash
"""
import unreal

SRC, DST = "/Game/Maps/ProvingGround", "/Game/Maps/CarLineup"
CARS = ["vehicle02_Car", "vehicle03_Car", "vehicle05_Car", "vehicle06_Car", "vehicle07_Car", "vehicle12_Car",
        "vehicle13_Car"]
SPACING_CM = 400

assets = unreal.EditorAssetLibrary
if assets.does_asset_exist(DST):
    assets.delete_asset(DST)
assets.duplicate_asset(SRC, DST)
level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
level.load_level(DST)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

for i, name in enumerate(CARS):
    path = f"/Game/CitySampleVehicles/{name}/BP_{name}"
    cls = unreal.EditorAssetLibrary.load_blueprint_class(path)
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(0, i * SPACING_CM, 2), unreal.Rotator(0, 0, 0))
    actor.set_actor_label(name)
    for comp in actor.get_components_by_class(unreal.PrimitiveComponent):
        comp.set_simulate_physics(False)
    unreal.log_warning(f"LINEUP {name} at y={i * SPACING_CM / 100} m")

level.save_current_level()
