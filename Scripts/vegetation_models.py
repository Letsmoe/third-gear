"""Vegetation model table shared by import scripts: model key (as written by Tools/osmimport vegetation.json) ->
Unreal asset + its natural size, so instances can be scaled to the real tree's crown diameter / height.

The scanned trees come from the engine's Procedural Vegetation Editor samples as skinned Nanite assemblies.
Skinned foliage + virtual shadow maps crashes the GPU (UE 5.8, Vulkan/Linux), so Scripts/bake_vegetation.py bakes
them into static Nanite assemblies under /Game/Vegetation, which are what we place.
"""
import unreal

PVE = "/ProceduralVegetationEditor/SampleAssets/StarterContent"
BAKED = "/Game/Vegetation"

# key: (source skinned asset, baked static asset)
MODELS = {
    "broadleaf": (f"{PVE}/DeciduousTree_01/PVE_Deciduous_Tree_01", f"{BAKED}/SM_Broadleaf_01"),
    "conifer": (f"{PVE}/ConiferTree_01/PVE_Conifer_01", f"{BAKED}/SM_Conifer_01"),
    "shrub": (f"{PVE}/Deciduous_Shrub_01/PVE_Deciduous_Shrub_01", f"{BAKED}/SM_Shrub_01"),
}

_cache = {}


def load(key):
    """Baked static mesh for a model key."""
    if key not in _cache:
        path = MODELS[key][1]
        asset = unreal.load_asset(path)
        if asset is None:
            raise RuntimeError(f"vegetation model {key}: {path} missing - run Scripts/bake_vegetation.py")
        _cache[key] = asset
    return _cache[key]


def native_size(key):
    """(crown diameter, height) of the model in metres."""
    box = load(key).get_bounding_box()
    width = max(box.max.x - box.min.x, box.max.y - box.min.y)
    return width / 100.0, (box.max.z - box.min.z) / 100.0
