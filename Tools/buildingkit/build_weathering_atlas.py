"""Packs the Megascans grime brushes into one atlas for the facade weathering overlay (Scripts/create_facade_materials.py,
Plugins/MapRuntime/Shaders/Private/FacadeWeathering.ush). Reads raw_assets/megascans, writes <data root>/facade_weathering/
weathering_atlas.png and weathering_moss.png (derived from Fab content: stays in the data root, never committed).

Atlas, 4 x 2 tiles of 1024 px, one grayscale mask each (all stored in red, green and blue the same):
  0 wide drips from a ledge   1 drips under a sill   2 single long streak   3 thin streak (downpipe, crack)
  4 splash dots (plinth)      5 to 7 unused
The moss image is Tileable Moss Patches: colour in RGB, its opacity in alpha.

Usage: python3 -I Tools/buildingkit/build_weathering_atlas.py
"""
import glob
import os

from PIL import Image, ImageOps

DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
MEGASCANS = os.path.join(DATA_ROOT, "raw_assets", "megascans")
OUT_DIR = os.path.join(DATA_ROOT, "facade_weathering")
TILE = 1024
BRUSHES = [
    ("leakage_16f6623b", "Brush"),
    ("leakage_74377d74", "Brush"),
    ("leakage_ab46f137", "Brush"),
    ("leakage_e562483a", "Brush"),
    ("mud_stain_0192ac68", "Brush"),
]


def load_brush(folder, kind):
    """Largest-available brush as a grayscale image of the tile size, contrast stretched to the full range."""
    files = sorted(glob.glob(os.path.join(MEGASCANS, folder, f"*_4K_{kind}.jpg")))
    if not files:
        raise SystemExit(f"{folder} has no 4K {kind}: download the Megascans first")
    image = Image.open(files[0]).convert("L").resize((TILE, TILE), Image.LANCZOS)
    return ImageOps.autocontrast(image, cutoff=0.5)


def build_moss():
    """RGBA moss tile: base colour with the scan's opacity as alpha."""
    folder = os.path.join(MEGASCANS, "tileable_moss_patches_a9a94514")
    colour = Image.open(glob.glob(os.path.join(folder, "*_4K_BaseColor.jpg"))[0]).convert("RGB").resize((TILE, TILE), Image.LANCZOS)
    opacity = Image.open(glob.glob(os.path.join(folder, "*_4K_Opacity.jpg"))[0]).convert("L").resize((TILE, TILE), Image.LANCZOS)
    colour.putalpha(opacity)
    path = os.path.join(OUT_DIR, "weathering_moss.png")
    colour.save(path)
    print("wrote", path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    build_moss()
    atlas = Image.new("L", (TILE * 4, TILE * 2), 0)
    for index, (folder, kind) in enumerate(BRUSHES):
        atlas.paste(load_brush(folder, kind), ((index % 4) * TILE, (index // 4) * TILE))
    path = os.path.join(OUT_DIR, "weathering_atlas.png")
    atlas.convert("RGB").save(path)
    print("wrote", path)


main()
