"""Terrain heights: mosaic of 5 m DGM tiles (Hamburg > Niedersachsen) with Copernicus GLO-30 as fallback, served on a
1 m grid.

The 5 m tiles are averaged from the states' 1 m DGM by Tools/bootstrap/prepare_geodata.py.

The mosaic is a regular grid in world coordinates (see geo.py): row 0 is the northern edge (smallest y).
"""
import glob
import os
import re
from dataclasses import dataclass

import numpy as np
import rasterio
from rasterio.enums import Resampling
from rasterio.warp import reproject
from scipy import ndimage

from .geo import Area

TILE_RE = re.compile(r"dgm5_32_(\d+)_(\d+)_(hh|ni)")
COARSE_RES = 5
# Water holes in the DGM are filled at this percentile of their bank heights: near the lowest bank, the water level.
BANK_PERCENTILE = 5


@dataclass
class HeightGrid:
    """Heights on a regular grid. Cell (r, c) centre is at x = x0 + (c + 0.5) * res, y = y0 + (r + 0.5) * res."""
    z: np.ndarray
    x0: float
    y0: float
    res: float

    def sample(self, x, y):
        """Bilinear sample at world x/y (arrays or scalars). Clamps at the border."""
        x = np.asarray(x, dtype=np.float64)
        y = np.asarray(y, dtype=np.float64)
        fc = (x - self.x0) / self.res - 0.5
        fr = (y - self.y0) / self.res - 0.5
        h, w = self.z.shape
        fc = np.clip(fc, 0, w - 1.000001)
        fr = np.clip(fr, 0, h - 1.000001)
        c0 = np.floor(fc).astype(np.int64)
        r0 = np.floor(fr).astype(np.int64)
        tc = fc - c0
        tr = fr - r0
        z = self.z
        top = z[r0, c0] * (1 - tc) + z[r0, c0 + 1] * tc
        bottom = z[r0 + 1, c0] * (1 - tc) + z[r0 + 1, c0 + 1] * tc
        return top * (1 - tr) + bottom * tr

    def cell_centers(self):
        h, w = self.z.shape
        xs = self.x0 + (np.arange(w) + 0.5) * self.res
        ys = self.y0 + (np.arange(h) + 0.5) * self.res
        return xs, ys


def _tile_files(root):
    """(east, north, path) of the 5 m terrain tiles, Lower Saxony's first so Hamburg's overwrite them."""
    files = []
    for path in glob.glob(os.path.join(root, "*.tif")):
        match = TILE_RE.search(os.path.basename(path))
        if match:
            files.append((match[3] == "hh", int(match[1]) * 1000, int(match[2]) * 1000, path))
    return [(east, north, path) for _, east, north, path in sorted(files)]


def build_mosaic(area: Area, geodata_root: str, margin: float = 100.0) -> HeightGrid:
    """1 m height grid covering the area extent plus margin (metres), interpolated from the 5 m terrain."""
    e0, n0, e1, n1 = area.utm_bounds(margin)
    e0, n0 = np.floor(e0), np.floor(n0)
    e1, n1 = np.ceil(e1), np.ceil(n1)
    coarse = _coarse_mosaic(e0, n0, e1, n1, os.path.join(geodata_root, "raw"))
    xs = e0 + np.arange(int(e1 - e0)) + 0.5
    ys = n1 - np.arange(int(n1 - n0)) - 0.5
    # The coarse grid runs north to south like the fine one, so sampling it with -northing as y keeps rows in order.
    z = coarse.sample(xs[None, :], -ys[:, None]).astype(np.float32)
    # world: x = E - origin_e, y = origin_n - N; row 0 = northern edge (n1)
    return HeightGrid(z=z, x0=e0 - area.origin_e, y0=area.origin_n - n1, res=1.0)


