"""Classifies the buildings of a region and reports what came out (debug aid for osmimport/building_types.py).

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/classify_buildings.py <region> [out_dir]

Reads the region's cached OSM data (world/<region>/cache.pkl, written by build_world.py), prints the class counts
against the typology's estimates, the roof shape and storey statistics, how well the rules reproduce the tags that
are present (each tagged value is hidden, inferred, and compared), and draws a top-down image coloured by class
with the ridge directions and one coloured by roof shape.
"""
import argparse
import collections
import copy
import os
import pickle
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from build_world import building_footprints  # noqa: E402
from osmimport import building_types as bt  # noqa: E402

# Share of the buildings of the core estimated in typology.md, for a plausibility check only.
TYPOLOGY_SHARES = {
    "gruenderzeit_clinker": 7, "brick_block_1920s": 3, "postwar_plaster": 12, "slab_block": 3, "terraced": 3,
    "semidetached": 4, "detached_postwar": 16, "villa": 1, "modern": 5, "commercial_groundfloor": 4,
    "vierlande_farmhouse": 0, "shed_garage": 26, "industrial_hall": 2, "public": 2, "retail_centre": 1, "halftimbered_town": 1,
}
CLASS_COLOURS = {
    "gruenderzeit_clinker": "#b5121b", "brick_block_1920s": "#7b2d1e", "postwar_plaster": "#f2c14e",
    "slab_block": "#6a4fb3", "terraced": "#f28c28", "semidetached": "#e7a1c5", "detached_postwar": "#4aa564",
    "villa": "#1b7a3a", "modern": "#2da0d8", "commercial_groundfloor": "#e6194b", "vierlande_farmhouse": "#8b5a2b",
    "shed_garage": "#9a9a9a", "industrial_hall": "#3d3d3d", "public": "#0b3d91", "retail_centre": "#ff00ff", "halftimbered_town": "#00ffff",
}
ROOF_COLOURS = {"flat": "#888", "gabled": "#d35400", "hipped": "#27ae60", "half_hipped": "#16a085",
                "mansard": "#8e44ad", "gambrel": "#c0392b", "pyramidal": "#f1c40f", "skillion": "#2980b9",
                "round": "#ecf0f1"}


def classify(world, strip_tags=()):
    """(footprints, types) for the region's buildings; tags named in strip_tags are removed first (hold-out test)."""
    data = world[0]
    footprints = list(building_footprints(data))
    if strip_tags:
        footprints = [(osm_id, {k: v for k, v in tags.items() if k not in strip_tags}, geometry)
                      for osm_id, tags, geometry in footprints]
    typer = bt.BuildingTyper(footprints, data.roads, data.footways, data.areas)
    return footprints, typer.classify_all()


def print_class_table(types):
    counts = collections.Counter(bt.CLASS_NAMES[t.class_id] for t in types)
    total = len(types)
    print(f"\n{total} buildings\n")
    print(f"{'class':26s} {'count':>6s} {'share':>7s} {'typology':>9s}")
    for name in bt.CLASS_NAMES:
        print(f"{name:26s} {counts[name]:6d} {100 * counts[name] / total:6.1f}% {TYPOLOGY_SHARES[name]:8d}%")
    roofs = collections.Counter(bt.ROOF_NAMES[t.roof_shape] for t in types)
    print("\nroof shapes: " + ", ".join(f"{name} {count}" for name, count in roofs.most_common()))
    storeys = collections.Counter(min(t.storeys, 9) for t in types)
    print("storeys: " + ", ".join(f"{level} {storeys[level]}" for level in sorted(storeys)))
    print("tag bits: class %d, levels %d, roof shape %d, height %d, start_date %d" % tuple(
        sum(1 for t in types if t.tag_bits & bit) for bit in (bt.TAG_CLASS, bt.TAG_LEVELS, bt.TAG_ROOF_SHAPE,
                                                               bt.TAG_HEIGHT, bt.TAG_START_DATE)))


