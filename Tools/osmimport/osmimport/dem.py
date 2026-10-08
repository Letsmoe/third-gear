"""Terrain heights: mosaic of 1 m DGM tiles (Hamburg > Niedersachsen) with Copernicus GLO-30 as fallback.

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

TILE_RE = re.compile(r"dgm1_32_(\d+)_(\d+)_")
NODATA_LIMIT = -1000.0  # Hamburg tiles use -9999 despite the header saying -3.4e38


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
    files = []
    for path in glob.glob(os.path.join(root, "**", "*.tif"), recursive=True):
        m = TILE_RE.search(os.path.basename(path))
        if m:
            files.append((int(m[1]) * 1000, int(m[2]) * 1000, path))
    return files


def build_mosaic(area: Area, geodata_root: str, margin: float = 100.0) -> HeightGrid:
    """1 m height grid covering the area extent plus margin (metres)."""
    e0, n0, e1, n1 = area.utm_bounds(margin)
    e0, n0 = np.floor(e0), np.floor(n0)
    e1, n1 = np.ceil(e1), np.ceil(n1)
    width, height = int(e1 - e0), int(n1 - n0)
    z = np.full((height, width), np.nan, dtype=np.float32)

    raw = os.path.join(geodata_root, "raw")
    # Lowest priority first; later sources overwrite where they have valid data.
    ni = [f for f in _tile_files(os.path.join(raw, "dgm1_niedersachsen")) if "_2025" in f[2]]
    hh = _tile_files(os.path.join(raw, "dgm1_hamburg_extracted"))
    for tile_e, tile_n, path in ni + hh:
        if tile_e + 1000 <= e0 or tile_e >= e1 or tile_n + 1000 <= n0 or tile_n >= n1:
            continue
        with rasterio.open(path) as src:
            data = src.read(1).astype(np.float32)
            left, top = src.bounds.left, src.bounds.top
        # Overlap in grid coordinates (rows go north -> south).
        c_from = int(max(left, e0) - e0)
        c_to = int(min(left + data.shape[1], e1) - e0)
        r_from = int(n1 - min(top, n1))
        r_to = int(n1 - max(top - data.shape[0], n0))
        tr_from = int(top - (n1 - r_from))
        tc_from = int(e0 + c_from - left)
        sub = data[tr_from:tr_from + (r_to - r_from), tc_from:tc_from + (c_to - c_from)]
        valid = sub > NODATA_LIMIT
        region = z[r_from:r_to, c_from:c_to]
        region[valid] = sub[valid]

    missing = np.isnan(z)
    if missing.any():
        _fill_from_glo30(z, missing, e0, n1, os.path.join(raw, "copernicus_glo30"))

    # world: x = E - origin_e, y = origin_n - N; row 0 = northern edge (n1)
    return HeightGrid(z=z, x0=e0 - area.origin_e, y0=area.origin_n - n1, res=1.0)


def _fill_from_glo30(z, missing, e0, n1, glo_dir):
    files = glob.glob(os.path.join(glo_dir, "*.tif"))
    if not files:
        raise RuntimeError("DEM gap and no GLO-30 fallback found")
    from rasterio.transform import from_origin

    dst = np.full(z.shape, np.nan, dtype=np.float32)
    dst_transform = from_origin(e0, n1, 1.0, 1.0)
    for path in files:
        with rasterio.open(path) as src:
            reproject(
                source=rasterio.band(src, 1),
                destination=dst,
                dst_transform=dst_transform,
                dst_crs="EPSG:25832",
                dst_nodata=np.nan,
                resampling=Resampling.bilinear,
            )
    # GLO-30 is a surface model (trees/roofs) and uses EGM2008; blend it towards the DGM at the seam
    # by shifting it with the median offset along the border of valid DGM data.
    have_dgm = ~missing
    border = have_dgm & ndimage.binary_dilation(missing, iterations=3)
    if border.any():
        offset = np.nanmedian(z[border] - dst[border])
        dst += offset
    z[missing] = dst[missing]
    if np.isnan(z).any():
        z[np.isnan(z)] = np.nanmedian(z)
