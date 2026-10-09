"""Draws the streets as OSM tags them over the Hamburg aerial photo, in an interactive window for review.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/street_view.py <region> [--centre x,y --size m] [--png out.png]

Carriageway edges come from the width and lane tags (yellow dashed), lane dividers are cyan, cycle lanes and tracks
tagged on the road are blue beside it, separate cycleways and footways are their own lines, railways are black and white, bridges are
hatched, and signals, crossings,
stop and give way signs and other traffic signs are markers. Click anything to see its OSM tags.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import matplotlib  # noqa: E402
import numpy as np  # noqa: E402
import shapely  # noqa: E402

import data_root  # noqa: E402
from osmimport import aerial, geo, osm, roads  # noqa: E402

CARRIAGEWAY_COLOUR = "#ffd400"
LANE_COLOUR = "#3ff"
CYCLE_COLOUR = "#2f7bff"
FOOT_COLOUR = "#ddd"
CROSSING_COLOUR = "#ff3fd5"
RAIL_COLOUR = "#111"
BRIDGE_COLOUR = "#9be"
RAIL_TRACK_WIDTH = 3.0
CYCLE_LANE_INSET = 0.8
CYCLE_TRACK_OFFSET = 1.5
POINT_STYLES = {
    "traffic_signals": dict(marker="o", color="#ff2020", markersize=8, markeredgecolor="black"),
    "stop": dict(marker="8", color="#ff2020", markersize=9, markeredgecolor="white"),
    "give_way": dict(marker="v", color="white", markersize=9, markeredgecolor="#ff2020"),
    "crossing": dict(marker="s", color=CROSSING_COLOUR, markersize=6, markeredgecolor="black"),
    "street_lamp": dict(marker=".", color="#fff38a", markersize=5),
    "bus_stop": dict(marker="P", color="#2e2", markersize=8, markeredgecolor="black"),
}


def side_offset(line: shapely.LineString, distance: float, side: str) -> shapely.LineString:
    """The line moved sideways by distance metres to the physical left or right of its direction.

    World y points south, so the frame is mirrored and shapely's positive (left) offset lands on the physical right.
    """
    if side == "left":
        return shapely.offset_curve(line, -distance)
    return shapely.offset_curve(line, distance)


def cycleway_tags(tags: dict) -> dict:
    """The cycleway tagged on a road, as {"left": value, "right": value} relative to the way's direction."""
    sides = {}
    both = tags.get("cycleway:both") or tags.get("cycleway")
    if both:
        sides = {"left": both, "right": both}
        if tags.get("cycleway") and roads.is_oneway(tags) and "cycleway:both" not in tags:
            sides = {"right": both}  # a plain cycleway= on a one-way street is on the right
    for side in ("left", "right"):
        value = tags.get(f"cycleway:{side}")
        if value:
            sides[side] = value
    return sides


def lane_divider_offsets(tags: dict, width: float) -> list:
    """Sideways offsets (positive = physical right) of the lines between lanes, from the lane tags."""
    lane_count = roads.road_lanes(tags)
    if lane_count < 2:
        return []
    lane_width = width / lane_count
    return [-width / 2 + lane_width * index for index in range(1, lane_count)]


