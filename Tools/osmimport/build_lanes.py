"""Builds the lane graph AI traffic drives on (see osmimport/lanes.py) and writes it beside the world tiles.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/build_lanes.py <region> [--preview out.png]

It reads the region's cached OSM, DEM and road step (cache.pkl, written by build_world.py), derives the signal approaches
and signs the same way build_world.py does, so approach ids match traffic.json, and writes lanes.json next to
world.json. The tiles are not touched.
"""
import argparse
import json
import os
import pickle
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from osmimport import furniture, geo, lanes  # noqa: E402


def log(message, start=[time.time()]):
    """Prints a message with the seconds since the start."""
    print(f"[{time.time() - start[0]:6.1f}s] {message}", flush=True)


def check_against_traffic(builder, traffic_path):
    """Fails when the approach ids derived here differ from the ones in traffic.json."""
    if not os.path.exists(traffic_path):
        log("no traffic.json to compare approach ids with")
        return
    with open(traffic_path) as f:
        traffic = json.load(f)
    expected = [(a["id"], a["way"]) for j in traffic["junctions"] for a in j["approaches"]]
    derived = []
    next_id = 0
    for junction in builder.junctions:
        for approach in junction.approaches:
            derived.append((next_id, approach.way.id))
            next_id += 1
    if expected != derived:
        raise SystemExit(f"approach ids differ from traffic.json ({len(expected)} there, {len(derived)} here): rebuild the world first")


def write_preview(builder, lane_builder, path):
    """Top-down picture of the lanes, coloured by the control at the end of each road lane."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axis = plt.subplots(figsize=(16, 16), dpi=110)
    colors = {"none": "#2a7", "signal": "#d22", "priority": "#27c", "equal": "#c70", "yield": "#a3a", "stop": "#000"}
    for lane in lane_builder.lanes:
        if lane.kind == "road":
            axis.plot(lane.xy[:, 0], lane.xy[:, 1], color=colors.get(lane.control, "#888"), linewidth=0.8 if lane.good else 0.3)
            end, back = lane.xy[-1], lane.xy[-3 if len(lane.xy) > 3 else 0]
            axis.annotate("", xy=end, xytext=back, arrowprops=dict(arrowstyle="->", color=colors.get(lane.control, "#888"), lw=0.6))
        else:
            axis.plot(lane.xy[:, 0], lane.xy[:, 1], color="#777", linewidth=0.4)
    axis.set_aspect("equal")
    axis.invert_yaxis()
    figure.savefig(path, bbox_inches="tight")


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region", choices=sorted(geo.AREAS))
    parser.add_argument("--preview", help="write a top-down picture of the lanes")
    parser.add_argument("--output-name", help="world folder to read and write (default: the region name)")
    args = parser.parse_args()
    area = geo.AREAS[args.region]
    world_dir = data_root.world_dir(args.output_name or area.name)
    with open(os.path.join(world_dir, "cache.pkl"), "rb") as f:
        data, heights, building_union, net, surfaces, ground = pickle.load(f)
    log("loaded cached sources")
    builder = furniture.FurnitureBuilder(data, net, building_union)
    builder.build()
    check_against_traffic(builder, os.path.join(world_dir, "traffic.json"))
    lane_builder = lanes.LaneBuilder(area, net, builder)
    good, total = lane_builder.build()
    roads_count = sum(1 for lane in lane_builder.lanes if lane.kind == "road")
    log(f"{roads_count} road lanes, {total - roads_count} connections, {good} lanes in the main loop")
    with open(os.path.join(world_dir, "lanes.json"), "w") as f:
        json.dump(lane_builder.to_dict(), f, separators=(",", ":"))
    log(f"wrote {os.path.getsize(os.path.join(world_dir, 'lanes.json')) / 1e6:.2f} MB to {world_dir}/lanes.json")
    if args.preview:
        write_preview(builder, lane_builder, args.preview)


if __name__ == "__main__":
    main()
