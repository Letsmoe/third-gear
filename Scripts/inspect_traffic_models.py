"""Logs what AI traffic needs to know about each City Sample vehicle: mesh bounds, wheel positions, material slots and
the parameters of the paint material. Lines are prefixed TM.

  Scripts/lock.sh gpu UnrealEditor-Cmd <project> -run=pythonscript -script="Scripts/inspect_traffic_models.py [folder ...]"
Folder names are like vehicle07_Car (default: all cars, vans, the bus).
"""
import sys

import unreal

ROOT = "/Game/CitySampleVehicles"
DEFAULT_FOLDERS = ["vehicle01_Van", "vehicle02_Car", "vehicle03_Car", "vehicle05_Car", "vehicle06_Car", "vehicle07_Car",
                   "vehicle09_Van", "vehicle10_Bus", "vehicle12_Car", "vehicle13_Car"]
folders = sys.argv[1:] or DEFAULT_FOLDERS


def log(message):
    unreal.log_warning("TM " + message)


def mesh_summary(path):
    mesh = unreal.load_asset(path)
    if mesh is None:
        log(f"  missing {path}")
        return None
    bounds = mesh.get_bounds()
    log(f"  {path.split('/')[-1]} origin={bounds.origin} extent={bounds.box_extent}")
    return mesh


def material_summary(mesh):
    for index, slot in enumerate(mesh.static_materials):
        material = slot.material_interface
        if material is None:
            log(f"    slot {index} {slot.material_slot_name}: none")
            continue
        parent = material.get_class().get_name()
        parameters = ""
        if isinstance(material, unreal.MaterialInstanceConstant):
            vectors = [str(v.parameter_info.name) for v in material.vector_parameter_values]
            scalars = [str(v.parameter_info.name) for v in material.scalar_parameter_values]
            parameters = f" parent={material.parent.get_name() if material.parent else None} vectors={vectors} scalars={scalars}"
        log(f"    slot {index} {slot.material_slot_name}: {material.get_name()} [{parent}]{parameters}")


for folder in folders:
    kind = folder.split("_")[1]
    tag = folder.split("_")[0]
    prefix = f"veh{kind}"
    log(f"=== {folder}")
    base = f"{ROOT}/{folder}/Mesh"
    body = mesh_summary(f"{base}/SM_{prefix}_{tag}_No_Wheel")
    if body:
        material_summary(body)
    for name in ("All_Trans", "Trans"):
        glass = unreal.load_asset(f"{base}/SM_{name}_{prefix}_{tag}") if name == "All_Trans" else unreal.load_asset(f"{base}/SM_{prefix}_{tag}_{name}")
        if glass:
            log(f"  glass {name}")
            mesh_summary(f"{base}/SM_{name}_{prefix}_{tag}" if name == "All_Trans" else f"{base}/SM_{prefix}_{tag}_{name}")
    for wheel in ("Front_L", "Front_R", "Rear_L", "Rear_R"):
        mesh = mesh_summary(f"{base}/SM_Wheel_{wheel}_{prefix}_{tag}")
        if mesh and wheel == "Front_L":
            material_summary(mesh)
    for pad in ("L", "R"):
        mesh_summary(f"{base}/SM_Brake_Pad_{pad}_{prefix}_{tag}")
    skeletal = unreal.load_asset(f"{base}/SKM_{prefix}_{tag}")
    if skeletal:
        log(f"  skeletal ok, bounds={skeletal.get_bounds().box_extent}")
