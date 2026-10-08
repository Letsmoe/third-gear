"""Low-detail surroundings so the world doesn't end at the edge of the detailed area: terrain out to the horizon,
buildings, water, measured trees close by and forest silhouettes further out.

  .venv/bin/python -I build_horizon.py [--detail bergedorf_core] [--radius 13000] [--tree-ring 1000]

Output: GeoData/build/horizon/tile_*.dgmesh + manifest.json + vegetation.json (same formats as build_area.py);
Scripts/import_osm_area.py <area> adds them to the level of the detailed area.
Terrain resolution falls off with distance (10 / 20 / 40 m); tile borders get skirts so the resolution steps and the
seam to the detailed terrain never show gaps. Heights: DGM1 (HH > NI) inside the geodata bbox, Copernicus GLO-30
beyond it (a surface model, which conveniently gives distant woods a silhouette).
"""
import argparse
import json
import os
import sys
import time

import numpy as np
import shapely
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from osmimport import buildings, canopy, dem, geo, landcover, osm, paths, terrain, vegetation  # noqa: E402
from osmimport.mesh import MeshBuilder, triangulate_polygon, walls  # noqa: E402

TILE = 2000.0
SKIRT = 4.0          # m
CANOPY_HEIGHT = (14.0, 22.0)
FAR_TREE_MIN_HEIGHT = 6.0


def log(msg, t0=[time.time()]):
    print(f"[{time.time() - t0[0]:6.1f}s] {msg}", flush=True)


def rect_distance(a, b):
    """Distance between two (x0, y0, x1, y1) rectangles (0 if they touch/overlap)."""
    dx = max(b[0] - a[2], a[0] - b[2], 0)
    dy = max(b[1] - a[3], a[1] - b[3], 0)
    return float(np.hypot(dx, dy))


def resolution(dist):
    return 10.0 if dist < 3000 else 20.0 if dist < 7000 else 40.0


def tile_dem(bounds, origin_area, geodata):
    x0, y0, x1, y1 = bounds
    tmp = geo.Area("tmp", origin_area.origin_e, origin_area.origin_n, x0, x1, y0, y1, tile_size=TILE)
    return dem.build_mosaic(tmp, geodata, margin=60.0)


def grid_mesh(mb, heights, bounds, res, hole, weights):
    x0, y0, x1, y1 = bounds
    xs = np.arange(x0, x1 + 1e-6, res)
    ys = np.arange(y0, y1 + 1e-6, res)
    gx, gy = np.meshgrid(xs, ys)
    z = heights(gx, gy)
    nx, ny = len(xs), len(ys)
    r, c = np.meshgrid(np.arange(ny - 1), np.arange(nx - 1), indexing="ij")
    a = (r * nx + c).ravel()
    b, d = a + 1, a + nx
    e = d + 1
    tris = np.concatenate([np.stack([a, e, b], 1), np.stack([a, d, e], 1)])
    if hole is not None:
        cx = gx.ravel()[tris].mean(1)
        cy = gy.ravel()[tris].mean(1)
        hx0, hy0, hx1, hy1 = hole
        inside = (cx > hx0) & (cx < hx1) & (cy > hy0) & (cy < hy1)
        tris = tris[~inside]
    pos = np.column_stack([gx.ravel(), gy.ravel(), z.ravel()])
    mb.add("Terrain_Grass", pos, pos[:, :2].copy(), tris, landcover.to_colors(weights))

    # skirts along the tile border hide cracks against neighbours of a different resolution
    ring = np.concatenate([
        np.column_stack([xs, np.full(nx, y0)]), np.column_stack([np.full(ny, x1), ys])[1:],
        np.column_stack([xs[::-1], np.full(nx, y1)])[1:], np.column_stack([np.full(ny, x0), ys[::-1]])[1:]])
    zr = heights(ring[:, 0], ring[:, 1])
    walls(mb, "Terrain_Grass", ring, zr, zr - SKIRT, outward=True, color=(0, 0, 0, 255))
    return z


