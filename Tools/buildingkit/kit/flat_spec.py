"""Writes a family's spec as flat text for the world compiler (WorldBuilder/Source/World/KitSpecs.cpp), which has no
JSON reader: one line per value, the key path joined by dots, then the value.

    pylons.Donau_Suspension.sockets.0.name Arm1_L1
    pylons.Donau_Suspension.sockets.0.position -3.8 0.0 19.37

Lists of numbers stay on one line; lists of anything else are numbered. Run on its own, it rewrites
kit_spec_<family>.txt for every kit_stats_<family>.json in a building_kit folder:

    python3 -I Tools/buildingkit/kit/flat_spec.py <data root>/building_kit
"""
import glob
import json
import os
import sys


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def flat_lines(prefix, value):
    """The lines of one value under a key path."""
    if isinstance(value, dict):
        lines = []
        for key, item in value.items():
            lines.extend(flat_lines("%s.%s" % (prefix, key), item))
        return lines
    if isinstance(value, list) and value and all(is_number(item) for item in value):
        return ["%s %s" % (prefix, " ".join(repr(float(item)) for item in value))]
    if isinstance(value, list):
        lines = []
        for index, item in enumerate(value):
            lines.extend(flat_lines("%s.%d" % (prefix, index), item))
        return lines
    if isinstance(value, bool):
        return ["%s %d" % (prefix, int(value))]
    return ["%s %s" % (prefix, value)]


def write(path, family, spec):
    """Writes one family's spec to path."""
    with open(path, "w") as handle:
        handle.write("\n".join(flat_lines(family, spec)) + "\n")


def main():
    root = sys.argv[1]
    for stats_path in sorted(glob.glob(os.path.join(root, "kit_stats_*.json"))):
        with open(stats_path) as handle:
            stats = json.load(handle)
        for family, content in stats.items():
            if "spec" in content:
                write(os.path.join(root, "kit_spec_%s.txt" % family), family, content["spec"])


if __name__ == "__main__":
    main()
