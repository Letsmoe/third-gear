"""Compares the building typing (osmimport/building_types.py) with Hamburg's LoD2 city model, as a check only.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/check_lod2.py <region> <lod2_dir>

<lod2_dir> holds the CityGML tiles (LoD2_32_<E km>_<N km>_1_HH.gml, unpacked from the open data zip, dl-de/by-2.0)
that cover the region. Every LoD2 building is matched to the OSM footprint it overlaps most (at least half of the
union), then the roof type, the storey count and the total height of LoD2 are compared with what the rules infer
without any of the optional tags (building:levels, roof:shape, roof:levels, height, roof:angle, start_date), which is
the situation in most of the country, and with all tags. The runtime never reads LoD2.
"""
import argparse
import collections
import glob
import math
import os
import pickle
import re
import sys
import xml.etree.ElementTree as ElementTree

import numpy as np
import shapely
from shapely.strtree import STRtree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from classify_buildings import classify  # noqa: E402
from osmimport import building_types as bt  # noqa: E402
from osmimport import geo  # noqa: E402

NAMESPACES = {"bldg": "http://www.opengis.net/citygml/building/1.0", "gml": "http://www.opengis.net/gml"}
# ADV roof type codes of the Hamburg LoD2 model
LOD2_ROOF = {"1000": "flat", "2100": "skillion", "2200": "skillion", "3100": "gabled", "3200": "hipped",
             "3300": "half_hipped", "3400": "mansard", "3500": "pyramidal", "3600": "pyramidal", "3700": "round",
             "3800": "gabled", "3900": "round", "4000": "pyramidal", "5000": "gabled"}
ROOF_GROUPS = {"flat": "flat", "skillion": "flat", "gabled": "gabled", "gambrel": "gabled", "mansard": "gabled",
               "hipped": "hipped", "half_hipped": "hipped", "pyramidal": "hipped", "round": "flat"}
TAG_STRIP = ("building:levels", "roof:shape", "roof:levels", "height", "roof:angle", "start_date")


def read_lod2(directory, area):
    """List of dicts (footprint in world metres, roof, height, storeys) for every LoD2 building in the tiles."""
    cache = os.path.join(directory, "summary.pkl")
    if os.path.exists(cache):
        with open(cache, "rb") as f:
            return pickle.load(f)
    result = []
    for path in sorted(glob.glob(os.path.join(directory, "*.gml"))):
        for _, element in ElementTree.iterparse(path, events=("end",)):
            if element.tag != "{%s}Building" % NAMESPACES["bldg"]:
                continue
            record = _building_record(element, area)
            if record:
                result.append(record)
            element.clear()
    with open(cache, "wb") as f:
        pickle.dump(result, f)
    return result


def _building_record(element, area):
    roof = element.findtext("bldg:roofType", namespaces=NAMESPACES)
    height = element.findtext("bldg:measuredHeight", namespaces=NAMESPACES)
    storeys = element.findtext("bldg:storeysAboveGround", namespaces=NAMESPACES)
    ground = element.find(".//bldg:GroundSurface//gml:posList", NAMESPACES)
    if roof is None or height is None or ground is None:
        return None
    values = np.array(ground.text.split(), dtype=np.float64).reshape(-1, 3)
    x = values[:, 0] - area.origin_e
    y = -(values[:, 1] - area.origin_n)
    polygon = shapely.Polygon(np.column_stack([x, y]))
    if not polygon.is_valid:
        polygon = shapely.make_valid(polygon)
    return {"footprint": polygon, "roof": LOD2_ROOF.get(roof, "flat"), "roof_code": roof, "height": float(height),
            "storeys": int(storeys) if storeys else None}


def match(lod2, footprints):
    """Pairs (lod2 index, footprint index) with at least 50 % overlap of the union."""
    tree = STRtree([f for _, _, f in footprints])
    pairs = []
    for lod_index, record in enumerate(lod2):
        polygon = record["footprint"]
        if polygon.is_empty or polygon.area < 15:
            continue
        best, best_score = None, 0.5
        for index in tree.query(polygon, predicate="intersects"):
            other = footprints[int(index)][2]
            union = polygon.union(other).area
            score = polygon.intersection(other).area / max(union, 1e-6)
            if score > best_score:
                best, best_score = int(index), score
        if best is not None:
            pairs.append((lod_index, best))
    return pairs


def total_height(building_type, footprint_rectangle_short):
    """Ground to ridge height implied by a BuildingType: eave plus the rise of the roof over half the span."""
    shape = bt.ROOF_NAMES[building_type.roof_shape]
    eave = building_type.eave_height
    if shape == "flat":
        return eave + 0.5
    rise = footprint_rectangle_short / 2.0 * math.tan(math.radians(building_type.pitch_degrees))
    if shape == "skillion":
        return eave + rise * 2
    return eave + min(rise, 8.0)