def far_cover(z, res, osm_covered):
    """No OSM out there: fields with meadow patches, woods where GLO-30 sticks out above its surroundings."""
    relief = z - ndimage.gaussian_filter(z, 80.0 / res)
    w = np.zeros(z.shape + (3,), dtype=np.float32)
    rng = np.random.default_rng(int(abs(z.sum())) % 2**32)
    patches = ndimage.gaussian_filter(rng.random(z.shape), 150.0 / res)
    patches = (patches - patches.min()) / max(np.ptp(patches), 1e-6)
    w[..., 0] = patches > 0.55
    w[..., 1] = patches <= 0.55
    w[..., 2] = np.clip((relief - 3.0) / 4.0, 0, 1)
    w[..., 2] = ndimage.gaussian_filter(w[..., 2], 1.0)
    w[..., :2] *= (1 - w[..., 2:3])
    return np.where(osm_covered[..., None], 0, w)


def forest_shells(mb, forests, heights, rng):
    """Extruded canopy volumes for woods too far away for individual trees."""
    for poly in forests:
        for part in getattr(poly, "geoms", [poly]):
            if not isinstance(part, shapely.Polygon) or part.area < 400:
                continue
            part = part.buffer(-2.0).simplify(3.0)
            if not isinstance(part, shapely.Polygon) or part.is_empty:
                continue
            h = rng.uniform(*CANOPY_HEIGHT)
            ring = np.asarray(part.exterior.coords)
            zg = heights(ring[:, 0], ring[:, 1])
            top = float(np.median(zg)) + h
            walls(mb, "Canopy_Far", ring, np.full(len(ring), top) + rng.uniform(-1.5, 1.5, len(ring)), zg - 1.0,
                  outward=True)
            verts, tris = triangulate_polygon(part)
            if len(tris):
                mb.add("Canopy_Far", np.column_stack([verts, np.full(len(verts), top)]), verts, tris)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser()
    parser.add_argument("--detail", default="bergedorf_core", choices=sorted(geo.AREAS))
    parser.add_argument("--radius", type=float, default=13000.0)
    parser.add_argument("--tree-ring", type=float, default=1000.0,
                        help="tiles within this distance of the detailed area get individual measured trees")
    parser.add_argument("--outer", type=float, default=60000.0, help="flat ground ring out to this distance")
    parser.add_argument("--geodata", default=os.path.join(here, "..", "..", "GeoData"))
    args = parser.parse_args()
    detail = geo.AREAS[args.detail]
    hole = (detail.x_min, detail.y_min, detail.x_max, detail.y_max)
    # tile grid aligned with the detailed area so it is exactly one hole
    R = args.radius
    span = geo.Area("horizon", detail.origin_e, detail.origin_n, -R, R, -R, R, tile_size=TILE)
    out_dir = os.path.abspath(os.path.join(args.geodata, "build", "horizon"))
    os.makedirs(out_dir, exist_ok=True)

    data = osm.load(os.path.join(args.geodata, "osm", "bergedorf.osm.pbf"), span)
    log(f"OSM: {len(data.buildings)} buildings, {len(data.areas)} areas")
    cover = landcover.LandCover(data.areas)
    # extent of the OSM extract (geodata bbox) in world coordinates; beyond it there is no land use / buildings
    e0, n0, e1, n1 = 569599, 5917282, 588427, 5932054
    osm_box = (e0 - detail.origin_e, detail.origin_n - n1, e1 - detail.origin_e, detail.origin_n - n0)
    forests = [g for _, t, g in data.areas if landcover.classify(t) == landcover.FOREST]
    water_polys = [g for _, t, g in data.areas
                   if t.get("natural") == "water" or t.get("waterway") in {"riverbank", "dock"} or "water" in t]
    roads_lines = [shapely.LineString(w.xy) for w in data.roads]
    rng = np.random.default_rng(3)

    buildings_by_tile = {}
    for b in data.buildings:
        p = b[2].representative_point()
        if hole[0] <= p.x < hole[2] and hole[1] <= p.y < hole[3]:
            continue
        key = (int(np.floor((p.x + R) / TILE)), int(np.floor((p.y + R) / TILE)))
        buildings_by_tile.setdefault(key, []).append(b)

    tiles, trees = [], []
    for ix, iy, x0, y0, x1, y1 in span.tiles():
        bounds = (x0, y0, x1, y1)
        if bounds == hole:
            continue
        dist = rect_distance(bounds, hole)
        res = resolution(dist)
        hgrid = tile_dem(bounds, detail, args.geodata)
        smooth = ndimage.uniform_filter(hgrid.z.astype(np.float64), size=int(res))  # area average, no aliasing
        heights = dem.HeightGrid(smooth.astype(np.float32), hgrid.x0, hgrid.y0, hgrid.res).sample
        box = shapely.box(*bounds)

        # water: flat surfaces, terrain pushed below them
        tile_water = [g.intersection(box.buffer(res)) for g in water_polys if g.intersects(box)]
        bodies = []
        for g in tile_water:
            for part in getattr(g, "geoms", [g]):
                if isinstance(part, shapely.Polygon) and part.area > 200:
                    bodies.append(paths.WaterBody(part, paths._water_level(part, hgrid)))

        def tile_heights(x, y, bodies=bodies, base=heights):
            z = base(x, y)
            for body in bodies:
                inside = shapely.contains_xy(body.polygon, x, y)
                z = np.where(inside, np.minimum(z, body.level - 1.0), z)
            return z

        xs = np.arange(x0, x1 + 1e-6, res)
        ys = np.arange(y0, y1 + 1e-6, res)
        in_osm = rect_distance(bounds, osm_box) == 0
        weights = cover.weights(xs, ys, res, blur_m=res * 1.2)
        mb = MeshBuilder()
        gx, gy = np.meshgrid(xs, ys)
        if not (osm_box[0] <= x0 and x1 <= osm_box[2] and osm_box[1] <= y0 and y1 <= osm_box[3]):
            covered = (gx >= osm_box[0]) & (gx <= osm_box[2]) & (gy >= osm_box[1]) & (gy <= osm_box[3])
            weights = weights + far_cover(heights(gx, gy), res, covered)
        grid_mesh(mb, tile_heights, bounds, res, hole if dist == 0 else None, weights)

        for body in bodies:
            clipped = body.polygon.intersection(box)
            for part in getattr(clipped, "geoms", [clipped]):
                if isinstance(part, shapely.Polygon) and not part.is_empty:
                    verts, tris = triangulate_polygon(part)
                    if len(tris):
                        mb.add("Water", np.column_stack([verts, np.full(len(verts), body.level)]), verts, tris)

        if in_osm:
            buildings.build(mb, buildings_by_tile.get((ix, iy), []), hgrid)

        # vegetation: measured trees near the detailed area, canopy shells elsewhere
        tile_forests = [g.intersection(box) for g in forests if g.intersects(box)]
        tile_forests = [g for g in tile_forests if not g.is_empty]
        placed_trees = False
        if dist <= args.tree_ring:
            bmask = terrain._rasterize(hgrid, shapely.union_all([g for _, _, g in buildings_by_tile.get((ix, iy), [])])
                                       if buildings_by_tile.get((ix, iy)) else shapely.Polygon())
            road_geo = shapely.union_all([ln.buffer(4.0) for ln in roads_lines if ln.intersects(box)]) \
                if roads_lines else shapely.Polygon()
            wmask = terrain._rasterize(hgrid, shapely.union_all([b.polygon for b in bodies]) if bodies else shapely.Polygon())
            found = canopy.detect(hgrid, detail, args.geodata, bmask, terrain._rasterize(hgrid, road_geo), wmask)
            if found is not None and found.covered.mean() > 0.5:
                sel = found.trees[found.trees[:, 2] >= FAR_TREE_MIN_HEIGHT]
                sel = sel[(sel[:, 0] >= x0) & (sel[:, 0] < x1) & (sel[:, 1] >= y0) & (sel[:, 1] < y1)]
                for x, y, h, crown in sel:
                    model = "conifer" if rng.random() < 0.12 else "broadleaf"
                    if model == "conifer":
                        crown = min(crown, h * 0.45)
                    trees.append(vegetation.Plant(model, x, y, crown, h, 0.0, "far_canopy"))
                placed_trees = True
        if not placed_trees and dist <= args.tree_ring and tile_forests:
            # close by but no surface model (Schleswig-Holstein / Niedersachsen): scatter trees in the OSM woods
            blocked = shapely.union_all([ln.buffer(5.0) for ln in roads_lines if ln.intersects(box)] or [shapely.Polygon()])
            for g in tile_forests:
                for x, y in vegetation._poisson_points(g.difference(blocked), 7.0, rng, keep=0.85):
                    h = rng.uniform(15, 26)
                    if rng.random() < 0.3:
                        trees.append(vegetation.Plant("conifer", x, y, h * 0.35, h, 0.0, "far_wood"))
                    else:
                        trees.append(vegetation.Plant("broadleaf", x, y, h * 0.45, h, 0.0, "far_wood"))
            placed_trees = True
        if not placed_trees and tile_forests:
            forest_shells(mb, tile_forests, heights, rng)

        if mb.is_empty():
            continue
        pivot = ((x0 + x1) / 2, (y0 + y1) / 2, 0.0)
        name = f"horizon_{ix}_{iy}"
        mb.write(os.path.join(out_dir, name + ".dgmesh"), pivot)
        counts = {k: v["count"] for k, v in mb.sections.items()}
        tiles.append({"name": name, "file": name + ".dgmesh", "pivot_cm": [p * 100 for p in pivot],
                      "bounds_m": bounds, "sections": counts})
        log(f"{name}: res {res:.0f} m, {sum(counts.values())} verts, {len(trees)} trees so far")

    tiles.append(outer_ring(out_dir, R, args.outer))
    log(f"outer ring out to {args.outer / 1000:.0f} km")
    tree_z = _tree_heights(trees, detail, args.geodata)
    vegetation.write(os.path.join(out_dir, "vegetation.json"), trees, tree_z)
    manifest = {"area": "horizon", "detail": args.detail, "origin_utm32": [detail.origin_e, detail.origin_n],
                "tiles": tiles, "attribution": ["© OpenStreetMap contributors (ODbL)",
                                                "Freie und Hansestadt Hamburg, LGV (dl-de/by-2.0)",
                                                "LGLN (2025), CC BY 4.0", "Contains modified Copernicus data"]}
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    log(f"wrote {len(tiles)} horizon tiles, {len(trees)} trees to {out_dir}")


