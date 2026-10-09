"""Compiles the low-detail surroundings of a region into world data tiles, so the streamed world reaches the horizon.

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/build_horizon_world.py <region> [--radius 13000] [--outer 60000]

Output: <data root>/world/<region>/horizon/h_<ix>_<iy>.tgtile, h_outer.tgtile and horizon.json (tile list). The game
loads all of them at start, at the lowest detail and without collision.

Horizon tiles are 2 km square. Their terrain grid gets coarser with distance (25, 50 and 125 m, all dividing the 250 m
world tiles, so the region's edge always falls on grid lines); grid points inside the region are marked as holes and
the region's own tiles fill them. Content: terrain (DGM1 inside the geodata bbox, Copernicus GLO-30 beyond), land
cover, water, major roads, OSM buildings as plain extrusions and woods as extruded canopy blocks. A flat ring of
marshland continues the ground to 60 km.
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
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from build_horizon import CANOPY_HEIGHT, far_cover, rect_distance, tile_dem  # noqa: E402
from build_world import building_footprints, write_building  # noqa: E402
from osmimport import dem, geo, landcover, osm, paths, vegetation, worldtile  # noqa: E402

TILE = 2000.0
OUTER_CELL = 1000.0
ROAD_LIFT = 0.25
# Woods closer than this to the region get single trees (the scanned models); further out a canopy block is enough.
TREE_RING = 1500.0
TREE_SPACING = 9.0
# Road width (m) by OSM highway class; smaller roads aren't visible from kilometres away.
ROAD_WIDTHS = {"motorway": 14.0, "trunk": 11.0, "primary": 9.0, "secondary": 7.5, "tertiary": 6.5,
               "motorway_link": 6.0, "trunk_link": 6.0, "primary_link": 6.0}
# Extent of the OSM extract (UTM 32N); beyond it there are no buildings or land use, only terrain.
OSM_EXTENT_UTM = (569599, 5917282, 588427, 5932054)


def log(message, start=[time.time()]):
    """Prints a message with the seconds since the start."""
    print(f"[{time.time() - start[0]:6.1f}s] {message}", flush=True)


def cell_size(distance):
    """Terrain grid spacing for a tile this far from the region (m)."""
    if distance < 3000:
        return 25.0
    if distance < 7000:
        return 50.0
    return 125.0


def horizon_tiles(region, radius):
    """(ix, iy, bounds) of the 2 km tiles out to the radius, leaving out tiles entirely inside the region."""
    count = int(np.ceil(radius / TILE))
    for iy in range(-count, count):
        for ix in range(-count, count):
            bounds = (ix * TILE, iy * TILE, (ix + 1) * TILE, (iy + 1) * TILE)
            inside = (bounds[0] >= region.x_min and bounds[2] <= region.x_max
                      and bounds[1] >= region.y_min and bounds[3] <= region.y_max)
            if not inside:
                yield ix, iy, bounds


def hole_mask(gx, gy, hole):
    """Grid points strictly inside the hole rectangle; the game leaves out every cell touching one."""
    x0, y0, x1, y1 = hole
    return (gx > x0 + 1e-3) & (gx < x1 - 1e-3) & (gy > y0 + 1e-3) & (gy < y1 - 1e-3)


def water_bodies(water_polygons, box, cell, height_grid):
    """Water surfaces in the tile with their levels."""
    bodies = []
    for polygon in water_polygons:
        if not polygon.intersects(box):
            continue
        clipped = polygon.intersection(box.buffer(cell))
        for part in getattr(clipped, "geoms", [clipped]):
            if isinstance(part, shapely.Polygon) and part.area > 400:
                bodies.append(paths.WaterBody(part, paths._water_level(part, height_grid)))
    return bodies


def lowered_under_water(terrain, gx, gy, bodies):
    """Terrain pushed a metre below each water level, so the flat water surface covers it."""
    for body in bodies:
        inside = shapely.contains_xy(body.polygon, gx, gy)
        terrain = np.where(inside, np.minimum(terrain, body.level - 1.0), terrain)
    return terrain


def major_roads(data, box):
    """Buffered outlines of the roads visible from far away, merged."""
    pieces = []
    for way in data.roads:
        width = ROAD_WIDTHS.get(way.tags.get("highway"))
        if width is None or len(way.xy) < 2:
            continue
        line = shapely.LineString(way.xy)
        if line.intersects(box):
            pieces.append(line.buffer(width / 2.0, cap_style="flat"))
    if not pieces:
        return shapely.Polygon()
    return shapely.union_all(pieces).simplify(1.0)


def add_canopy_blocks(writer, forests, heights, rng):
    """Woods as extruded canopy blocks: a silhouette on the horizon where single trees would be too many."""
    for polygon in forests:
        for part in getattr(polygon, "geoms", [polygon]):
            if not isinstance(part, shapely.Polygon) or part.area < 2000:
                continue
            part = part.buffer(-3.0).simplify(6.0)
            if not isinstance(part, shapely.Polygon) or part.is_empty or part.area < 1000:
                continue
            ring = np.asarray(part.exterior.coords)
            ground = heights.sample(ring[:, 0], ring[:, 1])
            base_z = float(np.min(ground)) - 1.0
            canopy_top = float(np.median(ground)) + rng.uniform(*CANOPY_HEIGHT)
            writer.add_building(0, "Canopy_Far", "Canopy_Far", worldtile.ROOF_FLAT, 128, 128, base_z,
                                canopy_top - base_z, part)


def add_forest_trees(writer, forests, heights, rng):
    """Woods near the region as single trees on a jittered grid: from a kilometre away blocks read as green walls."""
    for polygon in forests:
        for x, y in vegetation._poisson_points(polygon, TREE_SPACING, rng, keep=0.85):
            height = rng.uniform(15.0, 26.0)
            model, crown = "broadleaf", height * 0.45
            if rng.random() < 0.3:
                model, crown = "conifer", height * 0.35
            z = float(heights.sample(x, y))
            writer.add_plant(model, x, y, z, float(rng.uniform(0, 360)), crown, height, 0.0)


def build_tile(bounds, region, data, cover, water_polygons, forests, buildings_by_tile, key, osm_box, rng):
    """One horizon tile, or None when it would be empty."""
    distance = rect_distance(bounds, (region.x_min, region.y_min, region.x_max, region.y_max))
    cell = cell_size(distance)
    x0, y0, x1, y1 = bounds
    height_grid = tile_dem(bounds, region, data_root.geodata_dir())
    smooth = ndimage.uniform_filter(height_grid.z.astype(np.float64), size=max(1, int(cell / height_grid.res)))
    heights = dem.HeightGrid(smooth.astype(np.float32), height_grid.x0, height_grid.y0, height_grid.res)
    box = shapely.box(*bounds)
    # Everything drawn on top of the terrain stays out of the region, which its own tiles cover in full detail.
    outside = box.difference(shapely.box(region.x_min, region.y_min, region.x_max, region.y_max))
    xs = np.arange(x0, x1 + 1e-6, cell)
    ys = np.arange(y0, y1 + 1e-6, cell)
    gx, gy = np.meshgrid(xs, ys)

    bodies = water_bodies(water_polygons, box, cell, height_grid)
    terrain = lowered_under_water(heights.sample(gx, gy), gx, gy, bodies)
    holes = hole_mask(gx, gy, (region.x_min, region.y_min, region.x_max, region.y_max))
    weights = cover.weights(xs, ys, cell, blur_m=cell * 1.2)
    fully_in_osm = osm_box[0] <= x0 and x1 <= osm_box[2] and osm_box[1] <= y0 and y1 <= osm_box[3]
    if not fully_in_osm:
        covered = (gx >= osm_box[0]) & (gx <= osm_box[2]) & (gy >= osm_box[1]) & (gy <= osm_box[3])
        weights = weights + far_cover(terrain, cell, covered)

    writer = worldtile.TileWriter(bounds)
    writer.set_grid(terrain, terrain, np.clip(weights, 0, 1), cell, holes)
    for body in bodies:
        writer.add_surface("Water", body.polygon.intersection(outside), worldtile.HEIGHT_CONSTANT, [body.level])
    writer.add_surface("Road_Asphalt", major_roads(data, box).intersection(outside), worldtile.HEIGHT_TERRAIN,
                       [ROAD_LIFT])
    for osm_id, tags, footprint in buildings_by_tile.get(key, []):
        write_building(writer, osm_id, tags, footprint, heights)
    tile_forests = [g.intersection(outside) for g in forests if g.intersects(outside)]
    tile_forests = [g for g in tile_forests if not g.is_empty]
    if distance < TREE_RING:
        add_forest_trees(writer, tile_forests, heights, rng)
    else:
        add_canopy_blocks(writer, tile_forests, heights, rng)
    return writer, cell


def outer_ring(inner, outer):
    """Flat marshland (the Elbe valley is 0 to 3 m above sea level) from the horizon tiles out to the outer edge."""
    bounds = (-outer, -outer, outer, outer)
    xs = np.arange(-outer, outer + 1e-6, OUTER_CELL)
    gx, gy = np.meshgrid(xs, xs)
    terrain = np.full(gx.shape, 0.5)
    rng = np.random.default_rng(5)
    weights = np.zeros(gx.shape + (3,), dtype=np.float32)
    weights[..., 0] = rng.random(gx.shape) * 0.7
    weights[..., 1] = 1.0 - weights[..., 0]
    writer = worldtile.TileWriter(bounds)
    writer.set_grid(terrain, terrain, weights, OUTER_CELL, hole_mask(gx, gy, (-inner, -inner, inner, inner)))
    return writer


def buildings_per_tile(data, region, radius):
    """Building footprints outside the region, keyed by the horizon tile of their representative point."""
    out = {}
    for osm_id, tags, footprint in building_footprints(data):
        point = footprint.representative_point()
        if region.x_min <= point.x < region.x_max and region.y_min <= point.y < region.y_max:
            continue
        if abs(point.x) >= radius or abs(point.y) >= radius:
            continue
        key = (int(np.floor(point.x / TILE)), int(np.floor(point.y / TILE)))
        out.setdefault(key, []).append((osm_id, tags, footprint))
    return out


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region", choices=sorted(geo.AREAS))
    parser.add_argument("--radius", type=float, default=13000.0, help="horizon tiles out to this distance (m)")
    parser.add_argument("--outer", type=float, default=60000.0, help="flat ground ring out to this distance (m)")
    args = parser.parse_args()
    region = geo.AREAS[args.region]
    radius = float(np.ceil(args.radius / TILE) * TILE)
    out_dir = os.path.join(data_root.world_dir(region.name), "horizon")
    os.makedirs(out_dir, exist_ok=True)

    span = geo.Area("horizon", region.origin_e, region.origin_n, -radius, radius, -radius, radius, tile_size=TILE)
    data = osm.load(os.path.join(data_root.geodata_dir(), "osm", "bergedorf.osm.pbf"), span)
    log(f"OSM: {len(data.roads)} roads, {len(data.buildings)} buildings, {len(data.areas)} areas")
    cover = landcover.LandCover(data.areas)
    e0, n0, e1, n1 = OSM_EXTENT_UTM
    osm_box = (e0 - region.origin_e, region.origin_n - n1, e1 - region.origin_e, region.origin_n - n0)
    forests = [g for _, tags, g in data.areas if landcover.classify(tags) == landcover.FOREST]
    water_polygons = [g for _, tags, g in data.areas
                      if tags.get("natural") == "water" or tags.get("waterway") in {"riverbank", "dock"}
                      or "water" in tags]
    buildings_by_tile = buildings_per_tile(data, region, radius)
    rng = np.random.default_rng(3)

    tiles = []
    for ix, iy, bounds in horizon_tiles(region, radius):
        writer, cell = build_tile(bounds, region, data, cover, water_polygons, forests, buildings_by_tile, (ix, iy),
                                  osm_box, rng)
        name = f"h_{ix}_{iy}.tgtile"
        writer.write(os.path.join(out_dir, name))
        tiles.append({"file": name, "bounds_m": list(bounds), "cell_m": cell})
        log(f"{name}: {cell:.0f} m grid, {len(writer.buildings)} buildings and canopy blocks, {len(writer.plants)} trees, "
            f"{len(writer.surfaces)} surfaces")
    outer = outer_ring(radius, args.outer)
    outer.write(os.path.join(out_dir, "h_outer.tgtile"))
    tiles.append({"file": "h_outer.tgtile", "bounds_m": [-args.outer, -args.outer, args.outer, args.outer],
                  "cell_m": OUTER_CELL})
    with open(os.path.join(out_dir, "horizon.json"), "w") as f:
        json.dump({"format": worldtile.VERSION, "region": region.name, "radius_m": radius, "tiles": tiles}, f,
                  indent=1)
    size = sum(os.path.getsize(os.path.join(out_dir, t["file"])) for t in tiles)
    log(f"wrote {len(tiles)} horizon tiles ({size / 1e6:.1f} MB) to {out_dir}")


if __name__ == "__main__":
    main()
