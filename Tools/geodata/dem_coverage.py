"""ASCII coverage map of valid DGM1 cells inside WGS84 bbox. usage: dem_coverage.py TILE_ROOT"""
import sys, glob, os, re
import numpy as np, rasterio
from pyproj import Transformer
roots = sys.argv[1].split(",")
W, S, E, N = 10.05, 53.40, 10.33, 53.53
D = 10  # decimation (10 m)
t = Transformer.from_crs(4326, 25832, always_xy=True)
x0, y0 = 569000, 5917000; x1, y1 = 589000, 5933000
nx, ny = (x1-x0)//D, (y1-y0)//D
grid = np.full((ny, nx), np.nan, dtype=np.float32)
files = sorted((f for r_ in roots for f in glob.glob(os.path.join(r_, "**", "*.tif"), recursive=True)), key=lambda f: ("2025" in f, f))
src = np.zeros((ny, nx), dtype=np.uint8)
for f in files:
    m = re.search(r"dgm1_32_(\d+)_(\d+)_", os.path.basename(f))
    if not m: continue
    e, n = int(m[1])*1000, int(m[2])*1000
    if not (x0 <= e < x1 and y0 <= n < y1): continue
    with rasterio.open(f) as r:
        a = r.read(1, out_shape=(1000//D, 1000//D), resampling=rasterio.enums.Resampling.nearest)
    a = np.where((a > -1000) & (a < 1000), a, np.nan)
    c = (e-x0)//D; rr = (y1-(n+1000))//D
    sl = (slice(rr, rr+1000//D), slice(c, c+1000//D))
    ok = ~np.isnan(a)
    grid[sl] = np.where(ok, a, grid[sl])
    src[sl] = np.where(ok, 2 if '_ni_' in f else 1, src[sl])
# mask by geographic bbox
ti = Transformer.from_crs(25832, 4326, always_xy=True)
X, Y = np.meshgrid(x0 + D*(np.arange(nx)+.5), y1 - D*(np.arange(ny)+.5))
lon, lat = ti.transform(X, Y)
inb = (lon >= W) & (lon <= E) & (lat >= S) & (lat <= N)
valid = ~np.isnan(grid)
print("share by source: HH %.1f%% NI %.1f%% none %.1f%%" % tuple(100*((src==k)&inb).sum()/inb.sum() for k in (1,2,0)))
print("cells in bbox", inb.sum(), "valid DGM share %.1f%%" % (100*(valid & inb).sum()/inb.sum()))
v = grid[valid & inb]
print("height m: min %.2f max %.2f mean %.2f p1 %.2f p99 %.2f" % (v.min(), v.max(), v.mean(), np.percentile(v,1), np.percentile(v,99)))
np.save(sys.argv[2] if len(sys.argv) > 2 else "/tmp/dem10.npy", grid)
# ascii map, 1 char = 500 m x 1000 m
for rr in range(0, ny, 100):
    line = ""
    for c in range(0, nx, 50):
        b = inb[rr:rr+100, c:c+50]; vv = valid[rr:rr+100, c:c+50] & b
        if b.sum() == 0: line += " "
        else:
            f = vv.sum()/b.sum(); line += "#" if f > .9 else ("+" if f > .5 else ("." if f > .05 else "-"))
    print(line)