def outer_ring(out_dir, inner, outer, z=0.5, res=1000.0):
    """Flat marshland around everything (Elbe valley ~0-3 m NHN) so the ground reaches the horizon."""
    mb = MeshBuilder()
    xs = np.arange(-outer, outer + 1e-6, res)
    gx, gy = np.meshgrid(xs, xs)
    n = len(xs)
    r, c = np.meshgrid(np.arange(n - 1), np.arange(n - 1), indexing="ij")
    a = (r * n + c).ravel()
    b, d = a + 1, a + n
    e = d + 1
    tris = np.concatenate([np.stack([a, e, b], 1), np.stack([a, d, e], 1)])
    cx, cy = gx.ravel()[tris].mean(1), gy.ravel()[tris].mean(1)
    tris = tris[(np.abs(cx) > inner) | (np.abs(cy) > inner)]
    pos = np.column_stack([gx.ravel(), gy.ravel(), np.full(gx.size, z)])
    rng = np.random.default_rng(5)
    w = np.zeros((gx.size, 3), dtype=np.float32)
    w[:, 0] = rng.random(gx.size) * 0.7  # meadow/crops vs soil, blurred by the vertex interpolation
    w[:, 1] = 1 - w[:, 0]
    mb.add("Terrain_Grass", pos, pos[:, :2].copy(), tris, landcover.to_colors(w))
    mb.write(os.path.join(out_dir, "horizon_outer.dgmesh"), (0.0, 0.0, 0.0))
    return {"name": "horizon_outer", "file": "horizon_outer.dgmesh", "pivot_cm": [0.0, 0.0, 0.0],
            "bounds_m": [-outer, -outer, outer, outer], "sections": {k: v["count"] for k, v in mb.sections.items()}}


def _tree_heights(trees, detail, geodata):
    """Terrain height under each tree (grouped per 2 km tile to reuse one DEM per tile)."""
    out = np.zeros(len(trees))
    groups = {}
    for i, t in enumerate(trees):
        groups.setdefault((int(np.floor(t.x / TILE)), int(np.floor(t.y / TILE))), []).append(i)
    for (gx, gy), idx in groups.items():
        xs = np.array([trees[i].x for i in idx])
        ys = np.array([trees[i].y for i in idx])
        g = tile_dem((xs.min() - 1, ys.min() - 1, xs.max() + 1, ys.max() + 1), detail, geodata)
        out[idx] = g.sample(xs, ys)
    return out


if __name__ == "__main__":
    main()
