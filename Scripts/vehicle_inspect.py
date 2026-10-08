"""Prints geometry of a vehicle skeletal mesh (bounds, wheel bone positions, physics bodies) to the log.
Used to fill in UVehicleSpec values when swapping the car asset.

Run headless:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script="<this file> /Game/Vehicles/SportsCar/SKM_SportsCar" -unattended -nosplash
"""
import sys
import unreal

path = sys.argv[1] if len(sys.argv) > 1 else "/Game/Vehicles/SportsCar/SKM_SportsCar"
mesh = unreal.load_asset(path)
if not mesh:
    unreal.log_error(f"VEHINSPECT could not load {path}")
else:
    bounds = mesh.get_bounds()
    unreal.log(f"VEHINSPECT mesh {path}")
    unreal.log(f"VEHINSPECT bounds origin={bounds.origin} extent={bounds.box_extent}")
    skeleton = mesh.skeleton
    unreal.log(f"VEHINSPECT skeleton={skeleton.get_path_name() if skeleton else None} physics={mesh.physics_asset.get_path_name() if mesh.physics_asset else None}")
    lib = unreal.SkeletalMeshEditorSubsystem if hasattr(unreal, "SkeletalMeshEditorSubsystem") else None
    # Bone names + reference pose (component space) via a temporary component in an editor world.
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, 0))
    comp = actor.skeletal_mesh_component
    comp.set_skeletal_mesh_asset(mesh)
    for i in range(comp.get_num_bones()):
        name = comp.get_bone_name(i)
        loc = comp.get_socket_location(name)
        unreal.log(f"VEHINSPECT bone {i} {name} {loc}")
    actor.destroy_actor()
    unreal.log(f"VEHINSPECT materials {[m.material_slot_name for m in mesh.materials]}")
