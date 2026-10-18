"""Generates stone id maps for the paved surfaces: one random value pair per stone, so the surface material can tint every
cobble, brick or paver on its own (see SURFACE_HLSL in Scripts/create_materials.py).

The stones are found in the scan's height map: joints are narrow valleys. Where the stones' own heights vary a lot
(cobbles), the valleys are measured against the local surface (a morphological closing) instead of by one global height.
Touching stones are separated by eroding the stone mask before labelling, then the labels grow back over the stones. The tile is labelled as a 3 x 3 block and
the labels of a stone cut by the tile border are merged, so a stone has one id across the repeat.

Output in <data root>/texturegen/stone_ids/T_StoneIds_<Set>.png, 1024 x 1024 RGBA:
  R, G  two random values per stone (0 to 255), the same for every pixel of the stone
  B     0 in joints; on stones 32 + 16 * code, where code = (sx + 1) + 3 * (sy + 1) and (sx, sy) is the tile offset from this
        pixel's tile to the tile the stone belongs to: a stone cut by the tile border is one stone in the world, so its
        piece on the far side of the border is tinted as part of the neighbouring tile's stone (0, 0 for every other stone)
  A     the stone's own average brightness against the average over all stones, 0.5 + A / 255 (0.5 to 1.5; 128 in joints), so
        the material can level out the scan's own light and dark stones before it tints them at random
Usage: python3 -I Tools/texturegen/stone_ids.py [--preview out_dir]
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402

RESOLUTION = 1024
RAW_ASSETS = os.path.join(data_root.data_root(), "raw_assets")
MINIMUM_STONE_PIXELS = 80
# Set name -> (height map, colour map, valley window in pixels or 0 for a global height threshold, share of the automatic valley
# threshold to use (lower catches fainter joints), pixels to erode so that touching stones separate). The valley window has
# to be wider than a joint.
SETS = {
    "PavingStones092": ("ambientcg/PavingStones092/PavingStones092_2K-JPG_Displacement.jpg",
                        "ambientcg/PavingStones092/PavingStones092_2K-JPG_Color.jpg", 14, 0.6, 3),
    "brick_pavement_03": ("polyhaven/brick_pavement_03/brick_pavement_03_disp_2k.png",
                          "polyhaven/brick_pavement_03/brick_pavement_03_diff_2k.jpg", 0, 1.0, 1),
    "cobblestone_floor_08": ("polyhaven/cobblestone_floor_08/cobblestone_floor_08_disp_4k.png",
                             "polyhaven/cobblestone_floor_08/cobblestone_floor_08_diff_4k.jpg", 44, 0.8, 6),
}


def load_height(relative_path):
    """The height map as floats in 0..1 at RESOLUTION, smoothed a little so that the threshold finds clean joints."""
    image = Image.open(os.path.join(RAW_ASSETS, relative_path))
    height = np.asarray(image, dtype=np.float64)
    if height.ndim == 3:
        height = height[..., 0]
    height = (height - height.min()) / max(height.max() - height.min(), 1e-6)
    resized = Image.fromarray(height.astype(np.float32), mode="F").resize((RESOLUTION, RESOLUTION), Image.BOX)
    return ndimage.gaussian_filter(np.asarray(resized), 1.0, mode="wrap")


def load_luminance(relative_path):
    """The colour map's linear luminance at RESOLUTION."""
    image = Image.open(os.path.join(RAW_ASSETS, relative_path)).convert("RGB").resize((RESOLUTION, RESOLUTION), Image.BOX)
    linear = (np.asarray(image, dtype=np.float64) / 255.0) ** 2.2
    return linear @ np.array([0.2126, 0.7152, 0.0722])


def otsu_threshold(values):
    """Threshold between the joint and stone heights (Otsu's method on a 256 bin histogram)."""
    histogram, edges = np.histogram(values, bins=256, range=(0.0, 1.0))
    weights = histogram / histogram.sum()
    centres = (edges[:-1] + edges[1:]) / 2
    best_threshold, best_variance = 0.5, -1.0
    for index in range(1, 256):
        below, above = weights[:index].sum(), weights[index:].sum()
        if below == 0 or above == 0:
            continue
        mean_below = (weights[:index] * centres[:index]).sum() / below
        mean_above = (weights[index:] * centres[index:]).sum() / above
        variance = below * above * (mean_below - mean_above) ** 2
        if variance > best_variance:
            best_threshold, best_variance = centres[index], variance
    return best_threshold


