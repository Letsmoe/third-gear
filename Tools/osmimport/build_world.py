"""Compiles the world data tiles the game streams (see osmimport/worldtile.py for the format).

  Tools/osmimport/.venv/bin/python -I Tools/osmimport/build_world.py <region> [--reuse]

Output: <data root>/world/<region>/tile_<ix>_<iy>.tgtile and world.json (tile index, start pose, attribution).
Only data goes in: terrain and road heights, land cover, surface outlines, marking lines, building footprints with
their heights and styles, plant positions and street furniture (lamps, signal poles, signs). The game builds every mesh
from it while you drive. traffic.json next to world.json holds the signal junctions with their approaches, stop lines
and phases, and the speed limit of every road.

--output-name writes to another folder under world/ (copy cache.pkl there first to use --reuse), so a tile format
change can be tried without breaking the data other checkouts read.
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
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402
from build_area import MARKING_STYLE, SURFACE_SECTIONS, find_start  # noqa: E402
from osmimport import buildings, canopy, dem, furniture, geo, landcover, osm, paths, roads, terrain, vegetation  # noqa: E402
from osmimport import worldtile  # noqa: E402

GRID_CELL = 1.0
MARKING_STYLES = {"dash_urban": 0, "dash_rural": 1, "solid": 2, "edge": 3}
PATH_LIFT = 0.04
ATTRIBUTION = [
    "© OpenStreetMap contributors (ODbL)",
    "Freie und Hansestadt Hamburg, LGV (dl-de/by-2.0)",
    "LGLN (2025), CC BY 4.0",
    "Copernicus DEM GLO-30, © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018",
]


def log(message, start=[time.time()]):
    """Prints a message with the seconds since the start."""
    print(f"[{time.time() - start[0]:6.1f}s] {message}", flush=True)


def load_sources(area, geodata, cache_path, reuse):
    """OSM data, DEM, roads, surfaces and conformed terrain for the area, cached between runs."""
    if reuse and os.path.exists(cache_path):
        with open(cache_path, "rb") as f:
            log("reused cached OSM, DEM, roads and terrain")
            return pickle.load(f)
    data = osm.load(os.path.join(geodata, "osm", "bergedorf.osm.pbf"), area)
    log(f"OSM: {len(data.roads)} roads, {len(data.buildings)} buildings, {len(data.points)} points")
    heights = dem.build_mosaic(area, geodata)
    log(f"DEM {heights.z.shape}")
    building_union = shapely.union_all([g for _, _, g in data.buildings]) if data.buildings else None
    net = roads.build(data, heights, building_union)
    log(f"roads: {len(net.ways)} ways, {len(net.markings)} marking lines, {len(net.bridges)} bridges")
    surfaces = paths.build(data, heights, net.ground, net.pavement, building_union)
    ground = terrain.conform(heights, net, surfaces.water)
    log("terrain conformed")
    result = (data, heights, building_union, net, surfaces, ground)
    with open(cache_path, "wb") as f:
        pickle.dump(result, f, protocol=pickle.HIGHEST_PROTOCOL)
    return result


def detect_plants(data, area, geodata, heights, building_union, net, surfaces, ground):
    """Plant list (vegetation.build) and the canopy height grid used for leaf litter in the land cover."""
    paths_union = shapely.union_all([surfaces.paved, surfaces.unpaved])
    water_union = shapely.union_all([wb.polygon for wb in surfaces.water]) if surfaces.water else shapely.Polygon()
    hard_union = shapely.union_all([net.ground, net.pavement, surfaces.paved])
    canopy_data = canopy.detect(heights, area, geodata,
                                terrain._rasterize(heights, building_union or shapely.Polygon()),
                                terrain._rasterize(heights, hard_union), terrain._rasterize(heights, water_union))
    plants, plant_z, counts = vegetation.build(
        data, area, ground, net.ground, net.pavement, paths_union, water_union, building_union,
        os.path.join(geodata, "raw", "strassenbaeume", "strassenbaeume_bbox.geojson"), canopy_data)
    log(f"vegetation: {len(plants)} plants {counts}")
    return plants, plant_z, (canopy_data.grid if canopy_data is not None else None), counts


def tile_key(area, x, y):
    """(ix, iy) of the tile containing world point (x, y)."""
    return int(np.floor((x - area.x_min) / area.tile_size)), int(np.floor((y - area.y_min) / area.tile_size))


def write_grid(writer, bounds, ground, net, cover, canopy_grid):
    """Terrain and road heights and land cover on the tile's 1 m vertex grid (edges shared with neighbours)."""
    x0, y0, x1, y1 = bounds
    xs = np.arange(x0, x1 + 1e-6, GRID_CELL)
    ys = np.arange(y0, y1 + 1e-6, GRID_CELL)
    gx, gy = np.meshgrid(xs, ys)
    terrain_z = ground.sample(gx, gy)
    road_z = net.height.sample(gx, gy)
    weights = cover.weights(xs, ys, GRID_CELL, canopy_grid)
    writer.set_grid(terrain_z, road_z, weights, GRID_CELL)


