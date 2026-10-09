"""Builds the fallen leaf atlas from ambientCG leaf sets (CC0, photographed leaves on a scanner).

  Tools/osmimport/.venv/bin/python -I Tools/leafatlas/build_leaf_atlas.py

Downloads the 4K colour and opacity maps of a few autumn leaf sets into <data root>/raw_assets/leaves/ambientcg,
cuts every leaf out with its opacity map, and packs 16 leaves into a 4x4 atlas of 1024 px cells:
<data root>/raw_assets/leaves/leaf_atlas.png (RGBA, alpha = leaf shape) and leaf_atlas.json (species per cell).
Scripts/create_leaf_assets.py imports the result into /Game/Leaves.
"""
import io
import json
import os
import sys
import urllib.request
import zipfile

import numpy
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "bootstrap"))
import data_root  # noqa: E402

CELL_SIZE = 1024
GRID = 4
CELL_PADDING = 12
MINIMUM_LEAF_AREA_FRACTION = 0.35  # of the biggest leaf in the set, drops specks and cut-off leaves
USER_AGENT = "driving-game-asset-fetch/1.0 (personal UE project)"

# set id -> (species label, indices of the leaves to take, counted from the biggest down, evenly spread by colour)
SETS = [
    ("LeafSet030", "oak", 4),
    ("LeafSet028", "maple", 4),
    ("LeafSet021", "maple_yellow", 2),
    ("LeafSet027", "maple_green", 1),
    ("LeafSet007", "elm_yellow", 3),
    ("LeafSet011", "beech_brown", 2),
]


def download_set(set_id, target_dir):
    """Fetches the 4K-JPG zip of one ambientCG set and extracts its colour and opacity maps."""
    colour_path = os.path.join(target_dir, f"{set_id}_Color.jpg")
    opacity_path = os.path.join(target_dir, f"{set_id}_Opacity.jpg")
    if os.path.exists(colour_path) and os.path.exists(opacity_path):
        return colour_path, opacity_path
    os.makedirs(target_dir, exist_ok=True)
    url = f"https://ambientcg.com/get?file={set_id}_4K-JPG.zip"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=300) as response:
        archive = zipfile.ZipFile(io.BytesIO(response.read()))
    for name in archive.namelist():
        base = os.path.basename(name)
        if base.endswith("_Color.jpg"):
            open(colour_path, "wb").write(archive.read(name))
        elif base.endswith("_Opacity.jpg"):
            open(opacity_path, "wb").write(archive.read(name))
    return colour_path, opacity_path


def find_leaves(opacity):
    """Bounding boxes (slices) and masks of the solid leaves in an opacity map, biggest first."""
    solid = opacity > 0.5
    labels, count = ndimage.label(solid)
    areas = ndimage.sum(solid, labels, range(1, count + 1))
    slices = ndimage.find_objects(labels)
    candidates = []
    for index, (area, region) in enumerate(zip(areas, slices), start=1):
        box_area = (region[0].stop - region[0].start) * (region[1].stop - region[1].start)
        if area / box_area < 0.2:
            continue  # thin scan lines and fragments
        candidates.append((area, region, labels[region] == index))
    candidates.sort(key=lambda item: -item[0])
    if not candidates:
        return []
    threshold = candidates[0][0] * MINIMUM_LEAF_AREA_FRACTION
    return [item[1:] for item in candidates if item[0] >= threshold]


def extract_cell(colour, opacity, region, mask):
    """One leaf as an RGBA cell image: colour bled outward past the edge, alpha from the opacity map."""
    colour_crop = colour[region].astype(numpy.float32)
    alpha = opacity[region] * mask
    alpha = ndimage.binary_erosion(alpha > 0.5, iterations=2).astype(numpy.float32)
    alpha = ndimage.gaussian_filter(alpha, 1.0)
    # The scan background around the leaf is blurred or tinted; replace it with the nearest leaf colour.
    distance, nearest = ndimage.distance_transform_edt(alpha < 0.5, return_indices=True)
    bled = colour_crop[nearest[0], nearest[1]]
    rgba = numpy.dstack([bled, alpha * 255.0]).clip(0, 255).astype(numpy.uint8)
    image = Image.fromarray(rgba, "RGBA")
    scale = (CELL_SIZE - 2 * CELL_PADDING) / max(image.size)
    image = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))), Image.LANCZOS)
    cell = Image.new("RGBA", (CELL_SIZE, CELL_SIZE), (0, 0, 0, 0))
    cell.paste(image, ((CELL_SIZE - image.width) // 2, (CELL_SIZE - image.height) // 2))
    return cell


def pick_spread(leaves, count):
    """Picks count leaves spread over the list so that different colours of one set are all represented."""
    if len(leaves) <= count:
        return leaves
    positions = numpy.linspace(0, len(leaves) - 1, count).round().astype(int)
    return [leaves[position] for position in positions]


def main():
    """Downloads the sets, builds the atlas and its description."""
    out_dir = os.path.join(data_root.raw_assets_dir(), "leaves")
    cache_dir = os.path.join(out_dir, "ambientcg")
    atlas = Image.new("RGBA", (CELL_SIZE * GRID, CELL_SIZE * GRID), (0, 0, 0, 0))
    description = []
    for set_id, species, count in SETS:
        colour_path, opacity_path = download_set(set_id, os.path.join(cache_dir, set_id))
        colour = numpy.array(Image.open(colour_path).convert("RGB"))
        opacity = numpy.array(Image.open(opacity_path).convert("L")).astype(numpy.float32) / 255.0
        for region, mask in pick_spread(find_leaves(opacity), count):
            slot = len(description)
            cell = extract_cell(colour, opacity, region, mask)
            atlas.paste(cell, ((slot % GRID) * CELL_SIZE, (slot // GRID) * CELL_SIZE))
            description.append({"cell": slot, "species": species, "source": set_id})
            print(f"cell {slot}: {species} from {set_id}")
    atlas.save(os.path.join(out_dir, "leaf_atlas.png"))
    json.dump({"grid": GRID, "cells": description}, open(os.path.join(out_dir, "leaf_atlas.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