def hold_out_report(world, footprints_tagged, types_tagged):
    """Hide the tags the rules can use, infer without them, and count agreement with the hidden values."""
    footprints, inferred = classify(world, strip_tags=("building:levels", "roof:shape", "roof:levels", "height",
                                                       "roof:angle", "start_date"))
    exact_levels = near_levels = levels_total = 0
    shape_hits = shape_total = flat_hits = flat_total = 0
    pitched_hits = pitched_total = 0
    for (osm_id, tags, _), truth, guess in zip(footprints_tagged, types_tagged, inferred):
        levels = bt._float(tags.get("building:levels"))
        if levels and tags.get("building") not in bt.OUTBUILDING_TAGS:
            levels_total += 1
            exact_levels += int(guess.storeys == round(levels))
            near_levels += int(abs(guess.storeys - levels) <= 1)
        shape = bt.OSM_ROOF_SHAPES.get(tags.get("roof:shape"))
        if shape is None or tags.get("building") in bt.OUTBUILDING_TAGS:
            continue
        shape_total += 1
        guess_shape = bt.ROOF_NAMES[guess.roof_shape]
        shape_hits += int(guess_shape == shape)
        if shape == "flat":
            flat_total += 1
            flat_hits += int(guess_shape == "flat")
        else:
            pitched_total += 1
            pitched_hits += int(guess_shape not in ("flat", "skillion"))
    print("\nhold-out check (tags hidden, then inferred) on buildings that carry the tag, outbuildings excluded:")
    print(f"  storeys: {levels_total} buildings, exact {100 * exact_levels / max(levels_total, 1):.0f}%, "
          f"within one {100 * near_levels / max(levels_total, 1):.0f}%")
    print(f"  roof shape: {shape_total} buildings, exact shape {100 * shape_hits / max(shape_total, 1):.0f}%, "
          f"pitched vs flat right for {100 * (pitched_hits + flat_hits) / max(shape_total, 1):.0f}%")
    print(f"  of the tagged flat roofs {100 * flat_hits / max(flat_total, 1):.0f}% inferred flat, of the tagged "
          f"pitched roofs {100 * pitched_hits / max(pitched_total, 1):.0f}% inferred pitched")


def draw_maps(world, footprints, types, out_dir, region):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from shapely.plotting import plot_polygon
    for kind in ("class", "roof"):
        fig, ax = plt.subplots(figsize=(16, 16), dpi=110)
        for (osm_id, tags, polygon), t in zip(footprints, types):
            name = bt.CLASS_NAMES[t.class_id] if kind == "class" else bt.ROOF_NAMES[t.roof_shape]
            colour = (CLASS_COLOURS if kind == "class" else ROOF_COLOURS)[name]
            plot_polygon(polygon, ax=ax, add_points=False, facecolor=colour, edgecolor="#222", linewidth=0.2)
            if kind == "roof" and t.roof_shape != bt.ROOF_ID["flat"]:
                centre = np.array(polygon.centroid.coords[0])
                angle = np.radians(t.ridge_yaw)
                half = min(polygon.minimum_rotated_rectangle.length / 8, 12)
                direction = np.array([np.cos(angle), np.sin(angle)]) * half
                ax.plot(*zip(centre - direction, centre + direction), color="black", linewidth=0.5)
        colours = CLASS_COLOURS if kind == "class" else ROOF_COLOURS
        ax.legend(handles=[Line2D([0], [0], marker="s", color="w", markerfacecolor=c, markersize=11, label=n)
                           for n, c in colours.items()], loc="upper right", fontsize=11)
        for street in world[0].roads:
            ax.plot(street.xy[:, 0], street.xy[:, 1], color="#bbb", linewidth=0.6, zorder=0)
        area_bounds = bt_bounds(world)
        ax.set_xlim(area_bounds[0], area_bounds[1])
        ax.set_ylim(area_bounds[3], area_bounds[2])
        ax.set_aspect("equal")
        ax.set_title(f"{region}: buildings by {kind} (x east, y south, metres)")
        path = os.path.join(out_dir, f"{region}_by_{kind}.png")
        fig.savefig(path, bbox_inches="tight")
        plt.close(fig)
        print("wrote", path)


def bt_bounds(world):
    from osmimport import geo
    area = geo.AREAS[world[-1]] if isinstance(world[-1], str) else None
    return (area.x_min, area.x_max, area.y_min, area.y_max)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region")
    parser.add_argument("out_dir", nargs="?", default="/tmp/building_types")
    parser.add_argument("--no-hold-out", action="store_true")
    args = parser.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(data_root.world_dir(args.region), "cache.pkl"), "rb") as f:
        world = tuple(pickle.load(f)) + (args.region,)
    footprints, types = classify(world)
    print_class_table(types)
    if not args.no_hold_out:
        hold_out_report(world, footprints, types)
    draw_maps(world, footprints, types, args.out_dir, args.region)


if __name__ == "__main__":
    main()