def _coarse_mosaic(e0, n0, e1, n1, raw) -> HeightGrid:
    """The 5 m terrain over a UTM box and one cell beyond, in UTM eastings and negated northings, with GLO-30 where
    the DGM has no data."""
    res = float(COARSE_RES)
    e0 = np.floor(e0 / res) * res - res
    n0 = np.floor(n0 / res) * res - res
    e1 = np.ceil(e1 / res) * res + res
    n1 = np.ceil(n1 / res) * res + res
    width, height = int((e1 - e0) / res), int((n1 - n0) / res)
    z = np.full((height, width), np.nan, dtype=np.float32)
    for tile_e, tile_n, path in _tile_files(os.path.join(raw, "dgm5")):
        if tile_e + 1000 <= e0 or tile_e >= e1 or tile_n + 1000 <= n0 or tile_n >= n1:
            continue
        with rasterio.open(path) as src:
            data = src.read(1).astype(np.float32)
            left, top = src.bounds.left, src.bounds.top
        _paste(z, data, int(round((left - e0) / res)), int(round((n1 - top) / res)))

    _fill_enclosed_holes(z)
    missing = np.isnan(z)
    if missing.any():
        _fill_from_glo30(z, missing, e0, n1, res, os.path.join(raw, "copernicus_glo30"))
    return HeightGrid(z=z, x0=e0, y0=-n1, res=res)


def _fill_enclosed_holes(z):
    """Fills gaps surrounded by survey data, which are water (Hamburg's DGM has none there), at the low level of
    their banks. GLO-30 is too coarse for them: a pond is a few of its cells, measured on the trees around it."""
    holes, count = ndimage.label(np.isnan(z))
    touching_edge = set(np.unique(np.concatenate([holes[0], holes[-1], holes[:, 0], holes[:, -1]])))
    for label, extent in enumerate(ndimage.find_objects(holes), start=1):
        if label in touching_edge:
            continue
        rows = slice(max(extent[0].start - 1, 0), extent[0].stop + 1)
        columns = slice(max(extent[1].start - 1, 0), extent[1].stop + 1)
        hole = holes[rows, columns] == label
        bank = ndimage.binary_dilation(hole) & ~hole
        z[rows, columns][hole] = np.percentile(z[rows, columns][bank], BANK_PERCENTILE)


def _paste(z, data, column, row):
    """Copies a tile's valid cells into the grid at a (column, row) offset, clipped to the grid."""
    rows, columns = z.shape
    r_from, c_from = max(row, 0), max(column, 0)
    r_to, c_to = min(row + data.shape[0], rows), min(column + data.shape[1], columns)
    if r_from >= r_to or c_from >= c_to:
        return
    tile = data[r_from - row:r_to - row, c_from - column:c_to - column]
    valid = ~np.isnan(tile)
    z[r_from:r_to, c_from:c_to][valid] = tile[valid]


def _fill_from_glo30(z, missing, e0, n1, res, glo_dir):
    """Fills the missing cells (outside the surveys) from the GLO-30 tiles."""
    files = glob.glob(os.path.join(glo_dir, "*.tif"))
    if not files:
        raise RuntimeError("DEM gap and no GLO-30 fallback found")
    from rasterio.transform import from_origin

    dst = np.full(z.shape, np.nan, dtype=np.float32)
    dst_transform = from_origin(e0, n1, res, res)
    for path in files:
        # Each reprojection starts from an empty grid, so tiles are merged afterwards instead of written in turn.
        tile = np.full(z.shape, np.nan, dtype=np.float32)
        with rasterio.open(path) as src:
            reproject(
                source=rasterio.band(src, 1),
                destination=tile,
                dst_transform=dst_transform,
                dst_crs="EPSG:25832",
                dst_nodata=np.nan,
                resampling=Resampling.bilinear,
            )
        covered = ~np.isnan(tile)
        dst[covered] = tile[covered]
    # No offset towards the DGM: EGM2008 and DHHN2016 differ by less than half a metre here, and the seam is often a
    # wooded edge where GLO-30, a surface model, measures the treetops, which shifted whole gaps by 15 m.
    z[missing] = dst[missing]
    if np.isnan(z).any():
        z[np.isnan(z)] = np.nanmedian(z)
