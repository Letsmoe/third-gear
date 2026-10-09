"""Lists, per signalised junction, its approaches (signals and stop lines) and flags stop lines that follow each other.

Usage: check_signals.py <region> [--verbose]

Two approaches count as stacked when their stop lines are closer than STACK_DISTANCE along the travel direction and point the same way within
about 25 degrees and on the same line within 4 m: a driver would meet two signal groups in a row.
"""
import argparse
import math
import os
import pickle
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from osmimport import furniture, geo  # noqa: E402

STACK_DISTANCE = 25.0
STACK_COS = 0.9
STACK_ACROSS = 4.0


def stacked_pairs(junction_list):
    """Pairs of approaches (over all junctions) whose stop lines follow one another in the same direction."""
    entries = [(j.id, a) for j in junction_list for a in j.approaches]
    pairs = []
    for i, (ja, a) in enumerate(entries):
        for jb, b in entries[i + 1:]:
            distance = math.hypot(a.stop_xy[0] - b.stop_xy[0], a.stop_xy[1] - b.stop_xy[1])
            if distance > STACK_DISTANCE:
                continue
            cosine = a.direction[0] * b.direction[0] + a.direction[1] * b.direction[1]
            if cosine < STACK_COS:
                continue
            offset = (b.stop_xy[0] - a.stop_xy[0], b.stop_xy[1] - a.stop_xy[1])
            along = offset[0] * a.direction[0] + offset[1] * a.direction[1]
            across = abs(offset[0] * a.direction[1] - offset[1] * a.direction[0])
            if across > STACK_ACROSS or not 0.0 <= along <= STACK_DISTANCE:
                continue
            pairs.append((ja, a, jb, b, along))
    return pairs


def main():
    """Prints the counts and the stacked pairs for one region."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("region", choices=sorted(geo.AREAS))
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    world_dir = data_root.world_dir(args.region)
    with open(os.path.join(world_dir, "cache.pkl"), "rb") as f:
        data, heights, building_union, net, surfaces, ground = pickle.load(f)
    builder = furniture.FurnitureBuilder(data, net, building_union)
    builder.build()
    junction_list = builder.junctions
    approach_count = sum(len(j.approaches) for j in junction_list)
    print(f"{args.region}: {len(junction_list)} junctions, {approach_count} approaches (signal + stop line each), "
          f"{len(builder.heads)} signal poles")
    for j in junction_list:
        if args.verbose:
            print(f"junction {j.id} at ({j.x:.0f}, {j.y:.0f}) crossing_only={j.crossing_only}")
            for a in j.approaches:
                print(f"   way {a.way.id} travel {a.travel:+d} explicit={a.explicit} stop=({a.stop_xy[0]:.0f}, "
                      f"{a.stop_xy[1]:.0f}) phase {a.phase}")
    pairs = stacked_pairs(junction_list)
    close = sum(1 for pair in pairs if pair[4] <= 12.0)
    print(f"{len(pairs)} stacked stop line pairs within {STACK_DISTANCE:.0f} m, {close} of them within 12 m")
    for ja, a, jb, b, along in pairs:
        print(f"   junction {ja} way {a.way.id} travel {a.travel:+d} explicit={a.explicit} at "
              f"({a.stop_xy[0]:.0f}, {a.stop_xy[1]:.0f})  and  junction {jb} way {b.way.id} travel {b.travel:+d} "
              f"explicit={b.explicit} at ({b.stop_xy[0]:.0f}, {b.stop_xy[1]:.0f})  {along:.0f} m ahead")


if __name__ == "__main__":
    main()
