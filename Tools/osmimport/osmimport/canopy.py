"""Tree and shrub detection from Hamburg's 1 m surface model (bDOM 2020, image matching).

Normalised surface height (bDOM - DGM) outside building footprints is vegetation: local maxima of the smoothed
canopy height are tree tops (position, height, crown size), low canopy (hedges, bushes) becomes shrubs.
This finds the garden, park and forest trees that no register contains.
"""
import glob
import os
from dataclasses import dataclass

import numpy as np
import rasterio
from rasterio.enums import Resampling
from rasterio.transform import from_origin
from rasterio.warp import reproject
from scipy import ndimage

from .dem import HeightGrid

TREE_MIN_HEIGHT = 3.0     # m above ground
SHRUB_HEIGHTS = (0.8, 3.0)
PEAK_WINDOW = 5           # m, local-maximum window


@dataclass
class Canopy:
    trees: np.ndarray    # (n, 4): x, y, height, crown diameter (world metres)
    shrubs: np.ndarray   # (n, 3): x, y, height
    covered: np.ndarray  # bool grid: bDOM data available (same grid as the DEM)
    grid: HeightGrid     # canopy height above ground

    def is_covered(self, x, y):
        c = ((np.asarray(x) - self.grid.x0) / self.grid.res).astype(int)
        r = ((np.asarray(y) - self.grid.y0) / self.grid.res).astype(int)
        h, w = self.covered.shape
        ok = (c >= 0) & (c < w) & (r >= 0) & (r < h)
        out = np.zeros(np.shape(c), dtype=bool)
        out[ok] = self.covered[r[ok], c[ok]]
        return out


def _load_dsm(dem: HeightGrid, area, geodata_root):
    files = glob.glob(os.path.join(geodata_root, "raw", "bdom_hamburg", "tif", "*.tif"))
    if not files:
        return None
    h, w = dem.z.shape
    e0 = dem.x0 + area.origin_e
    n1 = area.origin_n - dem.y0
    dst = np.full((h, w), np.nan, dtype=np.float32)
    transform = from_origin(e0, n1, dem.res, dem.res)
    for path in files:
        with rasterio.open(path) as src:
            b = src.bounds
            if b.right <= e0 or b.left >= e0 + w * dem.res or b.top <= n1 - h * dem.res or b.bottom >= n1:
                continue
            tmp = np.full((h, w), np.nan, dtype=np.float32)
            reproject(source=rasterio.band(src, 1), destination=tmp, dst_transform=transform, dst_crs="EPSG:25832",
                      src_nodata=-9999.0, dst_nodata=np.nan, resampling=Resampling.nearest)
            have = ~np.isnan(tmp)
            dst[have] = tmp[have]
    return dst


def detect(dem: HeightGrid, area, geodata_root, buildings_mask, hard_mask, water_mask):
    """dem: natural terrain grid; *_mask: bool grids (same shape) of building footprints, sealed surfaces, water."""
    dsm = _load_dsm(dem, area, geodata_root)
    if dsm is None:
        return None
    covered = ~np.isnan(dsm)
    ndsm = np.where(covered, dsm - dem.z, 0.0).astype(np.float32)
    # building edges are smeared in an image-matched DSM -> keep well clear of footprints
    near_building = ndimage.binary_dilation(buildings_mask, iterations=2)
    ndsm[near_building | water_mask] = 0.0
    ndsm = np.clip(ndsm, 0.0, 45.0)
    smooth = ndimage.gaussian_filter(ndsm, 1.0)

    # --- trees: local maxima of the canopy height ---
    peaks = (smooth == ndimage.maximum_filter(smooth, size=PEAK_WINDOW)) & (smooth > TREE_MIN_HEIGHT)
    tall = smooth > TREE_MIN_HEIGHT * 0.7
    dist = ndimage.distance_transform_edt(tall, sampling=dem.res)
    r, c = np.nonzero(peaks)
    heights = smooth[r, c]
    order = np.argsort(-heights)
    r, c, heights = r[order], c[order], heights[order]
    crowns = np.clip(2.0 * dist[r, c] + 1.0, 0.35 * heights, 0.85 * heights + 2.0)
    crowns = np.clip(crowns, 2.5, 22.0)
    # non-maximum suppression: a smaller peak inside a bigger tree's crown belongs to that crown
    keep = np.ones(len(r), dtype=bool)
    occupied = {}
    cell = 4
    for i in range(len(r)):
        y, x = r[i], c[i]
        rad = max(2.5, 0.3 * crowns[i])
        gx, gy = x // cell, y // cell
        k = int(np.ceil(rad / cell)) + 1
        clash = False
        for ix in range(gx - k, gx + k + 1):
            for iy in range(gy - k, gy + k + 1):
                for (px, py, pr) in occupied.get((ix, iy), ()):
                    if (px - x) ** 2 + (py - y) ** 2 < max(rad, pr) ** 2:
                        clash = True
                        break
                if clash:
                    break
            if clash:
                break
        if clash:
            keep[i] = False
            continue
        occupied.setdefault((gx, gy), []).append((x, y, rad))
    r, c, heights, crowns = r[keep], c[keep], heights[keep], crowns[keep]
    xs = dem.x0 + (c + 0.5) * dem.res
    ys = dem.y0 + (r + 0.5) * dem.res
    trees = np.column_stack([xs, ys, heights, crowns])

    # --- shrubs: low canopy away from sealed surfaces (parked cars!) ---
    low = (smooth > SHRUB_HEIGHTS[0]) & (smooth < SHRUB_HEIGHTS[1])
    low &= ~ndimage.binary_dilation(hard_mask, iterations=2)
    low = ndimage.binary_opening(low, iterations=1)  # drop single-pixel noise
    rr, cc = np.nonzero(low[::2, ::2])  # ~2 m spacing
    rr, cc = rr * 2, cc * 2
    shrubs = np.column_stack([dem.x0 + (cc + 0.5) * dem.res, dem.y0 + (rr + 0.5) * dem.res, smooth[rr, cc]])

    return Canopy(trees=trees, shrubs=shrubs, covered=covered, grid=HeightGrid(smooth, dem.x0, dem.y0, dem.res))
