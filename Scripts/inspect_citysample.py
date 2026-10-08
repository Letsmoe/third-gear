"""Logs the structure of the City Sample Vehicles blueprints (components, meshes, sockets, bounds, wheel bones)
so the car can be configured in DefaultGame.ini [/Script/DrivingGame.CarSettings].

  UnrealEditor-Cmd <project> -run=pythonscript -script="Scripts/inspect_citysample.py [vehicle02_Car ...]" -unattended -nosplash
Lines are prefixed CSV.
"""
import sys

import unreal

ROOT = "/Game/CitySampleVehicles"
names = sys.argv[1:] or ["vehicle%02d_Car" % i for i in (2, 3, 5, 6, 7, 12, 13)]
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(msg):
    unreal.log_warning("CSV " + msg)


for name in names:
    bp_path = f"{ROOT}/{name}/BP_{name}"
    bp = unreal.load_asset(bp_path)
    if not bp:
        log(f"{name}: cannot load {bp_path}")
        continue
    cls = unreal.EditorAssetLibrary.load_blueprint_class(bp_path)
    log(f"=== {name} class={cls.get_name()}")
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(0, 0, 0))
    for comp in actor.get_components_by_class(unreal.SceneComponent):
        parent = comp.get_attach_parent()
        line = (f"{name} comp {comp.get_name()} [{comp.get_class().get_name()}] parent={parent.get_name() if parent else None}"
                f" socket={comp.get_attach_socket_name()} rel={comp.get_relative_transform().translation}"
                f" rot={comp.get_relative_transform().rotation.rotator()} visible={comp.is_visible()}")
        if isinstance(comp, unreal.SkeletalMeshComponent):
            mesh = comp.get_skeletal_mesh_asset()
            line += f" skm={mesh.get_path_name() if mesh else None} anim={comp.get_editor_property('anim_class')}"
        elif isinstance(comp, unreal.StaticMeshComponent):
            mesh = comp.static_mesh
            line += f" sm={mesh.get_path_name() if mesh else None}"
            if mesh:
                b = mesh.get_bounds()
                line += f" bounds_origin={b.origin} extent={b.box_extent}"
        log(line)
    for comp in actor.get_components_by_class(unreal.SkeletalMeshComponent):
        mesh = comp.get_skeletal_mesh_asset()
        if not mesh:
            continue
        b = mesh.get_bounds()
        log(f"{name} SKM {mesh.get_path_name()} bounds origin={b.origin} extent={b.box_extent}"
            f" physics={mesh.physics_asset.get_path_name() if mesh.physics_asset else None}")
        for i in range(comp.get_num_bones()):
            bone = comp.get_bone_name(i)
            log(f"{name} bone {i} {bone} {comp.get_socket_location(bone)}")
        log(f"{name} SKM materials {[str(m.material_slot_name) for m in mesh.materials]}")
    movement = actor.get_components_by_class(unreal.ActorComponent)
    log(f"{name} other components {[c.get_class().get_name() for c in movement if not isinstance(c, unreal.SceneComponent)]}")
    actor.destroy_actor()
