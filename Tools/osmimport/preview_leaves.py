"""Top-down images of the settled leaf field (debug aid), from the leaves_debug.npz that build_world.py writes.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/preview_leaves.py <region> <out_dir> [x,y,half_size_m ...]
Each window is drawn with kerbs, buildings, hedges and tree crowns over the pile depth in cm.
"""
import os
import sys

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402


def draw_window(data, center_x, center_y, half, path):
    """One image of the field around a world position."""
    origin_x, origin_y = data["origin"]
    depth = data["depth_cm"].astype(np.float32)
    x_low, x_high = int(center_x - half - origin_x), int(center_x + half - origin_x)
    y_low, y_high = int(center_y - half - origin_y), int(center_y + half - origin_y)
    x_low, y_low = max(x_low, 0), max(y_low, 0)
    window = (slice(y_low, y_high), slice(x_low, x_high))
    extent = (x_low + origin_x - 0.5, x_high + origin_x - 0.5, y_high + origin_y - 0.5, y_low + origin_y - 0.5)
    figure, axis = plt.subplots(figsize=(10, 10), dpi=100)
    base = np.zeros(depth[window].shape + (3,))
    base[...] = 0.55
    base[data["paved"][window]] = 0.8
    base[data["road"][window]] = 0.35
    axis.imshow(base, extent=extent)
    shown = np.ma.masked_less(depth[window], 0.05)
    image = axis.imshow(shown, extent=extent, cmap="YlOrBr", vmin=0, vmax=8, alpha=0.9)
    axis.contour(data["road"][window].astype(float), levels=[0.5], extent=extent, colors="k", linewidths=0.6, origin="upper")
    for mask, colour in ((data["building"], "#5a3a2a"), (data["hedge"], "#1f6f2f")):
        axis.imshow(np.ma.masked_equal(mask[window].astype(float), 0), extent=extent, cmap=matplotlib.colors.ListedColormap([colour]))
    for x, y, crown, is_shrub in data["trees"]:
        if abs(x - center_x) < half and abs(y - center_y) < half:
            axis.add_patch(plt.Circle((x, y), crown / 2, fill=False, color="#2a8a2a" if not is_shrub else "#7a2", linewidth=0.7))
    axis.set_xlim(center_x - half, center_x + half)
    axis.set_ylim(center_y + half, center_y - half)
    axis.set_aspect("equal")
    figure.colorbar(image, ax=axis, shrink=0.7, label="pile depth at peak leaf fall, cm")
    axis.set_title(f"settled leaves around ({center_x:.0f}, {center_y:.0f}); grey road, light pavement, brown buildings, green hedges")
    figure.savefig(path, bbox_inches="tight")
    plt.close(figure)


def main():
    """Command line entry point."""
    region, out_dir = sys.argv[1], sys.argv[2]
    data = np.load(os.path.join(data_root.world_dir(region), "leaves_debug.npz"))
    os.makedirs(out_dir, exist_ok=True)
    for index, window in enumerate(sys.argv[3:]):
        center_x, center_y, half = (float(v) for v in window.split(","))
        draw_window(data, center_x, center_y, half, os.path.join(out_dir, f"field_{index}.png"))


if __name__ == "__main__":
    main()