def write_surfaces(writer, net, surfaces):
    """Road, pavement, path, water and bridge outlines with the height rule for each."""
    for kind, polygon in net.surfaces.items():
        writer.add_surface(SURFACE_SECTIONS[kind], polygon, worldtile.HEIGHT_ROAD)
    writer.add_surface("Pavement", net.pavement, worldtile.HEIGHT_ROAD, [roads.KERB_HEIGHT])
    writer.add_surface("Path_Paved", surfaces.paved, worldtile.HEIGHT_TERRAIN, [PATH_LIFT])
    writer.add_surface("Path_Gravel", surfaces.unpaved, worldtile.HEIGHT_TERRAIN, [PATH_LIFT])
    for body in surfaces.water:
        writer.add_surface("Water", body.polygon, worldtile.HEIGHT_CONSTANT, [body.level])
    for way, polygon in net.bridges:
        start, end = way.xy[0], way.xy[-1]
        z_start = float(net.height.sample(*start))
        z_end = float(net.height.sample(*end))
        writer.add_surface("Road_Asphalt", polygon, worldtile.HEIGHT_RAMP, [z_start, z_end, *start, *end])


def write_markings(writer, net):
    """Lane and edge marking lines."""
    for kind, line in net.markings:
        width, dash = MARKING_STYLE[kind]
        writer.add_marking("Marking_White", MARKING_STYLES[kind], line, width, dash)


def poi_ground_height(x, y, net, ground, on_pavement):
    """Foot height of a pole: pavement level where it stands on the pavement, else the conformed terrain."""
    if on_pavement:
        return float(net.height.sample(x, y)) + roads.KERB_HEIGHT
    return float(ground.sample(x, y))


def write_stop_lines(writers, junctions):
    """Painted stop lines (Haltlinie, 0.5 m wide) across each approach, just before the signal's line of sight."""
    for junction in junctions:
        for approach in junction["approaches"]:
            x0, y0, x1, y1 = approach["stop_line"]
            length = float(np.hypot(x1 - x0, y1 - y0))
            if length < 1.0:
                continue
            ux, uy = (x1 - x0) / length, (y1 - y0) / length
            back_x, back_y = -approach["direction"][0] * 0.25, -approach["direction"][1] * 0.25
            inset = 0.15
            line = shapely.LineString([(x0 + ux * inset + back_x, y0 + uy * inset + back_y),
                                       (x1 - ux * inset + back_x, y1 - uy * inset + back_y)])
            for writer in writers.values():
                if writer.box.intersects(line):
                    writer.add_marking("Marking_White", MARKING_STYLES["solid"], line, 0.5, None)


def write_furniture(writers, area, builder, net, ground):
    """Lamps, signal poles and signs into the tiles they stand in."""
    pavement = net.pavement
    shapely.prepare(pavement)

    def foot_z(x, y):
        return poi_ground_height(x, y, net, ground, pavement.contains(shapely.Point(x, y)))

    for lamp in builder.lamps:
        writer = writers.get(tile_key(area, lamp["x"], lamp["y"]))
        if writer is not None:
            mast = 7.0 if lamp["source"] == "osm" else 6.5
            writer.add_poi(worldtile.POI_LAMP, lamp["x"], lamp["y"], foot_z(lamp["x"], lamp["y"]), lamp["yaw"],
                           variant=0, param0=mast, param1=1.6, flags=1 if lamp["source"] == "osm" else 0)
    for head in builder.heads:
        writer = writers.get(tile_key(area, head["x"], head["y"]))
        if writer is not None:
            writer.add_poi(worldtile.POI_SIGNAL_HEAD, head["x"], head["y"], foot_z(head["x"], head["y"]), head["yaw"],
                           param0=3.4, link=head["approach"], flags=1 if head["side"] == "left" else 0)
    for sign in builder.signs:
        writer = writers.get(tile_key(area, sign["x"], sign["y"]))
        if writer is None:
            continue
        names = sign["names"]
        second = writer.names(names[1]) if len(names) > 1 else worldtile.NO_VARIANT
        writer.add_poi(worldtile.POI_SIGN, sign["x"], sign["y"], foot_z(sign["x"], sign["y"]), sign["yaw"],
                       variant=writer.names(names[0]), variant2=second)


