"""Serves the street viewer: our street features over the Hamburg aerial photo or OpenStreetMap, in the browser.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/street_server.py [--port 8988]

Then open http://127.0.0.1:8988/. The whole OSM extract is loaded and its buildings typed once (about 25 s); the page asks for the features in
its viewport, which are rebuilt from a freshly reloaded `osmimport/street_layers.py` on every request, so a change to it
shows after a browser refresh without a restart.
"""
import argparse
import http.server
import importlib
import json
import os
import sys
import traceback
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import shapely  # noqa: E402
from pyproj import Transformer  # noqa: E402

import data_root  # noqa: E402
from build_world import building_footprints  # noqa: E402
from osmimport import building_types, geo, osm, street_layers  # noqa: E402

PAGE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "street_viewer.html")
# The world frame of every region (same origin), wide enough to take the whole extract.
WHOLE_EXTRACT = geo.Area("whole_extract", geo.ORIGIN_E, geo.ORIGIN_N, -100000, 100000, -100000, 100000)
# Wider viewports are refused: the page asks the user to zoom in instead of drawing tens of thousands of features.
MAX_VIEWPORT_METRES = 4000.0

_wgs_to_utm = Transformer.from_crs(4326, 25832, always_xy=True)


class ViewportIndex:
    """The loaded OSM data and the typed buildings with a spatial index, to cut out the part inside a viewport."""

    def __init__(self, data: osm.OsmData):
        self.data = data
        footprints = list(building_footprints(data))
        typer = building_types.BuildingTyper(footprints, data.roads, data.footways, data.areas)
        self.buildings = [(osm_id, tags, footprint, typer.classify(index))
                          for index, (osm_id, tags, footprint) in enumerate(footprints)]
        self.building_tree = shapely.STRtree([footprint for _, _, footprint in footprints])
        self.way_lists = {"roads": data.roads, "footways": data.footways, "railways": data.railways}
        self.way_trees = {name: shapely.STRtree([shapely.LineString(way.xy) for way in ways])
                          for name, ways in self.way_lists.items()}
        self.area_tree = shapely.STRtree([geometry for _, _, geometry in data.areas])
        self.point_tree = shapely.STRtree([shapely.Point(point.x, point.y) for point in data.points])

    def subset(self, box) -> osm.OsmData:
        """The roads, paths, railways, areas and points touching the world rectangle."""
        subset = osm.OsmData()
        for name, ways in self.way_lists.items():
            setattr(subset, name, [ways[index] for index in self.way_trees[name].query(box)])
        subset.areas = [self.data.areas[index] for index in self.area_tree.query(box)]
        subset.points = [self.data.points[index] for index in self.point_tree.query(box)]
        return subset

    def buildings_in(self, box) -> list:
        """The typed buildings touching the world rectangle."""
        return [self.buildings[index] for index in self.building_tree.query(box)]


def viewport_box(query: dict):
    """The world rectangle of the bbox=west,south,east,north query parameter (longitude, latitude)."""
    west, south, east, north = (float(value) for value in query["bbox"][0].split(","))
    east_min, north_min = _wgs_to_utm.transform(west, south)
    east_max, north_max = _wgs_to_utm.transform(east, north)
    return shapely.box(east_min - geo.ORIGIN_E, geo.ORIGIN_N - north_max,
                       east_max - geo.ORIGIN_E, geo.ORIGIN_N - north_min)


def region_outlines(module) -> dict:
    """{region name: GeoJSON outline} of every build region, so the viewer can show what the game covers."""
    return {name: module.region_outline(area) for name, area in geo.AREAS.items()}


class StreetViewerHandler(http.server.BaseHTTPRequestHandler):
    """Serves the page and the layer data; the spatial index is set on the class by main()."""
    index = None

    def do_GET(self):
        """The page at /, the layers in a viewport as GeoJSON at /layers.json?bbox=west,south,east,north."""
        url = urllib.parse.urlparse(self.path)
        if url.path in {"/", "/index.html"}:
            with open(PAGE_PATH, "rb") as page:
                self.respond(200, "text/html; charset=utf-8", page.read())
        elif url.path == "/layers.json":
            self.respond_layers(urllib.parse.parse_qs(url.query))
        else:
            self.respond(404, "text/plain", b"not found")

    def respond_layers(self, query: dict):
        """Rebuilds the layers in the viewport with the current street_layers code, or sends the error."""
        try:
            module = importlib.reload(street_layers)
            box = viewport_box(query)
            body = {"outlines": region_outlines(module), "layers": {}, "too_wide": False,
                    "building_classes": module.BUILDING_CLASS_COLOURS}
            x_min, y_min, x_max, y_max = box.bounds
            if max(x_max - x_min, y_max - y_min) > MAX_VIEWPORT_METRES:
                body["too_wide"] = True
            else:
                layers = module.build(self.index.subset(box), self.index.buildings_in(box))
                body["layers"] = module.to_geojson(layers, WHOLE_EXTRACT)
        except Exception:
            self.respond(500, "text/plain; charset=utf-8", traceback.format_exc().encode())
            return
        self.respond(200, "application/json", json.dumps(body).encode())

    def respond(self, status: int, content_type: str, body: bytes):
        """Sends one complete response; the page cancels requests for viewports it has already left, so a closed
        connection is normal."""
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, format, *args):
        """Quiet: no line per request."""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8988)
    args = parser.parse_args()
    data = osm.load(os.path.join(data_root.geodata_dir(), "osm", "bergedorf.osm.pbf"), WHOLE_EXTRACT, margin=0.0)
    StreetViewerHandler.index = ViewportIndex(data)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), StreetViewerHandler)
    print(f"Street viewer at http://127.0.0.1:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
