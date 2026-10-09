"""Hamburg leaf-off orthophoto as a reference image under the street model (about 10 cm per pixel).

Tiles are fetched from the city's public WMS and cached under `<data root>/geodata/aerial/`, so a region is
downloaded once. Coordinates are world metres (x east, y south, see geo.py).
"""
import io
import os
import urllib.request
from dataclasses import dataclass

import numpy as np
from PIL import Image

from .geo import Area

WMS_URL = "https://geodienste.hamburg.de/wms_dop_zeitreihe_unbelaubt"
WMS_LAYER = "dop_zeitreihe_unbelaubt"
TILE_METRES = 100
TILE_PIXELS = 1000


@dataclass
class AerialImage:
    """An RGB mosaic covering world x in [x_min, x_max) and y in [y_min, y_max), row 0 at y_min (north)."""
    rgb: np.ndarray
    x_min: float
    y_min: float
    x_max: float
    y_max: float

    def extent(self):
        """Extent for matplotlib's imshow with the y axis pointing down (south)."""
        return (self.x_min, self.x_max, self.y_max, self.y_min)


def _tile_url(east: int, north: int) -> str:
    """GetMap request for the 100 m tile whose south-west corner is at (east, north) in UTM 32N."""
    return (f"{WMS_URL}?SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap&FORMAT=image/jpeg&STYLES=&CRS=EPSG:25832"
            f"&LAYERS={WMS_LAYER}&WIDTH={TILE_PIXELS}&HEIGHT={TILE_PIXELS}"
            f"&BBOX={east},{north},{east + TILE_METRES},{north + TILE_METRES}")


def _load_tile(cache_dir: str, east: int, north: int) -> np.ndarray:
    """One tile as an RGB array, downloaded on first use."""
    path = os.path.join(cache_dir, f"{east}_{north}.jpg")
    if not os.path.exists(path):
        with urllib.request.urlopen(_tile_url(east, north), timeout=120) as response:
            data = response.read()
        Image.open(io.BytesIO(data)).verify()  # fails on a WMS error page instead of caching it
        with open(path + ".part", "wb") as out:
            out.write(data)
        os.replace(path + ".part", path)
    return np.asarray(Image.open(path).convert("RGB"))


def load(area: Area, geodata_dir: str, x_min: float, y_min: float, x_max: float, y_max: float,
         metres_per_pixel: float = 0.1) -> AerialImage:
    """Mosaic of the aerial photo covering the given world rectangle, snapped outward to whole tiles."""
    cache_dir = os.path.join(geodata_dir, "aerial", WMS_LAYER)
    os.makedirs(cache_dir, exist_ok=True)
    east_min = int(np.floor((area.origin_e + x_min) / TILE_METRES)) * TILE_METRES
    east_max = int(np.ceil((area.origin_e + x_max) / TILE_METRES)) * TILE_METRES
    north_min = int(np.floor((area.origin_n - y_max) / TILE_METRES)) * TILE_METRES
    north_max = int(np.ceil((area.origin_n - y_min) / TILE_METRES)) * TILE_METRES
    step = max(1, int(round(metres_per_pixel * TILE_PIXELS / TILE_METRES)))
    tile_size = TILE_PIXELS // step
    columns = (east_max - east_min) // TILE_METRES
    rows = (north_max - north_min) // TILE_METRES
    mosaic = np.zeros((rows * tile_size, columns * tile_size, 3), np.uint8)
    for row in range(rows):
        north = north_max - (row + 1) * TILE_METRES
        for column in range(columns):
            east = east_min + column * TILE_METRES
            tile = _load_tile(cache_dir, east, north)[::step, ::step]
            mosaic[row * tile_size:(row + 1) * tile_size, column * tile_size:(column + 1) * tile_size] = tile
    return AerialImage(mosaic, east_min - area.origin_e, area.origin_n - north_max,
                       east_max - area.origin_e, area.origin_n - north_min)
