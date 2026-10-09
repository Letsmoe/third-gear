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
import threading
import traceback
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import shapely  # noqa: E402
from pyproj import Transformer  # noqa: E402

import build_area  # noqa: E402
import build_world  # noqa: E402
import data_root  # noqa: E402
import game_streets  # noqa: E402
from osmimport import dem, furniture, geo, hh_survey, roads, street_layers, street_scene  # noqa: E402
from street_index import WHOLE_EXTRACT, ViewportIndex, load_whole_extract  # noqa: E402

PAGE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "street_viewer.html")
SCENE_PAGE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "street_scene.html")
AERIAL_WMS = ("https://geodienste.hamburg.de/wms_dop_zeitreihe_unbelaubt?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap"
              "&FORMAT=image/jpeg&STYLES=&CRS=EPSG:25832&LAYERS=dop_zeitreihe_unbelaubt")
AERIAL_PIXELS = 4096
# The 3D scene covers at most this square around the viewport centre: roofs and draping take a few seconds per km².
MAX_SCENE_METRES = 1000.0
# Wider viewports are refused: the page asks the user to zoom in instead of drawing tens of thousands of features.
MAX_VIEWPORT_METRES = 4000.0

_wgs_to_utm = Transformer.from_crs(4326, 25832, always_xy=True)


# The modules that make the game's streets, in the order they are reloaded when one of them changed.
GAME_MODULES = [roads, furniture, build_area, build_world, game_streets]


class GameStreetCache:
    """The game's streets in TILE_SIZE tiles, generated on first view and kept until the generator code changes."""

    def __init__(self, index: "ViewportIndex"):
        self.index = index
        self.tiles = {}
        self.lock = threading.Lock()
        self.stamp = self._source_stamp()

    @staticmethod
    def _source_stamp():
        """Modification times of the generator modules."""
        return tuple(os.path.getmtime(module.__file__) for module in GAME_MODULES)

    def _reload_if_changed(self):
        """Reloads the generator modules and drops the cached tiles when their code changed on disk."""
        stamp = self._source_stamp()
        if stamp == self.stamp:
            return
        for module in GAME_MODULES:
            importlib.reload(module)
        self.tiles.clear()
        self.stamp = stamp

    def _tile(self, column: int, row: int):
        """One tile's streets, generated on first use."""
        key = (column, row)
        if key not in self.tiles:
            self.tiles[key] = game_streets.generate_tile(self.index, column, row)
        return self.tiles[key]

    def streets_in(self, box):
        """The game's streets of every tile touching the box, and how many tiles had to be generated."""
        with self.lock:
            self._reload_if_changed()
            result = game_streets.GameStreets()
            generated = 0
            for column, row in game_streets.tiles_touching(box):
                generated += (column, row) not in self.tiles
                result.extend(self._tile(column, row))
            return result, generated


def viewport_box(query: dict):
    """The world rectangle of the bbox=west,south,east,north query parameter (longitude, latitude)."""
    west, south, east, north = (float(value) for value in query["bbox"][0].split(","))
    east_min, north_min = _wgs_to_utm.transform(west, south)
    east_max, north_max = _wgs_to_utm.transform(east, north)
    return shapely.box(east_min - geo.ORIGIN_E, geo.ORIGIN_N - north_max,
                       east_max - geo.ORIGIN_E, geo.ORIGIN_N - north_min)


def scene_box(query: dict):
    """The world rectangle of the 3D scene (world=… in metres or bbox=… in degrees), cut to MAX_SCENE_METRES around
    its centre."""
    if "world" in query:
        x_min, y_min, x_max, y_max = world_box_query(query).bounds
    else:
        x_min, y_min, x_max, y_max = viewport_box(query).bounds
    centre_x, centre_y = (x_min + x_max) / 2, (y_min + y_max) / 2
    half_x = min(x_max - x_min, MAX_SCENE_METRES) / 2
    half_y = min(y_max - y_min, MAX_SCENE_METRES) / 2
    return shapely.box(centre_x - half_x, centre_y - half_y, centre_x + half_x, centre_y + half_y)


def world_box_query(query: dict):
    """The world rectangle of the world=x_min,y_min,x_max,y_max query parameter."""
    return shapely.box(*(float(value) for value in query["world"][0].split(",")))


def region_outlines(module) -> dict:
    """{region name: GeoJSON outline} of every build region, so the viewer can show what the game covers."""
    return {name: module.region_outline(area) for name, area in geo.AREAS.items()}