def report(title, lod2, footprints, types, pairs):
    matrix = collections.defaultdict(collections.Counter)
    storey_errors, height_errors, storey_hits = [], [], 0
    storey_total = 0
    for lod_index, index in pairs:
        record = lod2[lod_index]
        building_type = types[index]
        if bt.CLASS_NAMES[building_type.class_id] == "shed_garage" and record["height"] < 5:
            pass
        truth = ROOF_GROUPS[record["roof"]]
        guess = ROOF_GROUPS[bt.ROOF_NAMES[building_type.roof_shape]]
        matrix[truth][guess] += 1
        if record["storeys"]:
            storey_total += 1
            storey_hits += int(record["storeys"] == building_type.storeys)
            storey_errors.append(building_type.storeys - record["storeys"])
        short = bt._Footprint(footprints[index][2]).short_length
        height_errors.append(total_height(building_type, short) - record["height"])
    print(f"\n{title}: {len(pairs)} buildings matched of {len(lod2)} in LoD2")
    total = sum(sum(row.values()) for row in matrix.values())
    hits = sum(matrix[name][name] for name in matrix)
    print(f"  roof (flat, gabled, hipped) agrees for {100 * hits / total:.0f}%; rows are LoD2, columns the rules")
    names = ["flat", "gabled", "hipped"]
    print("  " + " " * 8 + "".join(f"{name:>9s}" for name in names))
    for truth in names:
        print(f"  {truth:8s}" + "".join(f"{matrix[truth][name]:9d}" for name in names))
    errors = np.array(storey_errors)
    print(f"  storeys: exact {100 * storey_hits / storey_total:.0f}%, within one {100 * np.mean(np.abs(errors) <= 1):.0f}%,"
          f" mean error {errors.mean():+.2f}")
    heights = np.array(height_errors)
    print(f"  total height: median error {np.median(heights):+.1f} m, within 2 m {100 * np.mean(np.abs(heights) <= 2):.0f}%,"
          f" within 3 m {100 * np.mean(np.abs(heights) <= 3):.0f}%")
    return matrix


def priors(lod2, footprints, types, pairs):
    """Per class the LoD2 shares of roof shapes and storey counts (the numbers behind ROOF_PRIORS and STOREY_PRIORS)."""
    roofs = collections.defaultdict(collections.Counter)
    storeys = collections.defaultdict(collections.Counter)
    for lod_index, index in pairs:
        name = bt.CLASS_NAMES[types[index].class_id]
        roofs[name][lod2[lod_index]["roof"]] += 1
        if lod2[lod_index]["storeys"]:
            storeys[name][min(lod2[lod_index]["storeys"], 8)] += 1
    for name in bt.CLASS_NAMES:
        count = sum(roofs[name].values())
        if not count:
            continue
        shares = ", ".join(f"{roof} {100 * n / count:.0f}" for roof, n in roofs[name].most_common())
        levels = ", ".join(f"{level}: {n}" for level, n in sorted(storeys[name].items()))
        print(f"{name:24s} n={count:4d} roof {shares} | storeys {levels}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region")
    parser.add_argument("lod2_dir")
    parser.add_argument("--half", choices=["west", "east", "all"], default="all",
                        help="only buildings with x < 0 (west) or x >= 0 (east); priors come from one half, "
                             "the agreement is reported on the other")
    parser.add_argument("--priors", action="store_true", help="print the LoD2 shares per class instead of checking")
    args = parser.parse_args()
    area = geo.AREAS[args.region]
    lod2 = read_lod2(args.lod2_dir, area)
    with open(os.path.join(data_root.world_dir(args.region), "cache.pkl"), "rb") as f:
        world = tuple(pickle.load(f)) + (args.region,)
    footprints, with_tags = classify(world)
    _, without_tags = classify(world, strip_tags=TAG_STRIP)
    pairs = match(lod2, footprints)
    if args.half != "all":
        keep = (lambda x: x < 0) if args.half == "west" else (lambda x: x >= 0)
        pairs = [(a, b) for a, b in pairs if keep(footprints[b][2].centroid.x)]
    if args.priors:
        priors(lod2, footprints, without_tags, pairs)
        return
    report("Rules with all OSM tags", lod2, footprints, with_tags, pairs)
    report("Rules from geometry and context only", lod2, footprints, without_tags, pairs)


if __name__ == "__main__":
    main()