def write_building(writer, osm_id, tags, footprint, ground):
    """One building footprint with its eave height, roof shape and look (decided by buildings.py's rules)."""
    btype, eave, roof = buildings.building_params(osm_id, tags, footprint)
    facade, roof_section = buildings.facade_style(osm_id, btype)
    rect = footprint.minimum_rotated_rectangle
    rectangular = isinstance(rect, shapely.Polygon) and rect.area > 0 and footprint.area / rect.area > 0.85 \
        and not footprint.interiors
    roof_shape = worldtile.ROOF_GABLED if roof == "gabled" and rectangular else worldtile.ROOF_FLAT
    if roof_shape == worldtile.ROOF_FLAT and roof_section == "Roof_Tiles":
        roof_section = "Roof_Flat"
    ring = np.asarray(footprint.exterior.coords)
    base_z = float(np.min(ground.sample(ring[:, 0], ring[:, 1])))
    tint = int(buildings._hash01(osm_id, 1) * 255)
    variation = int(buildings._hash01(osm_id, 2) * 255)
    rectangle = np.asarray(rect.exterior.coords)[:4] if roof_shape == worldtile.ROOF_GABLED else None
    writer.add_building(osm_id, facade, roof_section, roof_shape, tint, variation, base_z, float(eave), footprint,
                        rectangle)


def building_footprints(data):
    """(osm_id, tags, footprint) for every usable building polygon, simplified like buildings.py does."""
    for osm_id, tags, geom in data.buildings:
        for footprint in getattr(geom, "geoms", [geom]):
            if not isinstance(footprint, shapely.Polygon) or footprint.area < 4.0:
                continue
            footprint = shapely.make_valid(footprint.simplify(0.15))
            if isinstance(footprint, shapely.Polygon) and footprint.area >= 4.0:
                yield osm_id, tags, footprint


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("region", choices=sorted(geo.AREAS))
    parser.add_argument("--reuse", action="store_true", help="reuse the cached OSM, DEM, road and terrain step")
    parser.add_argument("--output-name", help="folder under world/ to write to (default: the region name)")
    args = parser.parse_args()
    area = geo.AREAS[args.region]
    geodata = data_root.geodata_dir()
    out_dir = data_root.world_dir(args.output_name or area.name)
    os.makedirs(out_dir, exist_ok=True)

    data, heights, building_union, net, surfaces, ground = load_sources(
        area, geodata, os.path.join(out_dir, "cache.pkl"), args.reuse)
    plants, plant_z, canopy_grid, plant_counts = detect_plants(data, area, geodata, heights, building_union, net, surfaces, ground)
    cover = landcover.LandCover(data.areas)

    builder = furniture.FurnitureBuilder(data, net, building_union)
    traffic = builder.build()
    log(f"furniture: {len(traffic['junctions'])} signal junctions, {len(builder.heads)} signal poles, "
        f"{len(builder.signs)} signs, {len(builder.lamps)} lamps")

    writers = {(ix, iy): worldtile.TileWriter((x0, y0, x1, y1)) for ix, iy, x0, y0, x1, y1 in area.tiles()}
    for (ix, iy), writer in writers.items():
        write_grid(writer, (writer.x0, writer.y0, writer.x1, writer.y1), ground, net, cover, canopy_grid)
        write_surfaces(writer, net, surfaces)
        write_markings(writer, net)
    write_stop_lines(writers, traffic["junctions"])
    write_furniture(writers, area, builder, net, ground)
    for osm_id, tags, footprint in building_footprints(data):
        point = footprint.representative_point()
        writer = writers.get(tile_key(area, point.x, point.y))
        if writer is not None:
            write_building(writer, osm_id, tags, footprint, ground)
    rng = np.random.default_rng(11)
    for plant, z in zip(plants, plant_z):
        writer = writers.get(tile_key(area, plant.x, plant.y))
        if writer is not None:
            writer.add_plant(plant.model, plant.x, plant.y, float(z), float(rng.uniform(0, 360)), plant.crown,
                             plant.height, plant.trunk)

    tiles = []
    for (ix, iy), writer in sorted(writers.items()):
        name = f"tile_{ix}_{iy}.tgtile"
        writer.write(os.path.join(out_dir, name))
        tiles.append({"ix": ix, "iy": iy, "file": name, "bounds_m": [writer.x0, writer.y0, writer.x1, writer.y1]})
    world = {
        "format": worldtile.VERSION,
        "region": area.name,
        "origin_utm32": [area.origin_e, area.origin_n],
        "tile_size_m": area.tile_size,
        "bounds_m": [area.x_min, area.y_min, area.x_max, area.y_max],
        "tiles": tiles,
        "start": find_start(net),
        "traffic": "traffic.json",
        "plants_by_source": plant_counts,
        "attribution": ATTRIBUTION,
    }
    with open(os.path.join(out_dir, "traffic.json"), "w") as f:
        json.dump(traffic, f, separators=(",", ":"))
    with open(os.path.join(out_dir, "world.json"), "w") as f:
        json.dump(world, f, indent=1)
    size = sum(os.path.getsize(os.path.join(out_dir, t["file"])) for t in tiles)
    log(f"wrote {len(tiles)} tiles ({size / 1e6:.1f} MB) to {out_dir}; start {world['start']}")


if __name__ == "__main__":
    main()
