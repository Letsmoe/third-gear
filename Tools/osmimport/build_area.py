"""Builds mesh tiles + manifest for an area.

  .venv/bin/python -I build_area.py <area> [--geodata ../../GeoData] [--out ../../GeoData/build]

Output: <out>/<area>/tile_<ix>_<iy>.dgmesh and <out>/<area>/manifest.json (read by Scripts/import_osm_area.py).
"""
import argparse
import json
import os
import pickle
import sys
import time

import numpy as np
import shapely

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from osmimport import buildings, canopy, dem, geo, landcover, osm, paths, roads, terrain, vegetation  # noqa: E402
from osmimport.mesh import MeshBuilder, drape, ribbon, walls  # noqa: E402
from osmimport.streets import assumptions  # noqa: E402

MARKING_STYLE = {  # kind -> (width m, dash (on, off) or None); RMS dimensions, see streets/assumptions.py
    "dash_urban": (0.12, (3.0, 6.0)),
    "dash_rural": (0.12, (4.0, 8.0)),
    "solid": (0.12, None),
    "edge": (0.25, None),
    "guide": (0.12, assumptions.GUIDE_LINE_DASH),
    "edge_guide": (0.25, assumptions.EDGE_GUIDE_DASH),
    "cycle_exclusive": (0.25, None),
    "cycle_advisory": (0.12, assumptions.ADVISORY_CYCLE_LINE_DASH),
    "cycle_furt": (0.25, assumptions.CYCLE_FURT_DASH),
    "turn_lane_dash": (0.25, assumptions.TURN_LANE_DASH),
    "turn_lane_solid": (0.25, None),
    "hatch": (assumptions.HATCH_STRIPE_WIDTH, None),
}
SURFACE_SECTIONS = {"asphalt": "Road_Asphalt", "pavers": "Road_Pavers", "cobble": "Road_Cobble"}


def log(msg, t0=[time.time()]):
    print(f"[{time.time() - t0[0]:6.1f}s] {msg}", flush=True)


def bridge_height_fn(way, net: roads.RoadNetwork):
    line = shapely.LineString(way.xy)
    z0 = float(net.height.sample(*way.xy[0]))
    z1 = float(net.height.sample(*way.xy[-1]))
    length = max(line.length, 1e-6)

    def fn(x, y):
        pts = shapely.points(np.column_stack([x, y]))
        t = shapely.line_locate_point(line, pts) / length
        return z0 + (z1 - z0) * t

    return fn


