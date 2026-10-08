"""Converts Hamburg bDOM 2020 XYZ tiles (1 m surface model, see GeoData/REPORT.md) to float32 GeoTIFFs.

  .venv/bin/python -I convert_bdom.py <xyz dir> <tif dir>
"""
import glob
import os
import sys

import numpy as np
import rasterio
from rasterio.transform import from_origin


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(src, "*.xyz"))):
        out = os.path.join(dst, os.path.basename(path).replace(".xyz", ".tif"))
        if os.path.exists(out):
            continue
        xyz = np.fromfile(path, sep=" ", dtype=np.float64).reshape(-1, 3)
        e0, n0 = np.floor(xyz[:, 0].min()), np.floor(xyz[:, 1].min())
        grid = np.full((1000, 1000), -9999.0, dtype=np.float32)
        c = (xyz[:, 0] - e0).astype(int)
        r = 999 - (xyz[:, 1] - n0).astype(int)  # row 0 = north
        ok = (c >= 0) & (c < 1000) & (r >= 0) & (r < 1000)
        grid[r[ok], c[ok]] = xyz[ok, 2]
        # XYZ points are cell centres (e.g. 569000.0) -> the cell spans e-0.5 .. e+0.5
        with rasterio.open(out, "w", driver="GTiff", height=1000, width=1000, count=1, dtype="float32",
                           crs="EPSG:25832", transform=from_origin(e0 - 0.5, n0 + 999.5, 1.0, 1.0),
                           nodata=-9999.0, compress="deflate", predictor=3) as f:
            f.write(grid, 1)
        print(out, flush=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
