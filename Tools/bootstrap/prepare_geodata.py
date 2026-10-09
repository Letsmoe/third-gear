"""Turns the raw downloads into the geodata layout the map pipeline reads, under <data root>/geodata.

  <data root>/venvs/osmimport/bin/python -I Tools/bootstrap/prepare_geodata.py [--force]

Layout written (what Tools/osmimport expects):
  osm/bergedorf.osm.pbf                      Hamburg, Schleswig-Holstein and Lower Saxony, clipped to the Bergedorf bbox
  osm/hamburg.osm.pbf                        the same, clipped to the city bbox
  raw/dgm5/*.tif                             5 m terrain in 1 km tiles, averaged from the Hamburg and Lower Saxony DGM1
  raw/copernicus_glo30/*.tif                 GLO-30 fallback (links)
  raw/terrain_5m.grid                        the 5 m terrain of the city box plus TERRAIN_GRID_MARGIN, gaps filled
  raw/strassenbaeume/strassenbaeume_bbox.geojson   street tree register of the city bbox (link)
  raw/strassenbaeume/street_trees.tsv        the register's fields the world builder uses, one tree per line

Every step writes a marker file and is skipped on the next run unless --force is given.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import warnings
import zipfile

import numpy as np
import rasterio
from rasterio.transform import from_origin

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data_root  # noqa: E402

TOOLS_DIR = os.path.join(data_root.REPO_ROOT, "Tools")
# Bergedorf bbox, the first project area, in WGS84 for its OSM clip (see Data/sources.toml).
BBOX_WGS84 = (10.05, 53.40, 10.33, 53.53)
# City bbox in UTM 32N kilometres and WGS84: all of Hamburg except the island of Neuwerk.
CITY_KM_EAST = (548, 589)
CITY_KM_NORTH = (5916, 5957)
CITY_WGS84 = (9.72, 53.385, 10.35, 53.76)
# The terrain is averaged over squares of this many 1 m cells.
TERRAIN_CELL = 5
# The terrain grid file reaches this far beyond the city box, metres.
TERRAIN_GRID_MARGIN = 2000
OSM_SOURCES = ["osm_hamburg", "osm_schleswig_holstein", "osm_niedersachsen"]
TILE_KM_RE = re.compile(r"_32_(\d+)_(\d+)_")
# Hamburg tiles use -9999 for no data despite their header.
NODATA_LIMIT = -1000.0


def marker(name):
    """Path of the marker file that says a step finished."""
    return os.path.join(data_root.geodata_dir(), ".done", name)


def step(name, force):
    """True when the step still has to run."""
    return force or not os.path.exists(marker(name))


def finish(name):
    """Marks a step as finished."""
    os.makedirs(os.path.dirname(marker(name)), exist_ok=True)
    open(marker(name), "w").close()
    print(f"{name}: done", flush=True)


def in_city(file_name):
    """True when a 1 km tile named like `..._32_<E km>_<N km>_...` lies in the city bbox."""
    match = TILE_KM_RE.search(os.path.basename(file_name))
    if not match:
        return False
    east, north = int(match[1]), int(match[2])
    return CITY_KM_EAST[0] <= east < CITY_KM_EAST[1] and CITY_KM_NORTH[0] <= north < CITY_KM_NORTH[1]


def download_files(source_id, pattern="*"):
    """Downloaded files of one source matching a glob pattern."""
    return sorted(glob.glob(os.path.join(data_root.downloads_dir(source_id), pattern)))


def link(source, destination):
    """Symlinks a downloaded file into the geodata tree, replacing an older link."""
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    if os.path.islink(destination) or os.path.exists(destination):
        os.remove(destination)
    os.symlink(source, destination)


def prepare_osm(geodata):
    """The Bergedorf and the city OSM extracts."""
    clip_osm_extract(geodata, "bergedorf", BBOX_WGS84)
    clip_osm_extract(geodata, "hamburg", CITY_WGS84)


def clip_osm_extract(geodata, name, bbox):
    """Clips each OSM download to the bbox and merges them into osm/<name>.osm.pbf."""
    work = os.path.join(geodata, "osm", "clipped")
    os.makedirs(work, exist_ok=True)
    clipped = []
    for source_id in OSM_SOURCES:
        source = download_files(source_id, "*.osm.pbf")[0]
        output = os.path.join(work, source_id + ".osm.pbf")
        subprocess.run([sys.executable, "-I", os.path.join(TOOLS_DIR, "geodata", "clip_osm.py"), source, output,
                        *map(str, bbox)], check=True)
        clipped.append(output)
    merged = os.path.join(geodata, "osm", name + ".osm.pbf")
    subprocess.run([sys.executable, "-I", os.path.join(TOOLS_DIR, "geodata", "merge_osm.py"), merged, *clipped],
                   check=True)
    shutil.rmtree(work)


def prepare_terrain(geodata):
    """Averages the DGM1 tiles of the city bbox to 5 m: Hamburg's from its zip, Lower Saxony's 2025 survey."""
    destination = os.path.join(geodata, "raw", "dgm5")
    os.makedirs(destination, exist_ok=True)
    archive = download_files("dgm1_hamburg", "*.zip")[0]
    with zipfile.ZipFile(archive) as zip_file:
        hamburg = [f"/vsizip/{archive}/{entry}" for entry in zip_file.namelist()
                   if entry.endswith(".tif") and in_city(entry)]
    lower_saxony = [path for path in download_files("dgm1_niedersachsen", "*_2025.tif") if in_city(path)]
    for paths, state in ((hamburg, "hh"), (lower_saxony, "ni")):
        for path in paths:
            write_coarse_tile(path, destination, state)
        print(f"  {len(paths)} {state} tiles", flush=True)


def write_coarse_tile(source_path, destination, state):
    """One 1 km DGM1 tile averaged over TERRAIN_CELL squares, written as dgm5_32_<E>_<N>_<state>.tif."""
    east, north = TILE_KM_RE.search(os.path.basename(source_path)).groups()
    target = os.path.join(destination, f"dgm5_32_{east}_{north}_{state}.tif")
    if os.path.exists(target):
        return
    with rasterio.open(source_path) as source:
        heights = source.read(1).astype(np.float32)
        left, top = source.bounds.left, source.bounds.top
    heights[heights < NODATA_LIMIT] = np.nan
    rows, columns = heights.shape[0] // TERRAIN_CELL, heights.shape[1] // TERRAIN_CELL
    blocks = heights[:rows * TERRAIN_CELL, :columns * TERRAIN_CELL].reshape(rows, TERRAIN_CELL, columns, TERRAIN_CELL)
    # Squares with no valid cell (outside the survey) stay NaN, so the GLO-30 fallback fills them.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        coarse = np.nanmean(blocks, axis=(1, 3)).astype(np.float32)
    profile = {"driver": "GTiff", "width": columns, "height": rows, "count": 1, "dtype": "float32",
               "crs": "EPSG:25832", "transform": from_origin(left, top, TERRAIN_CELL, TERRAIN_CELL),
               "nodata": float("nan"), "compress": "deflate", "predictor": 3}
    with rasterio.open(target + ".part", "w", **profile) as output:
        output.write(coarse, 1)
    os.replace(target + ".part", target)


def prepare_glo30(geodata):
    """Links the Copernicus GLO-30 tiles."""
    for source_id in ("glo30", "glo30_e009"):
        for path in download_files(source_id, "*.tif"):
            link(path, os.path.join(geodata, "raw", "copernicus_glo30", os.path.basename(path)))


def prepare_terrain_grid(geodata):
    """The 5 m terrain of the city box as one grid file, with the GLO-30 tiles linked first for its fallback."""
    sys.path.insert(0, os.path.join(TOOLS_DIR, "osmimport"))
    from osmimport import dem
    margin = TERRAIN_GRID_MARGIN
    dem.write_terrain_grid(geodata, CITY_KM_EAST[0] * 1000 - margin, CITY_KM_NORTH[0] * 1000 - margin,
                           CITY_KM_EAST[1] * 1000 + margin, CITY_KM_NORTH[1] * 1000 + margin)


def prepare_street_trees(geodata):
    """Links the street tree register of the city bbox and writes the table the world builder reads."""
    for path in download_files("street_trees_hamburg", "*.geojson"):
        link(path, os.path.join(geodata, "raw", "strassenbaeume", os.path.basename(path)))
    write_street_tree_table(os.path.join(geodata, "raw", "strassenbaeume", "strassenbaeume_bbox.geojson"),
                            os.path.join(geodata, "raw", "strassenbaeume", "street_trees.tsv"))


def write_street_tree_table(geojson_path, table_path):
    """One line per tree: tree id, UTM east and north, German genus, crown diameter and trunk girth (0 when
    unknown), tab-separated, in the register's order."""
    import json
    with open(geojson_path) as source:
        features = json.load(source)["features"]
    with open(table_path + ".part", "w") as output:
        for feature in features:
            east, north = feature["geometry"]["coordinates"][0][:2]
            properties = feature["properties"]
            genus = (properties.get("gattung_deutsch") or "").replace("\t", " ")
            output.write(f"{properties.get('baumid') or 0}\t{east:.3f}\t{north:.3f}\t{genus}\t"
                         f"{properties.get('kronendurchmesser') or 0}\t{properties.get('stammumfang') or 0}\n")
    os.replace(table_path + ".part", table_path)


STEPS = [
    ("osm_city", prepare_osm),
    ("dgm5", prepare_terrain),
    ("glo30_city", prepare_glo30),
    ("terrain_grid", prepare_terrain_grid),
    ("street_tree_table", prepare_street_trees),
]


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="redo steps that already finished")
    args = parser.parse_args()
    geodata = data_root.geodata_dir()
    for name, function in STEPS:
        if not step(name, args.force):
            print(f"{name}: already prepared")
            continue
        print(f"{name}: preparing", flush=True)
        function(geodata)
        finish(name)


if __name__ == "__main__":
    main()
