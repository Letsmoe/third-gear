"""Counts placement conflicts in the compiled vegetation of a region.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/check_vegetation.py <region> [--world-name <folder>] [--worst N]

Reads the plants from the region's .tgtile files and the building footprints and pavement from its cache.pkl (so it
checks what the game loads). Reports trunks inside buildings, crowns reaching more than 1 m into buildings,
overlapping tree pairs (one trunk inside 0.6 of the other's crown radius), trunks on the walkable pavement (further
than the kerb strip from the carriageway) and trees per source (from world.json when the build recorded it).
Shrubs are not trees here. --worst prints world positions of the worst cases for screenshots.
"""
import argparse
import json
import os
import pickle
import struct
import sys
import zlib

import numpy as np
import shapely
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402

CROWN_INTO_BUILDING_LIMIT = 1.0  # m


def read_sections(path):
    """Decompressed sections {tag: bytes} of a .tgtile file."""
    with open(path, "rb") as f:
        data = f.read()
    x0, y0 = struct.unpack_from("<dd", data, 8)
    count = struct.unpack_from("<I", data, 32)[0]
    offset = 36
    sections = {}
    for _ in range(count):
        tag = data[offset:offset + 4].decode()
        raw_size, packed_size = struct.unpack_from("<II", data, offset + 4)
        offset += 12
        sections[tag] = zlib.decompress(data[offset:offset + packed_size])
        offset += packed_size
    return x0, y0, sections


def read_plants(world_dir):
    """All plants of the region as arrays: model name, x, y, crown, height."""
    models, rows = [], []
    for name in sorted(os.listdir(world_dir)):
        if not name.endswith(".tgtile"):
            continue
        x0, y0, sections = read_sections(os.path.join(world_dir, name))
        names_raw = sections["NAME"]
        count = struct.unpack_from("<I", names_raw, 0)[0]
        offset, names = 4, []
        for _ in range(count):
            length = struct.unpack_from("<H", names_raw, offset)[0]
            names.append(names_raw[offset + 2:offset + 2 + length].decode())
            offset += 2 + length
        vege = sections["VEGE"]
        plant_count = struct.unpack_from("<I", vege, 0)[0]
        for index in range(plant_count):
            model, x, y, z, yaw, crown, height, trunk = struct.unpack_from("<H2x7f", vege, 4 + index * 32)
            models.append(names[model])
            rows.append((x + x0, y + y0, crown, height))
    return np.array(models), np.array(rows)


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region")
    parser.add_argument("--world-name", help="folder under world/ with the tiles (default: the region)")
    parser.add_argument("--worst", type=int, default=5)
    args = parser.parse_args()
    world_dir = data_root.world_dir(args.world_name or args.region)
    with open(os.path.join(data_root.world_dir(args.region), "cache.pkl"), "rb") as f:
        data, heights, building_union, net, surfaces, ground = pickle.load(f)
    models, rows = read_plants(world_dir)
    is_tree = models != "shrub"
    trees = rows[is_tree]
    x, y, crown = trees[:, 0], trees[:, 1], trees[:, 2]
    print(f"{args.region}: {len(rows)} plants, {len(trees)} trees, {len(rows) - len(trees)} shrubs")

    parts = shapely.get_parts(building_union)
    index = shapely.STRtree(list(parts))
    points = shapely.points(x, y)
    inside = shapely.contains_xy(building_union, x, y)
    nearest = index.query_nearest(points, return_distance=True, all_matches=False)
    facade_distance = np.full(len(trees), np.inf)
    facade_distance[nearest[0][0]] = nearest[1]
    reach = crown / 2 - facade_distance
    crown_in = (~inside) & (reach > CROWN_INTO_BUILDING_LIMIT)

    pairs = cKDTree(np.column_stack([x, y])).query_pairs(0.6 * crown.max() / 2 + 1e-6, output_type="ndarray")
    radius = crown / 2
    distance = np.hypot(x[pairs[:, 0]] - x[pairs[:, 1]], y[pairs[:, 0]] - y[pairs[:, 1]]) if len(pairs) else np.array([])
    needed = 0.6 * np.maximum(radius[pairs[:, 0]], radius[pairs[:, 1]]) if len(pairs) else np.array([])
    overlap = distance < needed
    overlap_pairs = pairs[overlap]

    on_walkable = shapely.contains_xy(net.pavement, x, y)

    print(f"trunks inside buildings:                {int(inside.sum())}")
    print(f"crowns reaching >1 m into buildings:    {int(crown_in.sum())}")
    print(f"overlapping tree pairs:                 {len(overlap_pairs)}")
    print(f"trunks on the pavement:                 {int(on_walkable.sum())}")
    world_json = os.path.join(world_dir, "world.json")
    sources = json.load(open(world_json)).get("plants_by_source") if os.path.exists(world_json) else None
    print(f"trees per source: {sources if sources else 'not recorded'}")

    def show(title, mask_indices, key):
        print(f"worst {title}:")
        for i in mask_indices[np.argsort(-key[mask_indices])][:args.worst]:
            print(f"  x {x[i]:.1f} y {y[i]:.1f} crown {crown[i]:.1f}")
    show("trunks in buildings", np.flatnonzero(inside), crown)
    show("crowns into buildings", np.flatnonzero(crown_in), reach)
    show("trunks on pavement", np.flatnonzero(on_walkable), crown)
    print("worst overlaps:")
    order = np.argsort(distance[overlap] / needed[overlap])[:args.worst]
    for a, b in overlap_pairs[order]:
        print(f"  x {(x[a] + x[b]) / 2:.1f} y {(y[a] + y[b]) / 2:.1f} crowns {crown[a]:.1f} {crown[b]:.1f}")


if __name__ == "__main__":
    main()
