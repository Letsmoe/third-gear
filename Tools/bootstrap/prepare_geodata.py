"""Turns the raw downloads into the geodata layout the map pipeline reads, under <data root>/geodata.

  <data root>/venvs/osmimport/bin/python -I Tools/bootstrap/prepare_geodata.py [--force]

Layout written (what Tools/osmimport expects):
  osm/bergedorf.osm.pbf                      Hamburg, Schleswig-Holstein and Lower Saxony, clipped to the project bbox
  raw/dgm1_hamburg_extracted/*.tif           Hamburg DGM1 tiles inside the bbox
  raw/dgm1_niedersachsen/*.tif               Lower Saxony DGM1 tiles (links to the downloads)
  raw/copernicus_glo30/*.tif                 GLO-30 fallback (link)
  raw/bdom_hamburg/tif/*.tif                 Hamburg bDOM 2020 inside the bbox, converted from XYZ
  raw/strassenbaeume/strassenbaeume_bbox.geojson   street tree register (link)

Every step writes a marker file and is skipped on the next run unless --force is given.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data_root  # noqa: E402

TOOLS_DIR = os.path.join(data_root.REPO_ROOT, "Tools")
# Project bbox in UTM 32N kilometres (see Data/sources.toml) and in WGS84 for the OSM clip.
BBOX_KM_EAST = (569, 588)
BBOX_KM_NORTH = (5917, 5932)
BBOX_WGS84 = (10.05, 53.40, 10.33, 53.53)
OSM_SOURCES = ["osm_hamburg", "osm_schleswig_holstein", "osm_niedersachsen"]
TILE_KM_RE = re.compile(r"_32_(\d+)_(\d+)_")
ZIP_DEFLATE64 = 9  # zipfile has no constant for it


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


def in_bbox(file_name):
    """True when a 1 km tile named like `..._32_<E km>_<N km>_...` overlaps the project bbox."""
    match = TILE_KM_RE.search(os.path.basename(file_name))
    if not match:
        return False
    east, north = int(match[1]), int(match[2])
    return BBOX_KM_EAST[0] <= east < BBOX_KM_EAST[1] and BBOX_KM_NORTH[0] <= north < BBOX_KM_NORTH[1]


def download_files(source_id, pattern="*"):
    """Downloaded files of one source matching a glob pattern."""
    return sorted(glob.glob(os.path.join(data_root.downloads_dir(source_id), pattern)))


def link(source, destination):
    """Symlinks a downloaded file into the geodata tree, replacing an older link."""
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    if os.path.islink(destination) or os.path.exists(destination):
        os.remove(destination)
    os.symlink(source, destination)


def extract_tiles(archive, destination, suffix):
    """Extracts the bbox tiles with the given suffix from a zip into one flat folder; returns their count.

    Python's zipfile can't read Deflate64, which the Hamburg bDOM archive uses; those entries go through 7z."""
    os.makedirs(destination, exist_ok=True)
    count = 0
    deflate64_entries = []
    with zipfile.ZipFile(archive) as zip_file:
        for entry in zip_file.infolist():
            name = os.path.basename(entry.filename)
            if not name.endswith(suffix) or not in_bbox(name):
                continue
            count += 1
            target = os.path.join(destination, name)
            if os.path.exists(target):
                continue
            if entry.compress_type == ZIP_DEFLATE64:
                deflate64_entries.append(entry.filename)
                continue
            with zip_file.open(entry) as source, open(target + ".part", "wb") as output:
                shutil.copyfileobj(source, output, 1 << 22)
            os.replace(target + ".part", target)
    if deflate64_entries:
        # `e` extracts without the archive's folders, so names can't escape the destination.
        subprocess.run(["7z", "e", "-y", "-bd", f"-o{destination}", archive, *deflate64_entries], check=True,
                       stdout=subprocess.DEVNULL)
    return count


def prepare_osm(geodata):
    """Clips each OSM extract to the bbox and merges them into osm/bergedorf.osm.pbf."""
    work = os.path.join(geodata, "osm", "clipped")
    os.makedirs(work, exist_ok=True)
    clipped = []
    for source_id in OSM_SOURCES:
        source = download_files(source_id, "*.osm.pbf")[0]
        output = os.path.join(work, source_id + ".osm.pbf")
        subprocess.run([sys.executable, "-I", os.path.join(TOOLS_DIR, "geodata", "clip_osm.py"), source, output,
                        *map(str, BBOX_WGS84)], check=True)
        clipped.append(output)
    merged = os.path.join(geodata, "osm", "bergedorf.osm.pbf")
    subprocess.run([sys.executable, "-I", os.path.join(TOOLS_DIR, "geodata", "merge_osm.py"), merged, *clipped],
                   check=True)
    shutil.rmtree(work)


def prepare_dgm_hamburg(geodata):
    """Extracts the Hamburg DGM1 tiles inside the bbox."""
    archive = download_files("dgm1_hamburg", "*.zip")[0]
    count = extract_tiles(archive, os.path.join(geodata, "raw", "dgm1_hamburg_extracted"), ".tif")
    print(f"  {count} Hamburg DGM1 tiles")


def prepare_dgm_niedersachsen(geodata):
    """Links the Lower Saxony DGM1 tiles."""
    destination = os.path.join(geodata, "raw", "dgm1_niedersachsen")
    for path in download_files("dgm1_niedersachsen", "*.tif"):
        link(path, os.path.join(destination, os.path.basename(path)))


def prepare_glo30(geodata):
    """Links the Copernicus GLO-30 tile."""
    for path in download_files("glo30", "*.tif"):
        link(path, os.path.join(geodata, "raw", "copernicus_glo30", os.path.basename(path)))


def prepare_bdom(geodata):
    """Extracts the bDOM XYZ tiles inside the bbox, converts them to GeoTIFF and drops the XYZ files."""
    archive = download_files("bdom_hamburg", "*.zip")[0]
    xyz_dir = os.path.join(geodata, "raw", "bdom_hamburg", "xyz")
    tif_dir = os.path.join(geodata, "raw", "bdom_hamburg", "tif")
    count = extract_tiles(archive, xyz_dir, ".xyz")
    print(f"  {count} bDOM tiles, converting", flush=True)
    subprocess.run([sys.executable, "-I", os.path.join(TOOLS_DIR, "osmimport", "convert_bdom.py"), xyz_dir, tif_dir],
                   check=True, stdout=subprocess.DEVNULL)
    shutil.rmtree(xyz_dir)


def prepare_street_trees(geodata):
    """Links the street tree register."""
    for path in download_files("street_trees_hamburg", "*.geojson"):
        link(path, os.path.join(geodata, "raw", "strassenbaeume", os.path.basename(path)))


STEPS = [
    ("osm", prepare_osm),
    ("dgm1_hamburg", prepare_dgm_hamburg),
    ("dgm1_niedersachsen", prepare_dgm_niedersachsen),
    ("glo30", prepare_glo30),
    ("bdom_hamburg", prepare_bdom),
    ("street_trees", prepare_street_trees),
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