def find_start(net: roads.RoadNetwork):
    """A spot on a decent road close to the origin, facing along the road."""
    best = None
    for w in net.ways:
        if w.tags.get("highway") not in {"residential", "tertiary", "secondary", "unclassified"}:
            continue
        if not roads._is_ground(w):
            continue
        line = shapely.LineString(w.xy)
        if line.length < 60:
            continue
        mid = line.interpolate(0.5, normalized=True)
        d = np.hypot(mid.x, mid.y)
        if best is None or d < best[0]:
            best = (d, w, line)
    if best is None:
        return {"x": 0.0, "y": 0.0, "z": float(net.height.sample(0.0, 0.0)), "yaw": 0.0}
    _, w, line = best
    s = line.length * 0.5
    p = line.interpolate(s)
    q = line.interpolate(s + 5.0)
    yaw = float(np.degrees(np.arctan2(q.y - p.y, q.x - p.x)))  # Unreal yaw: +x = 0, +y (south) = 90
    # drive on the right: shift half a lane to the right of the centre line
    right = np.array([-(q.y - p.y), q.x - p.x]) / max(np.hypot(q.x - p.x, q.y - p.y), 1e-6)
    right = np.array([-right[0], -right[1]]) * -1  # right of travel in a y-south frame
    off = net.widths[w.id] / 4
    x, y = p.x + right[0] * off, p.y + right[1] * off
    return {"x": float(x), "y": float(y), "z": float(net.height.sample(x, y)), "yaw": yaw, "road": w.tags.get("name", "")}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser()
    parser.add_argument("area", choices=sorted(geo.AREAS))
    parser.add_argument("--geodata", default=os.path.join(here, "..", "..", "GeoData"))
    parser.add_argument("--out", default=None)
    parser.add_argument("--reuse", action="store_true",
                        help="reuse OSM/DEM/road/terrain results cached by the previous run (fast iteration)")
    parser.add_argument("--skip-tiles", action="store_true", help="only write manifest + vegetation")
    args = parser.parse_args()
    area = geo.AREAS[args.area]
    out_dir = os.path.abspath(args.out or os.path.join(args.geodata, "build", area.name))
    os.makedirs(out_dir, exist_ok=True)

    cache_path = os.path.join(out_dir, "cache.pkl")
    if args.reuse and os.path.exists(cache_path):
        with open(cache_path, "rb") as f:
            data, heights, building_union, net, surfaces, ground = pickle.load(f)
        log("reused cached OSM/DEM/roads/terrain")
    else:
        data = osm.load(os.path.join(args.geodata, "osm", "bergedorf.osm.pbf"), area)
        log(f"OSM: {len(data.roads)} roads, {len(data.buildings)} buildings, {len(data.points)} points")
        heights = dem.build_mosaic(area, args.geodata)
        log(f"DEM {heights.z.shape}")

        building_union = shapely.union_all([g for _, _, g in data.buildings]) if data.buildings else None
        net = roads.build(data, heights, building_union)
        log(f"roads: {len(net.ways)} ways, ground area {net.ground.area:.0f} m², pavement {net.pavement.area:.0f} m², "
            f"{len(net.markings)} marking lines, {len(net.bridges)} bridges")
        surfaces = paths.build(data, heights, net.ground, net.pavement, building_union)
        log(f"paths: paved {surfaces.paved.area:.0f} m², unpaved {surfaces.unpaved.area:.0f} m², "
            f"{len(surfaces.water)} water bodies")
        ground = terrain.conform(heights, net, surfaces.water)
        log("terrain conformed")
        with open(cache_path, "wb") as f:
            pickle.dump((data, heights, building_union, net, surfaces, ground), f, protocol=pickle.HIGHEST_PROTOCOL)

    paths_union = shapely.union_all([surfaces.paved, surfaces.unpaved])
    water_union = shapely.union_all([wb.polygon for wb in surfaces.water]) if surfaces.water else shapely.Polygon()
    hard_union = shapely.union_all([net.ground, net.pavement, surfaces.paved])
    canopy_data = canopy.detect(heights, area, args.geodata,
                                terrain._rasterize(heights, building_union or shapely.Polygon()),
                                terrain._rasterize(heights, hard_union), terrain._rasterize(heights, water_union))
    if canopy_data is not None:
        log(f"canopy: {len(canopy_data.trees)} trees, {len(canopy_data.shrubs)} shrub points from the surface model")
    plants, plant_z, plant_counts = vegetation.build(
        data, area, ground, net.ground, net.pavement, paths_union, water_union, building_union,
        os.path.join(args.geodata, "raw", "strassenbaeume", "strassenbaeume_bbox.geojson"), canopy_data)
    vegetation.write(os.path.join(out_dir, "vegetation.json"), plants, plant_z)
    log(f"vegetation: {len(plants)} plants {plant_counts}")

    road_z = net.height.sample
    cover = landcover.LandCover(data.areas)
    canopy_grid = canopy_data.grid if canopy_data is not None else None
    tiles = []
    buildings_by_tile = {}
    for b in data.buildings:
        p = b[2].representative_point()
        key = (int(np.floor((p.x - area.x_min) / area.tile_size)), int(np.floor((p.y - area.y_min) / area.tile_size)))
        buildings_by_tile.setdefault(key, []).append(b)

    for ix, iy, x0, y0, x1, y1 in ([] if args.skip_tiles else area.tiles()):
        bounds = (x0, y0, x1, y1)
        mb = MeshBuilder()
        terrain.build_tile(mb, ground, bounds, cover=cover, canopy=canopy_grid)
        for kind, poly in net.surfaces.items():
            drape(mb, SURFACE_SECTIONS[kind], poly, road_z, bounds)
        pave = net.pavement.intersection(shapely.box(*bounds))
        drape(mb, "Pavement", pave, road_z, bounds, z_offset=roads.KERB_HEIGHT)
        for poly in getattr(pave, "geoms", [pave]):
            if isinstance(poly, shapely.Polygon) and not poly.is_empty:
                for ring in [poly.exterior, *poly.interiors]:
                    xy = np.asarray(ring.coords)
                    z = road_z(xy[:, 0], xy[:, 1])
                    walls(mb, "Kerb", xy, z + roads.KERB_HEIGHT, z - 0.1, outward=(ring is poly.exterior))
        box = shapely.box(*bounds)
        for kind, line in net.markings:
            clipped = line.intersection(box)
            width, dash = MARKING_STYLE[kind]
            for part in getattr(clipped, "geoms", [clipped]):
                if isinstance(part, shapely.LineString) and part.length > 0.5:
                    ribbon(mb, "Marking_White", part, width, road_z, roads.MARKING_LIFT, dash=dash)
        for way, poly in net.bridges:
            fn = bridge_height_fn(way, net)
            drape(mb, "Road_Asphalt", poly, fn, bounds)
            clipped = poly.intersection(box)
            for part in getattr(clipped, "geoms", [clipped]):
                if isinstance(part, shapely.Polygon) and not part.is_empty:
                    xy = np.asarray(part.exterior.coords)
                    z = fn(xy[:, 0], xy[:, 1])
                    walls(mb, "Bridge_Concrete", xy, z + 0.9, z - 0.9, outward=True)
        drape(mb, "Path_Paved", surfaces.paved, ground.sample, bounds, cell=1.0, z_offset=0.04)
        drape(mb, "Path_Gravel", surfaces.unpaved, ground.sample, bounds, cell=1.0, z_offset=0.04)
        for body in surfaces.water:
            drape(mb, "Water", body.polygon, lambda x, y, lv=body.level: np.full(np.shape(x), lv), bounds, cell=16.0)
        buildings.build(mb, buildings_by_tile.get((ix, iy), []), heights)

        if mb.is_empty():
            continue
        pivot = ((x0 + x1) / 2, (y0 + y1) / 2, 0.0)
        name = f"tile_{ix}_{iy}"
        mb.write(os.path.join(out_dir, name + ".dgmesh"), pivot)
        counts = {k: v["count"] for k, v in mb.sections.items()}
        tiles.append({"name": name, "file": name + ".dgmesh", "pivot_cm": [p * 100 for p in pivot],
                      "bounds_m": bounds, "sections": counts})
        log(f"{name}: {sum(counts.values())} verts ({', '.join(f'{k}={v}' for k, v in sorted(counts.items()))})")

    points = [{"id": p.id, "tags": p.tags, "x": p.x, "y": p.y, "z": float(ground.sample(p.x, p.y))}
              for p in data.points if area.x_min <= p.x < area.x_max and area.y_min <= p.y < area.y_max]
    manifest = {
        "area": area.name,
        "origin_utm32": [area.origin_e, area.origin_n],
        "tiles": tiles,
        "start": find_start(net),
        "points": points,
        "attribution": [
            "© OpenStreetMap contributors (ODbL)",
            "Freie und Hansestadt Hamburg, LGV (dl-de/by-2.0)",
            "LGLN (2025), CC BY 4.0",
        ],
    }
    if args.skip_tiles:
        log("--skip-tiles: manifest left unchanged")
        return
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    log(f"wrote {len(tiles)} tiles + manifest to {out_dir}; start {manifest['start']}")


if __name__ == "__main__":
    main()