class StreetView:
    """Collects the drawn features with their OSM tags, so a click can show what is under the cursor."""

    def __init__(self, axes):
        self.axes = axes
        self.features = []  # (shapely geometry, label text)
        self.info = None

    def remember(self, geometry, kind: str, osm_id: int, tags: dict):
        """Keeps a feature for the click lookup."""
        tag_lines = "\n".join(f"{key}={value}" for key, value in sorted(tags.items()))
        self.features.append((geometry, f"{kind} {osm_id}\n{tag_lines}"))

    def line(self, geometry, **style):
        """Draws a (multi) line string."""
        for part in getattr(geometry, "geoms", [geometry]):
            if part.is_empty:
                continue
            coordinates = np.asarray(part.coords)
            self.axes.plot(coordinates[:, 0], coordinates[:, 1], **style)

    def draw_road(self, way):
        """Carriageway edges, centre line, lane dividers, one-way arrow and the cycleways tagged on the road."""
        centre = shapely.LineString(way.xy)
        width = roads.road_width(way.tags)
        self.remember(centre, "road", way.id, way.tags | {"(width used)": f"{width:.2f} m"})
        if is_bridge(way.tags):
            self.draw_bridge(centre, width)
        for side in ("left", "right"):
            self.line(side_offset(centre, width / 2, side), color=CARRIAGEWAY_COLOUR, linewidth=1.0,
                      linestyle=(0, (4, 2)))
        self.line(centre, color="white", linewidth=0.5, alpha=0.6)
        for offset in lane_divider_offsets(way.tags, width):
            self.line(side_offset(centre, abs(offset), "right" if offset > 0 else "left"), color=LANE_COLOUR,
                      linewidth=0.7)
        if roads.is_oneway(way.tags):
            self.oneway_arrow(centre, reverse=way.tags.get("oneway") == "-1")
        for side, value in cycleway_tags(way.tags).items():
            self.draw_road_cycleway(centre, width, side, value)

    def draw_bridge(self, line, width: float):
        """A hatched band under a way that crosses on a bridge, so bridges stand out from the ground."""
        band = shapely.buffer(line, width / 2 + 1.0, cap_style="flat")
        for polygon in getattr(band, "geoms", [band]):
            coordinates = np.asarray(polygon.exterior.coords)
            self.axes.fill(coordinates[:, 0], coordinates[:, 1], facecolor=BRIDGE_COLOUR, alpha=0.35,
                           edgecolor=BRIDGE_COLOUR, hatch="////", linewidth=1.0)

    def draw_railway(self, way):
        """A rail line as a black and white dashed line, with its bridge band where it crosses one."""
        line = shapely.LineString(way.xy)
        self.remember(line, "railway", way.id, way.tags)
        if is_bridge(way.tags):
            self.draw_bridge(line, RAIL_TRACK_WIDTH)
        self.line(line, color="white", linewidth=2.6)
        self.line(line, color=RAIL_COLOUR, linewidth=2.6, linestyle=(0, (3, 3)))

    def draw_road_cycleway(self, centre, width: float, side: str, value: str):
        """A cycle lane inside the carriageway edge, or a track outside it, on one side of a road."""
        if value in {"lane", "opposite_lane"}:
            self.line(side_offset(centre, width / 2 - CYCLE_LANE_INSET, side), color=CYCLE_COLOUR, linewidth=2.0)
        elif value in {"track", "opposite_track"}:
            self.line(side_offset(centre, width / 2 + CYCLE_TRACK_OFFSET, side), color=CYCLE_COLOUR, linewidth=2.0,
                      linestyle=(0, (3, 2)))
        elif value in {"shared_lane", "share_busway"}:
            self.line(side_offset(centre, width / 2 - CYCLE_LANE_INSET, side), color=CYCLE_COLOUR, linewidth=1.5,
                      linestyle=":")

    def oneway_arrow(self, centre, reverse: bool):
        """An arrow at the middle of a one-way road pointing in the direction of travel."""
        start = centre.interpolate(0.45, normalized=True)
        end = centre.interpolate(0.55, normalized=True)
        if reverse:
            start, end = end, start
        self.axes.annotate("", xy=(end.x, end.y), xytext=(start.x, start.y),
                           arrowprops=dict(arrowstyle="-|>", color=CARRIAGEWAY_COLOUR, linewidth=1.2))

    def draw_path(self, way):
        """A separately mapped cycleway, footway, crossing or path."""
        line = shapely.LineString(way.xy)
        tags = way.tags
        self.remember(line, "path", way.id, tags)
        if is_bridge(tags):
            self.draw_bridge(line, 2.0)
        if tags.get("footway") == "crossing" or tags.get("cycleway") == "crossing":
            self.line(line, color=CROSSING_COLOUR, linewidth=2.0)
        elif tags.get("highway") == "cycleway" or tags.get("bicycle") == "designated":
            self.line(line, color=CYCLE_COLOUR, linewidth=1.4)
        else:
            self.line(line, color=FOOT_COLOUR, linewidth=0.8, linestyle=(0, (2, 2)))

    def draw_area(self, osm_id: int, tags: dict, geometry):
        """A mapped road area (area:highway), the exact carriageway or path outline where mappers drew it."""
        self.remember(geometry, "area", osm_id, tags)
        for polygon in getattr(geometry, "geoms", [geometry]):
            coordinates = np.asarray(polygon.exterior.coords)
            self.axes.fill(coordinates[:, 0], coordinates[:, 1], color="#ff8c00", alpha=0.25, linewidth=0)

    def draw_point(self, point):
        """A signal, crossing, stop or give way sign, lamp, bus stop or traffic sign node."""
        tags = point.tags
        self.remember(shapely.Point(point.x, point.y), "node", point.id, tags)
        style = POINT_STYLES.get(tags.get("highway"))
        if style:
            self.axes.plot([point.x], [point.y], linestyle="none", **style)
        if "traffic_sign" in tags:
            self.axes.text(point.x + 0.8, point.y, tags["traffic_sign"].replace("DE:", ""), fontsize=6,
                           color="white", bbox=dict(boxstyle="round,pad=0.15", facecolor="#c00", linewidth=0))

    def on_click(self, event):
        """Shows the tags of the feature nearest to the click, within 3 m."""
        if event.inaxes is not self.axes or event.xdata is None or self.axes.get_navigate_mode():
            return
        cursor = shapely.Point(event.xdata, event.ydata)
        nearest = min(self.features, key=lambda feature: feature[0].distance(cursor), default=None)
        if self.info is not None:
            self.info.remove()
            self.info = None
        if nearest is not None and nearest[0].distance(cursor) < 3.0:
            self.info = self.axes.text(event.xdata, event.ydata, nearest[1], fontsize=7, family="monospace",
                                       color="black", va="top",
                                       bbox=dict(boxstyle="round", facecolor="white", alpha=0.9))
        self.axes.figure.canvas.draw_idle()


