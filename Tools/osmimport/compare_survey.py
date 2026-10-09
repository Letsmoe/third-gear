"""Measures the game's generated streets against Hamburg's surveyed street data (issue #91).

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/compare_survey.py [region] [--json out.json]

The streets come from the same code build_world.py runs, in the viewer's 500 m tiles (game_streets.py). Only the
street space the survey maps is compared; shared spaces are left out. It prints:

* carriageway: how much of our road surface lies on the real carriageway (precision), how much of the real
  carriageway we cover (recall), and where the rest of ours lands (parking, footway, cycle track, green);
* width per road class: real kerb-to-kerb width sampled every 10 m across the OSM centre line against ours;
* cycle tracks: how much of the length we draw (OSM tags and separate ways) lies on a real cycle surface;
* signal poles, lamps and signs: how many of ours have a real one nearby (precision) and the other way (recall).
"""
import argparse
import collections
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import numpy as np  # noqa: E402
import shapely  # noqa: E402

import data_root  # noqa: E402
import game_streets  # noqa: E402
from osmimport import geo, hh_survey, roads, street_layers  # noqa: E402
from street_index import WHOLE_EXTRACT, load_whole_extract  # noqa: E402

WIDTH_SAMPLE_SPACING = 10.0
WIDTH_PROBE_HALF_LENGTH = 25.0
# Matching distances for points: ours counts as right when a real one stands this close.
MATCH_DISTANCE = {"signal": 3.0, "lamp": 3.0, "sign": 6.0}
CYCLE_TOLERANCE = 0.75
# Width samples this close to a junction node are skipped: the probe would measure the side road.
JUNCTION_SKIP = 15.0


def log(message, start=[time.time()]):
    """Prints a message with the seconds since the start."""
    print(f"[{time.time() - start[0]:6.1f}s] {message}", flush=True)


def generate_region(index, box):
    """The game's streets in every tile of the box."""
    streets = game_streets.GameStreets()
    for column, row in game_streets.tiles_touching(box):
        streets.extend(game_streets.generate_tile(index, column, row))
    return streets


def carriageway_metrics(streets, survey, box, compared):
    """Area overlap of our road surface with the real carriageway, inside the compared street space."""
    ours = shapely.union_all([polygon for _, polygon in streets.surfaces] + [polygon for _, polygon in streets.bridges])
    ours = shapely.intersection(ours, compared)
    real = shapely.intersection(survey.group_union(box, "carriageway"), compared)
    on_real = shapely.intersection(ours, real).area
    result = {"ours_m2": round(ours.area), "real_m2": round(real.area),
              "precision": round(on_real / max(ours.area, 1.0), 3), "recall": round(on_real / max(real.area, 1.0), 3),
              "iou": round(on_real / max(shapely.union(ours, real).area, 1.0), 3), "ours_lands_on_m2": {}}
    outside = shapely.difference(ours, real)
    for group in ("parking", "footway", "cycle", "green"):
        result["ours_lands_on_m2"][group] = round(shapely.intersection(outside, survey.group_union(box, group)).area)
    return result


def real_width_at(point, direction, carriageway, other_roads):
    """Kerb-to-kerb width of the real carriageway across the road at a point, or None when the point is off it.

    Where another road's centre line crosses the probe (the other half of a dual carriageway without a median, a
    parallel slip lane), the width ends halfway to it: the survey maps both as one surface, divided only by paint."""
    normal = np.array([-direction[1], direction[0]])
    centre = np.array(point)
    probe = shapely.LineString([centre - normal * WIDTH_PROBE_HALF_LENGTH, centre + normal * WIDTH_PROBE_HALF_LENGTH])
    pieces = shapely.intersection(probe, carriageway)
    piece = next((part for part in getattr(pieces, "geoms", [pieces])
                  if isinstance(part, shapely.LineString) and part.distance(shapely.Point(point)) < 0.01), None)
    if piece is None:
        return None
    lateral = [float(np.dot(np.array(coordinate) - centre, normal)) for coordinate in piece.coords]
    low, high = min(lateral), max(lateral)
    for crossing in _crossings(probe, other_roads):
        offset = float(np.dot(np.array(crossing) - centre, normal))
        if 0.5 < offset < high:
            high = offset / 2
        elif low < offset < -0.5:
            low = offset / 2
    return high - low


def _crossings(probe, lines):
    """Points where the probe crosses any of the lines."""
    points = []
    for line in lines:
        hit = shapely.intersection(probe, line)
        for part in getattr(hit, "geoms", [hit]):
            if isinstance(part, shapely.Point):
                points.append((part.x, part.y))
    return points


