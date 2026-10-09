"""Counts how many Bergedorf buildings carry the tags the typology rules (#81) could use, with value distributions,
footprint area bands and terrace contact.

    Tools/osmimport/.venv/bin/python -I Tools/buildingkit/osm_tag_stats.py [region]

Reads <data root>/world/<region>/cache.pkl, the parsed OSM data of the world compiler.
"""
import os
import pickle
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "osmimport"))

DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
TAGS = ["building", "building:levels", "roof:shape", "height", "building:material", "roof:levels", "roof:material",
        "roof:colour", "building:colour", "building:use", "amenity", "shop", "addr:housenumber", "name",
        "start_date", "building:architecture", "roof:height", "roof:orientation", "building:min_level", "man_made"]


def count_touching(buildings):
    """Returns how many buildings share a wall (lie within 0.1 m of another footprint along at least 3 m) with a neighbour."""
    from shapely.strtree import STRtree
    geometries = [geometry for _, _, geometry in buildings]
    tree = STRtree(geometries)
    touching = 0
    for index, geometry in enumerate(geometries):
        for other in tree.query(geometry.buffer(0.1)):
            if other != index and geometry.buffer(0.1).intersection(geometries[other]).area > 0.3:
                touching += 1
                break
    return touching


def main():
    region = sys.argv[1] if len(sys.argv) > 1 else "bergedorf_core"
    with open(os.path.join(DATA_ROOT, "world", region, "cache.pkl"), "rb") as handle:
        buildings = pickle.load(handle)[0].buildings
    total = len(buildings)
    print(f"{region}: {total} buildings")
    for tag in TAGS:
        counter = Counter(tags[tag] for _, tags, _ in buildings if tag in tags)
        present = sum(counter.values())
        print(f"\n{tag}: {present} ({100.0 * present / total:.1f} %)")
        for value, number in counter.most_common(25 if tag != "name" else 0):
            print(f"  {value}: {number}")
    bands = [(0, 25), (25, 40), (40, 70), (70, 120), (120, 250), (250, 600), (600, 1500), (1500, 1e9)]
    print("\nfootprint area (m2):")
    for low, high in bands:
        number = sum(1 for _, _, g in buildings if low <= g.area < high)
        print(f"  {low}-{high}: {number} ({100.0 * number / total:.1f} %)")
    touching = count_touching(buildings)
    print(f"\nsharing a wall with a neighbour: {touching} ({100.0 * touching / total:.1f} %)")


if __name__ == "__main__":
    main()
