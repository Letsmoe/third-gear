"""Bakes the skinned PVE sample trees into static Nanite-assembly meshes under /Game/Vegetation.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/bake_vegetation.py -unattended -nosplash
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vegetation_models  # noqa: E402

for key, (source_path, baked_path) in vegetation_models.MODELS.items():
    source = unreal.load_asset(source_path)
    mesh = unreal.VegetationAssetTools.bake_skinned_assembly_to_static(source, baked_path, True)
    if mesh is None:
        unreal.log_error(f"bake_vegetation: {key} failed")
        continue
    w, h = vegetation_models.native_size(key)
    unreal.log_warning(f"bake_vegetation: {key} -> {baked_path}: crown {w:.1f} m, height {h:.1f} m")
