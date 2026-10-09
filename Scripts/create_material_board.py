"""Creates /Game/Maps/MaterialBoard: a copy of ProvingGround with one wall panel per facade material (the MB_ instances of
create_facade_materials.py) in rows by group, for judging the materials under daylight, overcast and rain with
Scripts/screenshot.sh. Panels are 2.4 m wide and high, 2.6 m apart along x, rows 3 m apart in height; they face -y.

  UnrealEditor-Cmd <project> -run=pythonscript -script=Scripts/create_material_board.py -unattended -nosplash
"""
import unreal

SRC, DST = "/Game/Maps/ProvingGround", "/Game/Maps/MaterialBoard"
FOLDER = "/Game/World/Facades/Board"
ROWS = [
    ["Facade_ClinkerDeepRed", "Facade_ClinkerYellowBrown", "Facade_BrickGruenderzeit", "Facade_BrickSooty", "Facade_BrickPostwar"],
    ["Facade_RenderScratchWhite", "Facade_RenderScratchBeige", "Facade_RenderScratchPastel", "Facade_RenderScratchGrey",
     "Facade_RenderSmoothWhite"],
    ["Facade_RenderSmoothBeige", "Facade_RenderSmoothPastel", "Facade_RenderSmoothGrey", "Facade_ConcreteSlab", "Facade_ConcreteSmooth"],
    ["Facade_Plinth", "Facade_TimberPainted", "Facade_TimberBeam", "Facade_MetalCladdingGrey", "Facade_MetalCladdingGreen"],
    ["Facade_MetalCladdingWhite", "Roof_Clay", "Roof_ClayOld", "Roof_ConcreteAnthracite", "Roof_Slate"],
    ["Roof_ConcreteGrey", "Roof_MetalGreen", "Facade_Brick", "Facade_Plaster", "Facade_Concrete"],
]
SPACING_CM = 260.0
PANEL_CM = 240.0

assets = unreal.EditorAssetLibrary
if assets.does_asset_exist(DST):
    assets.delete_asset(DST)
assets.duplicate_asset(SRC, DST)
level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
level.load_level(DST)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
cube = unreal.load_asset("/Engine/BasicShapes/Cube")

for row, names in enumerate(ROWS):
    for column, name in enumerate(names):
        material = unreal.load_asset(f"{FOLDER}/MB_{name}")
        if material is None:
            unreal.log_warning(f"MaterialBoard: MB_{name} missing")
            continue
        center = unreal.Vector(column * SPACING_CM, 0, 10 + row * 300 + PANEL_CM / 2 + 20)
        actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, center, unreal.Rotator(0, 0, 0))
        actor.set_actor_label(name)
        actor.set_actor_scale3d(unreal.Vector(PANEL_CM / 100.0, 0.25, PANEL_CM / 100.0))
        component = actor.static_mesh_component
        component.set_static_mesh(cube)
        component.set_material(0, material)
# The panels face -y, so the sun travels towards +y and a little east: lit from the front, shadows to the side.
for actor in actors.get_all_level_actors():
    if isinstance(actor, unreal.DirectionalLight):
        actor.set_actor_rotation(unreal.Rotator(roll=0, pitch=-42, yaw=55), False)
level.save_current_level()
unreal.log_warning(f"MaterialBoard: {sum(len(r) for r in ROWS)} panels")