def _junction_points(data):
    """Nodes where three or more road ends meet."""
    degree = roads.node_degrees(data.roads)
    positions = {}
    for way in data.roads:
        for node, xy in zip(way.node_ids, way.xy):
            positions[node] = xy
    return [positions[node] for node, count in degree.items() if count >= 3 and node in positions]


def width_metrics(data, buildings, survey, box, compared):
    """Real and our width sampled along the OSM centre lines away from junctions, per road class and lane count.
    Ours is the width the game builds, in the same urban or rural context (buildings: their union)."""
    carriageway = shapely.intersection(survey.group_union(box.buffer(30), "carriageway"), box.buffer(30))
    shapely.prepare(carriageway)
    junctions = shapely.union_all(shapely.buffer(shapely.points(np.array(_junction_points(data))), JUNCTION_SKIP))
    shapely.prepare(junctions)
    lines = {way.id: shapely.LineString(way.xy) for way in data.roads}
    line_tree = shapely.STRtree(list(lines.values()))
    line_ids = list(lines.keys())
    samples = collections.defaultdict(list)
    for way in data.roads:
        if roads._is_tunnel(way) or way.tags.get("bridge", "no") != "no":
            continue
        line = lines[way.id]
        section = roads.road_section(way.tags, roads.road_context(way, buildings))
        ours = section.width()
        key = f"{way.tags.get('highway')} {'oneway' if roads.is_oneway(way.tags) else 'twoway'} " \
              f"lanes={section.lane_count()}"
        for distance in np.arange(WIDTH_SAMPLE_SPACING / 2, line.length, WIDTH_SAMPLE_SPACING):
            here = line.interpolate(distance)
            if not compared.contains(here) or junctions.contains(here):
                continue
            ahead = line.interpolate(min(distance + 1.0, line.length))
            behind = line.interpolate(max(distance - 1.0, 0.0))
            direction = np.array([ahead.x - behind.x, ahead.y - behind.y])
            if np.hypot(*direction) < 1e-6:
                continue
            nearby = [lines[line_ids[index]] for index in line_tree.query(here.buffer(WIDTH_PROBE_HALF_LENGTH))
                      if line_ids[index] != way.id]
            real = real_width_at((here.x, here.y), direction / np.hypot(*direction), carriageway, nearby)
            if real is not None and real < 2 * WIDTH_PROBE_HALF_LENGTH - 0.5:
                samples[key].append((real, ours))
    samples["all roads"] = [pair for pairs in list(samples.values()) for pair in pairs]
    result = {}
    for key, pairs in sorted(samples.items(), key=lambda item: -len(item[1])):
        real, ours = np.array(pairs).T
        error = ours - real
        result[key] = {"samples": len(pairs), "real_median_m": round(float(np.median(real)), 2),
                       "real_p10_p90_m": [round(float(np.percentile(real, 10)), 2),
                                          round(float(np.percentile(real, 90)), 2)],
                       "ours_median_m": round(float(np.median(ours)), 2),
                       "error_median_m": round(float(np.median(error)), 2),
                       "error_abs_mean_m": round(float(np.mean(np.abs(error))), 2)}
    return result


def our_cycle_lines(data):
    """The cycle tracks and lanes we draw: tagged on roads (street_layers' offsets) and separately mapped ways."""
    layers = street_layers.Layers()
    for way in data.roads:
        street_layers.add_road(layers, way)
    for way in data.footways:
        street_layers.add_path(layers, way)
    return [geometry for geometry, style, _ in layers.by_name.get("Cycling", [])
            if style in {"cycle_lane", "cycle_track", "cycleway"}]


def cycle_metrics(data, survey, box, compared):
    """How much of our cycle line length lies on a real cycle surface."""
    real = shapely.intersection(survey.group_union(box, "cycle"), compared).buffer(CYCLE_TOLERANCE)
    lines = [shapely.intersection(line, compared) for line in our_cycle_lines(data)]
    total = sum(line.length for line in lines)
    on_real = sum(shapely.intersection(line, real).length for line in lines)
    return {"ours_m": round(total), "on_real_cycle_surface": round(on_real / max(total, 1.0), 3)}


def match_points(ours, real, distance):
    """(precision, recall) of two point lists [(x, y)] matched within a distance."""
    if not ours or not real:
        return 0.0, 0.0
    ours_points = shapely.points(np.array(ours))
    real_points = shapely.points(np.array(real))
    real_tree = shapely.STRtree(real_points)
    ours_tree = shapely.STRtree(ours_points)
    ours_hit = sum(len(real_tree.query(point, predicate="dwithin", distance=distance)) > 0 for point in ours_points)
    real_hit = sum(len(ours_tree.query(point, predicate="dwithin", distance=distance)) > 0 for point in real_points)
    return round(ours_hit / len(ours), 3), round(real_hit / len(real), 3)