def stone_mask_of(height, valley_window, threshold_share):
    """True on stones. With a valley window, joints are the pixels well below the closing of the height map."""
    if valley_window == 0:
        return height > otsu_threshold(height)
    closing = ndimage.grey_closing(height, size=(valley_window, valley_window), mode="wrap")
    valley = closing - height
    valley = valley / max(valley.max(), 1e-6)
    return valley < otsu_threshold(valley) * threshold_share


def label_stones(stone_mask, erosion):
    """Integer stone labels (0 = joint) that stay the same across the tile border."""
    size = stone_mask.shape[0]
    core = ndimage.binary_erosion(stone_mask, iterations=erosion, border_value=1) if erosion else stone_mask
    tiled_labels, count = ndimage.label(np.tile(core, (3, 3)))
    labels = tiled_labels[size:2 * size, size:2 * size]
    parent = list(range(count + 1))

    def find(label):
        while parent[label] != label:
            parent[label] = parent[parent[label]]
            label = parent[label]
        return label

    # The same stone seen from the next tile over has another label: merge them.
    for shift_y, shift_x in ((0, size), (size, 0)):
        centre = tiled_labels[size:2 * size, size:2 * size]
        shifted = tiled_labels[size + shift_y:2 * size + shift_y, size + shift_x:2 * size + shift_x]
        for first, second in set(zip(centre[centre > 0].tolist(), shifted[centre > 0].tolist())):
            if second > 0:
                parent[find(first)] = find(second)
    canonical = np.array([find(label) for label in range(count + 1)])
    labels = canonical[labels]
    areas = np.bincount(labels.ravel())
    labels[areas[labels] < MINIMUM_STONE_PIXELS] = 0
    # Grow the labels back over the stone mask.
    _, nearest = ndimage.distance_transform_edt(labels == 0, return_indices=True)
    grown = labels[nearest[0], nearest[1]]
    grown[~stone_mask] = 0
    return grown


def border_offsets(stone):
    """(sx, sy) arrays for one stone's pixels: -1 on the near-edge piece of a stone that wraps across the tile border, so
    the piece is tinted as part of the stone's home tile on the other side; 0 elsewhere."""
    size = stone.shape[0]
    quarter = size // 4
    offsets = []
    for axis in (1, 0):  # columns, then rows
        along = stone.any(axis=1 - axis)
        wraps = along[:quarter].any() and along[size - quarter:].any()
        coordinate = np.arange(size)
        near_edge = (coordinate < size // 2) & wraps
        offsets.append(-near_edge.astype(int))
    return offsets[0][None, :], offsets[1][:, None]


def id_image(labels, luminance):
    """RGBA image: two random bytes per stone in R and G, the stone mask and border offset code in B, the stone's relative
    brightness in A."""
    rng = np.random.default_rng(92)
    unique = np.unique(labels[labels > 0])
    image = np.zeros(labels.shape + (4,), dtype=np.uint8)
    image[..., 3] = 128
    overall_mean = luminance[labels > 0].mean()
    for label in unique:
        stone = labels == label
        relative = np.clip(luminance[stone].mean() / overall_mean, 0.5, 1.5)
        image[stone, 3] = int(round((relative - 0.5) * 255))
        values = rng.integers(0, 256, 2)
        shift_x, shift_y = border_offsets(stone)
        code = (shift_x + 1) + 3 * (shift_y + 1)
        image[stone, 0] = values[0]
        image[stone, 1] = values[1]
        image[..., 2][stone] = (32 + 16 * np.broadcast_to(code, stone.shape))[stone]
    return image, len(unique)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", help="directory for coloured previews")
    arguments = parser.parse_args()
    output_dir = os.path.join(data_root.data_root(), "texturegen", "stone_ids")
    os.makedirs(output_dir, exist_ok=True)
    for name, (height_path, colour_path, valley_window, threshold_share, erosion) in SETS.items():
        height = load_height(height_path)
        stone_mask = stone_mask_of(height, valley_window, threshold_share)
        labels = label_stones(stone_mask, erosion)
        image, count = id_image(labels, load_luminance(colour_path))
        Image.fromarray(image, "RGBA").save(os.path.join(output_dir, f"T_StoneIds_{name}.png"))
        print(f"{name}: {count} stones, {stone_mask.mean() * 100:.0f} % of the tile is stone")
        if arguments.preview:
            os.makedirs(arguments.preview, exist_ok=True)
            Image.fromarray(image[..., :3], "RGB").save(os.path.join(arguments.preview, f"{name}.png"))


if __name__ == "__main__":
    main()
