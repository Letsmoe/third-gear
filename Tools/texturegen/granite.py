"""Generates a tileable grey granite texture set for kerb stones: flamed surface with fine grains of feldspar, quartz and
mica, small pits and a faint large-scale tone drift. One tile is 1 m wide (1 px = 1 mm).

Output in <data root>/texturegen/kerb_granite/ as T_kerb_granite_<Map>.png (BaseColor, Normal in DirectX convention,
Roughness, AO, Height), ready for Scripts/import_generated_textures.py.
Usage: <osmimport venv python> -I Tools/texturegen/granite.py
"""
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402

SIZE = 1024
SET_NAME = "kerb_granite"
NORMAL_STRENGTH = 2.5


def periodic_noise(generator, sigma):
    """Gaussian-filtered white noise that wraps at the borders, normalised to zero mean and unit deviation."""
    noise = ndimage.gaussian_filter(generator.standard_normal((SIZE, SIZE)), sigma, mode="wrap")
    return (noise - noise.mean()) / noise.std()


def build_maps(seed=7):
    """Returns base colour (RGB 0..1), height (0..1), roughness and ambient occlusion arrays."""
    generator = np.random.default_rng(seed)
    grain = periodic_noise(generator, 0.8)
    coarse_grain = periodic_noise(generator, 1.1)
    drift = periodic_noise(generator, 90.0)
    # Light and dark mineral grains: thresholded noise gives crisp speckles.
    light = np.clip((coarse_grain - 0.7) * 1.4, 0, 1)
    dark = np.clip((-grain - 1.05) * 1.8, 0, 1)
    pink = np.clip((periodic_noise(generator, 2.2) - 1.4) * 2.0, 0, 1)
    tone = 0.40 + 0.022 * grain + 0.02 * drift
    tone = tone + 0.035 * light - 0.03 * dark
    color = np.stack([tone, tone, tone * 1.0], axis=-1)
    color[..., 0] += 0.012 * pink
    color[..., 1] -= 0.01 * pink
    height = 0.5 + 0.18 * grain + 0.10 * coarse_grain - 0.35 * dark
    pits = np.clip((-periodic_noise(generator, 1.6) - 2.0) * 2.0, 0, 1)
    height -= 0.4 * pits
    color *= (1.0 - 0.45 * pits)[..., None]
    # Worn, slightly polished patches: a smooth large-scale lowering of the roughness.
    wear = np.clip(0.5 + 0.5 * periodic_noise(generator, 40.0), 0, 1)
    roughness = 0.66 + 0.06 * grain - 0.06 * light - 0.14 * wear
    ambient = 1.0 - 0.5 * pits - 0.15 * dark
    return np.clip(color, 0, 1), (height - height.min()) / (height.max() - height.min()), np.clip(roughness, 0, 1), np.clip(ambient, 0, 1)


def normal_from_height(height):
    """DirectX tangent-space normal map from a height field, wrapping at the borders."""
    slope_x = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * NORMAL_STRENGTH
    slope_y = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * NORMAL_STRENGTH
    normal = np.stack([-slope_x, slope_y, np.ones_like(height)], axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    return normal * 0.5 + 0.5


def save(array, name, folder, grayscale=False):
    """Writes an array in 0..1 as an 8-bit PNG named T_<set>_<name>.png."""
    pixels = (np.clip(array, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(pixels if not grayscale else pixels).save(os.path.join(folder, f"T_{SET_NAME}_{name}.png"))


def build_gutter(seed=21):
    """Gutter paver texture: rows of 10 cm grey granite setts of varying length, running along u. 1024 px = 0.8 m."""
    generator = np.random.default_rng(seed)
    pixels_per_metre = SIZE / 0.8
    row_height = int(round(0.1 * pixels_per_metre))
    tone = np.zeros((SIZE, SIZE))
    joint = np.zeros((SIZE, SIZE))
    for row in range(SIZE // row_height):
        top, bottom = row * row_height, (row + 1) * row_height
        position = int(generator.integers(0, 200))
        while position < SIZE + 400:
            length = int(generator.integers(int(0.11 * pixels_per_metre), int(0.2 * pixels_per_metre)))
            value = generator.normal(0, 1)
            for column in range(position, position + length):
                tone[top:bottom, column % SIZE] = value
                if column - position < 5 or position + length - column <= 5:
                    joint[top:bottom, column % SIZE] = 1
            position += length
        joint[top:top + 5, :] = 1
    joint = ndimage.gaussian_filter(joint, 1.2, mode="wrap")
    grain = periodic_noise(generator, 0.9)
    base = 0.36 + 0.025 * tone + 0.02 * grain + 0.015 * periodic_noise(generator, 30.0)
    base = base * (1 - 0.65 * joint) + 0.15 * joint
    color = np.stack([base, base, base * 1.02], axis=-1)
    height = 0.6 + 0.05 * grain - 0.5 * joint + 0.03 * tone
    return np.clip(color, 0, 1), np.clip(height, 0, 1), np.clip(0.7 - 0.1 * joint, 0, 1), np.clip(1 - 0.6 * joint, 0, 1)


def save_set(set_name, maps):
    """Writes colour, normal, roughness, AO and height arrays of one set."""
    folder = os.path.join(data_root.data_root(), "texturegen", set_name)
    os.makedirs(folder, exist_ok=True)
    color, height, roughness, ambient = maps
    for name, array in (("BaseColor", color), ("Normal", normal_from_height(height)), ("Roughness", roughness), ("AO", ambient), ("Height", height)):
        pixels = (np.clip(array, 0, 1) * 255 + 0.5).astype(np.uint8)
        Image.fromarray(pixels).save(os.path.join(folder, f"T_{set_name}_{name}.png"))
    print("wrote", folder)


def main():
    save_set("gutter_pavers", build_gutter())
    folder = os.path.join(data_root.data_root(), "texturegen", SET_NAME)
    os.makedirs(folder, exist_ok=True)
    color, height, roughness, ambient = build_maps()
    save(color, "BaseColor", folder)
    save(normal_from_height(height), "Normal", folder)
    save(roughness, "Roughness", folder)
    save(ambient, "AO", folder)
    save(height, "Height", folder)
    print("wrote", folder)


if __name__ == "__main__":
    main()