def point_metrics(streets, survey, box, compared):
    """Signal poles, lamps and signs against the survey, inside the compared street space (plus 8 m beside it)."""
    near = compared.buffer(8.0)
    shapely.prepare(near)

    def inside(points):
        return [(x, y) for x, y in points if near.contains(shapely.Point(x, y))]

    ours = {"signal": inside((pole["x"], pole["y"]) for pole in streets.poles if pole["kind"] == "signal_head"),
            "lamp": inside((pole["x"], pole["y"]) for pole in streets.poles if pole["kind"] == "lamp"),
            "sign": inside((pole["x"], pole["y"]) for pole in streets.poles if pole["kind"] == "sign")}
    real = {"signal": inside((x, y) for _, x, y in survey.points_in(box, {"signal"})),
            "lamp": inside((x, y) for _, x, y in survey.points_in(box, {"lamp"})),
            "sign": inside({(x, y) for _, x, y, _, _ in survey.signs_in(box)})}
    result = {}
    for kind in ("signal", "lamp", "sign"):
        precision, recall = match_points(ours[kind], real[kind], MATCH_DISTANCE[kind])
        result[kind] = {"ours": len(ours[kind]), "real": len(real[kind]), "precision": precision, "recall": recall,
                        "match_m": MATCH_DISTANCE[kind]}
    return result


def print_report(report):
    """The metrics as readable tables."""
    carriageway = report["carriageway"]
    print(f"\nCarriageway ({carriageway['real_m2']} m² real, {carriageway['ours_m2']} m² ours)")
    print(f"  precision {carriageway['precision']:.0%}  recall {carriageway['recall']:.0%}  IoU {carriageway['iou']:.0%}")
    print("  ours off the carriageway lands on: " +
          ", ".join(f"{group} {area} m²" for group, area in carriageway["ours_lands_on_m2"].items()))
    print(f"\nWidth per road class (ours minus real, metres)")
    print(f"  {'class':42s} {'n':>5s} {'real':>6s} {'p10-p90':>12s} {'ours':>6s} {'error':>6s} {'|err|':>6s}")
    for key, row in report["width"].items():
        if row["samples"] < 10:
            continue
        low, high = row["real_p10_p90_m"]
        print(f"  {key:42s} {row['samples']:5d} {row['real_median_m']:6.2f} {low:5.1f}-{high:<6.1f} "
              f"{row['ours_median_m']:6.2f} {row['error_median_m']:+6.2f} {row['error_abs_mean_m']:6.2f}")
    cycle = report["cycle"]
    print(f"\nCycle tracks and lanes: {cycle['ours_m']} m drawn, {cycle['on_real_cycle_surface']:.0%} on a real cycle surface")
    print(f"\n{'Points':10s} {'ours':>6s} {'real':>6s} {'precision':>10s} {'recall':>8s}")
    for kind, row in report["points"].items():
        print(f"{kind + ' (' + str(row['match_m']) + ' m)':10s} {row['ours']:6d} {row['real']:6d} "
              f"{row['precision']:10.0%} {row['recall']:8.0%}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region", nargs="?", default="bergedorf_core", choices=sorted(geo.AREAS))
    parser.add_argument("--json", help="also write the metrics to this file")
    args = parser.parse_args()
    area = geo.AREAS[args.region]
    box = shapely.box(area.x_min, area.y_min, area.x_max, area.y_max)
    survey = hh_survey.load(data_root.geodata_dir(), WHOLE_EXTRACT)
    if survey is None:
        raise SystemExit("No survey data: run Tools/geodata/fetch_hh_street_survey.py first.")
    log("survey loaded")
    index = load_whole_extract(type_buildings=False)
    log("OSM loaded")
    streets = generate_region(index, box)
    log("game streets generated")
    data = index.subset(box)
    buildings = shapely.union_all([footprint for _, _, footprint, _ in index.buildings_in(box.buffer(50))])
    compared = shapely.difference(survey.coverage(box), survey.group_union(box, "shared"))
    shapely.prepare(compared)
    report = {"region": args.region,
              "carriageway": carriageway_metrics(streets, survey, box, compared),
              "width": width_metrics(data, buildings, survey, box, compared),
              "cycle": cycle_metrics(data, survey, box, compared),
              "points": point_metrics(streets, survey, box, compared)}
    log("compared")
    print_report(report)
    if args.json:
        with open(args.json, "w") as out:
            json.dump(report, out, indent=1)


if __name__ == "__main__":
    main()