class StreetViewerHandler(http.server.BaseHTTPRequestHandler):
    """Serves the page and the layer data; the spatial index is set on the class by main()."""
    index = None
    game_cache = None
    survey = None

    def do_GET(self):
        """The page at /, the layers in a viewport as GeoJSON at /layers.json?bbox=west,south,east,north."""
        url = urllib.parse.urlparse(self.path)
        if url.path in {"/", "/index.html"}:
            with open(PAGE_PATH, "rb") as page:
                self.respond(200, "text/html; charset=utf-8", page.read())
        elif url.path == "/layers.json":
            self.respond_layers(urllib.parse.parse_qs(url.query))
        elif url.path == "/game.json":
            self.respond_game(urllib.parse.parse_qs(url.query))
        elif url.path == "/3d":
            with open(SCENE_PAGE_PATH, "rb") as page:
                self.respond(200, "text/html; charset=utf-8", page.read())
        elif url.path == "/scene.json":
            self.respond_scene(urllib.parse.parse_qs(url.query))
        elif url.path == "/aerial.jpg":
            self.respond_aerial(urllib.parse.parse_qs(url.query))
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
                if self.survey is not None:
                    module.add_survey(layers, self.survey, box)
                body["layers"] = module.to_geojson(layers, WHOLE_EXTRACT)
        except Exception:
            self.respond(500, "text/plain; charset=utf-8", traceback.format_exc().encode())
            return
        self.respond(200, "application/json", json.dumps(body).encode())

    def respond_game(self, query: dict):
        """The game's generated streets in the viewport as GeoJSON layers, or the error."""
        try:
            module = importlib.reload(street_layers)
            box = viewport_box(query)
            x_min, y_min, x_max, y_max = box.bounds
            body = {"layers": {}, "too_wide": max(x_max - x_min, y_max - y_min) > MAX_VIEWPORT_METRES / 2,
                    "generated_tiles": 0}
            if not body["too_wide"]:
                streets, body["generated_tiles"] = self.game_cache.streets_in(box)
                body["layers"] = module.to_geojson(game_streets.map_layers(streets), WHOLE_EXTRACT)
        except Exception:
            self.respond(500, "text/plain; charset=utf-8", traceback.format_exc().encode())
            return
        self.respond(200, "application/json", json.dumps(body).encode())

    def respond_scene(self, query: dict):
        """Builds the 3D scene of the viewport with the current street_scene code, or sends the error."""
        try:
            layers_module = importlib.reload(street_layers)
            module = importlib.reload(street_scene)
            box = scene_box(query)
            x_min, y_min, x_max, y_max = box.bounds
            scene_area = geo.Area("scene", geo.ORIGIN_E, geo.ORIGIN_N, x_min, x_max, y_min, y_max)
            heights = dem.build_mosaic(scene_area, data_root.geodata_dir(), margin=20.0)
            streets, _ = self.game_cache.streets_in(box)
            body = module.build(self.index.subset(box), self.index.buildings_in(box), streets, heights, box)
            body["building_classes"] = layers_module.BUILDING_CLASS_COLOURS
            body["lonlat_bounds"] = list(layers_module.world_to_lonlat(WHOLE_EXTRACT, box).bounds)
        except Exception:
            self.respond(500, "text/plain; charset=utf-8", traceback.format_exc().encode())
            return
        self.respond(200, "application/json", json.dumps(body).encode())

    def respond_aerial(self, query: dict):
        """The aerial photo of a world rectangle, fetched from the WMS here because it sends no CORS headers and
        WebGL refuses foreign textures without them."""
        x_min, y_min, x_max, y_max = world_box_query(query).bounds
        bbox = f"{geo.ORIGIN_E + x_min},{geo.ORIGIN_N - y_max},{geo.ORIGIN_E + x_max},{geo.ORIGIN_N - y_min}"
        url = f"{AERIAL_WMS}&WIDTH={AERIAL_PIXELS}&HEIGHT={AERIAL_PIXELS}&BBOX={bbox}"
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                image = response.read()
        except Exception:
            self.respond(502, "text/plain; charset=utf-8", traceback.format_exc().encode())
            return
        self.respond(200, "image/jpeg", image)

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
    StreetViewerHandler.index = load_whole_extract()
    StreetViewerHandler.survey = hh_survey.load(data_root.geodata_dir(), WHOLE_EXTRACT)
    StreetViewerHandler.game_cache = GameStreetCache(StreetViewerHandler.index)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), StreetViewerHandler)
    print(f"Street viewer at http://127.0.0.1:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
