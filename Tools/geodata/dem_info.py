"""Summarise Hamburg DGM1 GeoTIFF tiles over a WGS84 bbox. usage: dem_info.py TILE_ROOT"""
import sys, glob, os, re
import numpy as np, rasterio
from rasterio.windows import from_bounds
from pyproj import Transformer
from shapely.geometry import box
from shapely.ops import unary_union

root = sys.argv[1]
W, S, E, N = 10.05, 53.40, 10.33, 53.53
t = Transformer.from_crs(4326, 25832, always_xy=True)
xs, ys = [], []
for lon in np.linspace(W, E, 20):
    for lat in np.linspace(S, N, 20):
        x, y = t.transform(lon, lat); xs.append(x); ys.append(y)
bb = (min(xs), min(ys), max(xs), max(ys))
print("bbox EPSG:25832", bb, "size km", (bb[2]-bb[0])/1000, (bb[3]-bb[1])/1000)
files = [f for f in glob.glob(os.path.join(root, "**", "*.tif"), recursive=True)]
print("total tiles", len(files))
sel = []
for f in files:
    m = re.search(r"dgm1_32_(\d+)_(\d+)_", os.path.basename(f))
    if not m: print("odd name", f); continue
    x0, y0 = int(m[1])*1000, int(m[2])*1000
    if x0 < bb[2] and x0+1000 > bb[0] and y0 < bb[3] and y0+1000 > bb[1]:
        sel.append((f, x0, y0))
print("tiles intersecting bbox", len(sel), "size MB", sum(os.path.getsize(f) for f,_,_ in sel)/1e6)
with rasterio.open(sel[0][0]) as r:
    print(r.profile); print("crs", r.crs, "res", r.res, "bounds", r.bounds, "tags", r.tags())
# coverage + stats
polys=[]; vmin=1e9; vmax=-1e9; n=0; nod=0; hist=[]
big = (bb[0], bb[1], bb[2], bb[3])
for f,x0,y0 in sel:
    with rasterio.open(f) as r:
        a = r.read(1, masked=True)
        polys.append(box(*r.bounds))
        if a.count():
            vmin=min(vmin,float(a.min())); vmax=max(vmax,float(a.max()))
        n += a.size; nod += a.mask.sum() if np.ma.is_masked(a) else 0
        if tuple(round(v,6) for v in r.res)!=(1.0,1.0) or r.dtypes[0]!='float32': print("odd", f, r.res, r.dtypes)
cov = unary_union(polys)
bbpoly = box(*bb)
print("tile union bounds", cov.bounds, "area km2", cov.area/1e6)
print("bbox rect area km2", bbpoly.area/1e6, "covered fraction (of projected-bbox rect)", cov.intersection(bbpoly).area/bbpoly.area)
print("value range", vmin, vmax, "nodata cells", nod, "of", n)
# exact geographic bbox polygon in 25832
ring = [(W,S),(E,S),(E,N),(W,N),(W,S)]
ring = []
for lon in np.linspace(W,E,50): ring.append(t.transform(lon,S))
for lat in np.linspace(S,N,50): ring.append(t.transform(E,lat))
for lon in np.linspace(E,W,50): ring.append(t.transform(lon,N))
for lat in np.linspace(N,S,50): ring.append(t.transform(W,lat))
from shapely.geometry import Polygon
gp = Polygon(ring)
miss = gp.difference(cov)
print("geo bbox area km2", gp.area/1e6, "not covered by tiles km2", miss.area/1e6, "fraction", miss.area/gp.area)
print("uncovered bounds", miss.bounds if not miss.is_empty else None)
# which parts uncovered, in lon/lat
ti = Transformer.from_crs(25832,4326,always_xy=True)
geoms = list(miss.geoms) if hasattr(miss,'geoms') else [miss]
for g in sorted(geoms, key=lambda g:-g.area)[:15]:
    b=g.bounds; print("  piece %.2f km2"%(g.area/1e6), "lon/lat", ti.transform(b[0],b[1]), ti.transform(b[2],b[3]))