def view_bounds(area: geo.Area, centre: str, size: float):
    """(x_min, y_min, x_max, y_max) of the viewed world rectangle."""
    if not centre:
        return area.x_min, area.y_min, area.x_max, area.y_max
    x, y = (float(value) for value in centre.split(","))
    return x - size / 2, y - size / 2, x + size / 2, y + size / 2


def is_bridge(tags: dict) -> bool:
    """True for ways that cross on a bridge (any bridge= value except no)."""
    return tags.get("bridge", "no") != "no"


def is_road_area(tags: dict) -> bool:
    """True for mapped carriageway and path areas."""
    return "area:highway" in tags


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region", choices=sorted(geo.AREAS))
    parser.add_argument("--centre", help="world x,y in metres (x east, y south); default the whole region")
    parser.add_argument("--size", type=float, default=150.0, help="side of the viewed square with --centre")
    parser.add_argument("--png", help="write an image instead of opening a window")
    args = parser.parse_args()
    if args.png:
        matplotlib.use("Agg")
    else:
        matplotlib.use("TkAgg")
    import matplotlib.pyplot as plt

    area = geo.AREAS[args.region]
    x_min, y_min, x_max, y_max = view_bounds(area, args.centre, args.size)
    geodata = data_root.geodata_dir()
    data = osm.load(os.path.join(geodata, "osm", "bergedorf.osm.pbf"), area, margin=50.0)
    photo = aerial.load(area, geodata, x_min - 20, y_min - 20, x_max + 20, y_max + 20)

    figure, axes = plt.subplots(figsize=(13, 13))
    figure.subplots_adjust(left=0.04, right=0.99, top=0.97, bottom=0.03)
    axes.imshow(photo.rgb, extent=photo.extent(), interpolation="lanczos")
    view = StreetView(axes)
    for osm_id, tags, geometry in data.areas:
        if is_road_area(tags):
            view.draw_area(osm_id, tags, geometry)
    for way in data.footways:
        view.draw_path(way)
    for way in data.railways:
        view.draw_railway(way)
    for way in data.roads:
        view.draw_road(way)
    for point in data.points:
        view.draw_point(point)
    axes.set_xlim(x_min, x_max)
    axes.set_ylim(y_max, y_min)
    axes.set_aspect("equal")
    axes.set_title(f"{area.name}: OSM as tagged over the aerial (x east, y south, metres). Click for tags.")
    if args.png:
        figure.savefig(args.png, dpi=150)
        return
    figure.canvas.mpl_connect("button_press_event", view.on_click)
    plt.show()


if __name__ == "__main__":
    main()
