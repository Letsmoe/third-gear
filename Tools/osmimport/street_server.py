"""Serves the street viewer: our street features over the Hamburg aerial photo or OpenStreetMap, in the browser.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/street_server.py <region> [--port 8988]

Then open http://127.0.0.1:8988/. The OSM data is loaded once; the street layers are rebuilt from a freshly reloaded
`osmimport/street_layers.py` on every page load, so a change to it shows after a browser refresh without a restart.
"""
import argparse
import http.server
import importlib
import json
import os
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from osmimport import geo, osm, street_layers  # noqa: E402

PAGE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "street_viewer.html")
OSM_MARGIN = 100.0


class StreetViewerHandler(http.server.BaseHTTPRequestHandler):
    """Serves the page and the layer data; the region and its OSM data are set on the class by main()."""
    area = None
    osm_data = None

    def do_GET(self):
        """The page at /, the layers as GeoJSON at /layers.json."""
        if self.path in {"/", "/index.html"}:
            with open(PAGE_PATH, "rb") as page:
                self.respond(200, "text/html; charset=utf-8", page.read())
        elif self.path == "/layers.json":
            self.respond_layers()
        else:
            self.respond(404, "text/plain", b"not found")

    def respond_layers(self):
        """Rebuilds the layers with the current street_layers code and sends them, or the error if it fails."""
        try:
            module = importlib.reload(street_layers)
            layers = module.build(self.osm_data)
            body = {"region": self.area.name, "outline": module.region_outline(self.area),
                    "layers": module.to_geojson(layers, self.area)}
            self.respond(200, "application/json", json.dumps(body).encode())
        except Exception:
            self.respond(500, "text/plain; charset=utf-8", traceback.format_exc().encode())

    def respond(self, status: int, content_type: str, body: bytes):
        """Sends one complete response."""
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        """Quiet: no line per request."""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region", choices=sorted(geo.AREAS))
    parser.add_argument("--port", type=int, default=8988)
    args = parser.parse_args()
    area = geo.AREAS[args.region]
    StreetViewerHandler.area = area
    StreetViewerHandler.osm_data = osm.load(os.path.join(data_root.geodata_dir(), "osm", "bergedorf.osm.pbf"), area,
                                            margin=OSM_MARGIN)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), StreetViewerHandler)
    print(f"Street viewer for {area.name} at http://127.0.0.1:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
