"""Creates /Game/Maps/ProvingGround: a flat 4x4 km test area with physically based sky and lighting,
plus distance markers for judging view distance and scale in VR.

Run headless:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

MAP_PATH = "/Game/Maps/ProvingGround"
EYE_HEIGHT_CM = 120.0  # roughly a seated driver's eye height in a compact car

level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    unreal.EditorAssetLibrary.delete_asset(MAP_PATH)
level_subsystem.new_level(MAP_PATH)


def spawn(actor_class, location=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), label=None):
    actor = actor_subsystem.spawn_actor_from_class(
        actor_class,
        unreal.Vector(*location),
        unreal.Rotator(roll=rotation[0], pitch=rotation[1], yaw=rotation[2]),
    )
    if label:
        actor.set_actor_label(label)
    return actor


def spawn_mesh(mesh_path, location, scale, label, material_path=None):
    actor = spawn(unreal.StaticMeshActor, location, label=label)
    component = actor.static_mesh_component
    component.set_static_mesh(unreal.load_asset(mesh_path))
    if material_path:
        component.set_material(0, unreal.load_asset(material_path))
    actor.set_actor_scale3d(unreal.Vector(*scale))
    return actor


# --- Sky and lighting (all movable: Lumen, no baked lighting) ---
sun = spawn(unreal.DirectionalLight, rotation=(0.0, -35.0, -40.0), label="Sun")
sun_component = sun.get_component_by_class(unreal.DirectionalLightComponent)
sun_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sun_component.set_editor_property("atmosphere_sun_light", True)
sun_component.set_editor_property("intensity", 10.0)  # lux, physically plausible daylight with auto-exposure

spawn(unreal.SkyAtmosphere, label="SkyAtmosphere")
sky_light = spawn(unreal.SkyLight, label="SkyLight")
sky_light_component = sky_light.get_component_by_class(unreal.SkyLightComponent)
sky_light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky_light_component.set_editor_property("real_time_capture", True)
spawn(unreal.ExponentialHeightFog, label="HeightFog")
# No VolumetricCloud: it costs ~5 ms GPU in VR stereo regardless of quality settings (see CLAUDE.md).

# --- Ground: 4 km x 4 km (engine plane is 1 m x 1 m) ---
spawn_mesh("/Engine/BasicShapes/Plane", (0.0, 0.0, 0.0), (4000.0, 4000.0, 1.0), "Ground",
           "/Engine/EngineMaterials/DefaultMaterial")

# --- Distance markers along +X: 2 m tall posts, plus a car-sized box every 100 m ---
for distance_m in [10, 25, 50, 100, 200, 300, 500, 750, 1000, 1500]:
    x = distance_m * 100.0
    spawn_mesh("/Engine/BasicShapes/Cylinder", (x, -400.0, 100.0), (0.3, 0.3, 2.0), f"Post_{distance_m}m")
    spawn_mesh("/Engine/BasicShapes/Cube", (x, 400.0, 75.0), (4.3, 1.8, 1.5), f"CarBox_{distance_m}m")

# --- Player start at driver eye height ---
spawn(unreal.PlayerStart, (0.0, 0.0, EYE_HEIGHT_CM), label="PlayerStart")

level_subsystem.save_current_level()
unreal.log(f"Created {MAP_PATH}")
