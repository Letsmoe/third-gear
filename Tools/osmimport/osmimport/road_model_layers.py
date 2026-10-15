"""The road model of a region (`worldbuilder road-model <region>`, issue #113) as layers for the street viewer.

Each chain's smooth centreline is drawn in pieces coloured by the curve radius. The pieces name their chain, whose
height profile (ground, the road height the tiles use today, the designed height) is sent once beside the layers for
the page to plot.
"""
import json
import os

import shapely

from . import street_layers

# Curve radius classes, upper bound in metres, with the style the page colours them by.
RADIUS_STYLES = [(15.0, "road_model_tight"), (50.0, "road_model_bend"), (200.0, "road_model_curve"),
                 (float("inf"), "road_model_straight")]


def radius_style(radius: float) -> str:
    """The style of a centreline piece with this curve radius."""
    for limit, style in RADIUS_STYLES:
        if radius < limit:
            return style
    return RADIUS_STYLES[-1][1]


def radius_runs(points: list, radii: list):
    """(style, points) for every run of samples whose radius falls in one class, each run sharing its end point with
    the next."""
    runs = []
    start = 0
    for index in range(1, len(points) + 1):
        if index < len(points) and radius_style(radii[index]) == radius_style(radii[start]):
            continue
        piece = points[start:min(index + 1, len(points))]
        if len(piece) >= 2:
            runs.append((radius_style(radii[start]), piece))
        start = index
    return runs


def grades_percent(along: list, heights: list) -> list:
    """The grade of every step between samples, in percent."""
    return [round(100.0 * (heights[i + 1] - heights[i]) / max(along[i + 1] - along[i], 0.05), 2)
            for i in range(len(heights) - 1)]


def design_text(design: dict) -> str:
    """The chain's design values in one line for the pop-up."""
    return (f"plan smoothing {design['plan_smoothing']} m, deviation ≤ {design['max_deviation']} m, "
            f"profile smoothing {design['profile_smoothing']} m, grade ≤ {100 * design['max_grade']:.0f} %")


def chain_properties(chain: dict, chain_index: int) -> dict:
    """Pop-up properties of a chain: its design values and measures, and its index into the profiles."""
    heights, ground, today = chain["height"], chain["ground"], chain["today"]
    grades = grades_percent(chain["along"], heights)
    return {
        "kind": "road model", "osm_id": "", "tags": {}, "chain": chain_index,
        "derived": {
            "class": chain["class"], "name": chain["name"], "length_m": round(chain["along"][-1], 1),
            "max_deviation_m": chain["max_deviation"], "min_radius_m": round(min(chain["radius"]), 1),
            "max_grade_pct": round(max((abs(g) for g in grades), default=0.0), 1),
            "max_off_ground_m": round(max(abs(h - g) for h, g in zip(heights, ground)), 2),
            "max_off_today_m": round(max(abs(h - t) for h, t in zip(heights, today)), 2),
            "design": design_text(chain["design"]),
        },
    }


def chain_profile(chain: dict, lonlat_points: list) -> dict:
    """The arrays the page plots for a chain, with the samples' positions so a click can be marked on the plot."""
    return {"along": chain["along"], "ground": chain["ground"], "today": chain["today"], "height": chain["height"],
            "under_bridge": chain["under_bridge"], "lonlat": lonlat_points, "nodes": chain["node_samples"]}


def build(region: str, world_dir: str, area) -> dict:
    """{"layers": {layer name: GeoJSON FeatureCollection}, "profiles": [one per chain]} of the region's road model,
    with both empty when it hasn't been written."""
    path = os.path.join(world_dir, region, "road_model.json")
    if not os.path.exists(path):
        return {"layers": {}, "profiles": []}
    with open(path) as file:
        model = json.load(file)
    layers = street_layers.Layers()
    profiles = []
    for chain in model["chains"]:
        if len(chain["points"]) < 2:
            continue
        line = street_layers.world_to_lonlat(area, shapely.LineString(chain["points"]))
        properties = chain_properties(chain, len(profiles))
        profiles.append(chain_profile(chain, [[round(x, 7), round(y, 7)] for x, y in line.coords]))
        for style, piece in radius_runs(chain["points"], chain["radius"]):
            layers.add("Road model", shapely.LineString(piece), style, properties)
    return {"layers": street_layers.to_geojson(layers, area), "profiles": profiles}
