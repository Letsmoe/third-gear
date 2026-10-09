"""Sets up everything that isn't in git, in one go: data root links, downloads, Python environments, geodata,
the patched OpenXR plugin, the editor build, the car, textures, materials, tree models and the world data.

  python3 -I Tools/bootstrap/bootstrap.py            run every step that isn't done yet
  python3 -I Tools/bootstrap/bootstrap.py --status   show which steps are done, without running anything
  python3 -I Tools/bootstrap/bootstrap.py <step> ... run only these steps (even if they look done)

Each step checks its own output, so running it again is cheap. The Unreal steps need the editor closed.
Environment: UE (engine root, default /mnt/storage/UnrealEngine/5.8.1), THIRD_GEAR_DATA (data root override).
The car comes from the Fab library and needs a current Epic login in Heroic.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data_root  # noqa: E402

REPO = data_root.REPO_ROOT
UE = os.environ.get("UE", "/mnt/storage/UnrealEngine/5.8.1")
PROJECT = os.path.join(REPO, "DrivingGame.uproject")
EDITOR_CMD = os.path.join(UE, "Engine", "Binaries", "Linux", "UnrealEditor-Cmd")
EDITOR_LIBRARY = os.path.join(REPO, "Binaries", "Linux", "libUnrealEditor-DrivingGame.so")
PYTHON_ENVS = {"osmimport": "Tools/osmimport/requirements.txt", "fab": None}
FAB_PACKAGES = ["legendary-gl"]
WORLD_REGIONS = ["bergedorf_test"]


def run(command, **kwargs):
    """Runs a command from the repo root, echoing it, and fails loudly."""
    print("  $ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, cwd=REPO, **kwargs)


def has_files(folder, pattern="**/*"):
    """True when the folder contains at least one file matching the pattern."""
    return any(os.path.isfile(path) for path in glob.iglob(os.path.join(folder, pattern), recursive=True))


def run_editor_script(script, *arguments):
    """Runs one of Scripts/*.py in a headless editor."""
    script_argument = " ".join([os.path.join(REPO, "Scripts", script), *arguments])
    run([EDITOR_CMD, PROJECT, "-run=pythonscript", f"-script={script_argument}", "-unattended", "-nosplash"])


# --- steps: each is (is_done, run) ---------------------------------------------------------------------------

def links_done():
    """The checkout's links into the data root exist."""
    return all(os.path.lexists(os.path.join(REPO, path)) for path in ["External", *data_root.CHECKOUT_LINKS])


def links_run():
    """Creates the links."""
    data_root.ensure_links()


def downloads_done():
    """Every required source has been fetched completely."""
    result = subprocess.run([sys.executable, "-I", os.path.join(REPO, "Tools", "bootstrap", "fetch.py"), "--status"],
                            capture_output=True, text=True, check=True)
    return all("ok" in line or "optional" in line for line in result.stdout.splitlines())


def downloads_run():
    """Fetches the missing sources."""
    run([sys.executable, "-I", "Tools/bootstrap/fetch.py"])


def python_done():
    """Both Python environments exist."""
    return all(os.path.exists(data_root.venv_python(name)) for name in PYTHON_ENVS)


def python_run():
    """Creates the Python environments in the data root, with uv when it is installed."""
    for name, requirements in PYTHON_ENVS.items():
        if os.path.exists(data_root.venv_python(name)):
            continue
        folder = os.path.join(data_root.data_root(), "venvs", name)
        packages = ["-r", os.path.join(REPO, requirements)] if requirements else FAB_PACKAGES
        if shutil.which("uv"):
            run(["uv", "venv", "-q", "--python", "3.13", folder])
            run(["uv", "pip", "install", "-q", "--python", data_root.venv_python(name), *packages])
        else:
            run([sys.executable, "-m", "venv", folder])
            run([data_root.venv_python(name), "-m", "pip", "install", "-q", *packages])


def geodata_done():
    """Every geodata preparation step has finished."""
    steps = ["osm_city", "dgm5", "glo30_city", "terrain_grid", "street_tree_table"]
    return all(os.path.exists(os.path.join(data_root.geodata_dir(), ".done", step)) for step in steps)


def geodata_run():
    """Unpacks and converts the downloads into the layout the map pipeline reads."""
    run([data_root.venv_python("osmimport"), "-I", "Tools/bootstrap/prepare_geodata.py"])


def openxr_done():
    """The patched OpenXR plugin exists."""
    return os.path.exists(os.path.join(REPO, "Plugins", "OpenXR", "OpenXR.uplugin"))


def openxr_run():
    """Copies and patches the engine's OpenXR plugin."""
    run(["bash", "Scripts/setup_openxr_plugin.sh"])


def build_done():
    """The editor module has been built at least once (rebuild after C++ changes yourself)."""
    return os.path.exists(EDITOR_LIBRARY)


def build_run():
    """Builds the editor target."""
    run([os.path.join(UE, "Engine", "Build", "BatchFiles", "Linux", "Build.sh"), "DrivingGameEditor", "Linux",
         "Development", f"-project={PROJECT}", "-waitmutex"])


def car_done():
    """The City Sample Vehicles pack is installed and the traffic paint made from it."""
    return (has_files(os.path.join(REPO, "Content", "CitySampleVehicles"), "**/*.uasset")
            and has_files(os.path.join(REPO, "Content", "Vehicles", "TrafficPaint"), "*.uasset"))


def car_run():
    """Downloads City Sample Vehicles from the Fab library unless installed, then makes the traffic paint from it."""
    if not has_files(os.path.join(REPO, "Content", "CitySampleVehicles"), "**/*.uasset"):
        download_city_sample_vehicles()
    run_editor_script("create_traffic_paint.py")


def download_city_sample_vehicles():
    """Downloads City Sample Vehicles from the Fab library and moves it into the data root's content."""
    download = data_root.downloads_dir("fab_city_sample_vehicles")
    run([data_root.venv_python("fab"), "-I", "Tools/asset_fetch/fab_download.py", "City Sample Vehicles", download])
    target = os.path.realpath(os.path.join(REPO, "Content", "CitySampleVehicles"))
    os.rmdir(target)  # empty folder created by the links step
    shutil.move(os.path.join(download, "Content", "CitySampleVehicles"), target)
    shutil.rmtree(download)


def textures_done():
    """The photo-scanned texture sets have been imported."""
    return has_files(os.path.join(REPO, "Content", "Textures"), "**/*.uasset")


def textures_run():
    """Imports the texture sets from the data root's raw assets."""
    run_editor_script("import_textures.py")


def materials_done():
    """The surface and terrain materials exist."""
    return os.path.exists(os.path.join(REPO, "Content", "World", "Materials", "M_TerrainMaster.uasset"))


def materials_run():
    """Creates the master materials and their instances."""
    run_editor_script("create_materials.py")


def facades_done():
    """The Megascans texture sets are imported and the facade materials exist."""
    return os.path.exists(os.path.join(REPO, "Content", "World", "Facades", "M_Facade_Brick.uasset"))


def facades_run():
    """Imports the Megascans from the data root's raw assets (Fab content, downloaded by hand), packs the grime atlas and builds the facade materials."""
    run([data_root.venv_python("osmimport"), "-I", "Tools/buildingkit/build_weathering_atlas.py"])
    run_editor_script("import_megascans.py")
    run_editor_script("create_facade_materials.py")


def trees_done():
    """The tree models have been baked."""
    return has_files(os.path.join(REPO, "Content", "Vegetation"), "**/*.uasset")


def trees_run():
    """Bakes the engine's sample trees into static Nanite meshes and gives them their wind materials."""
    run_editor_script("bake_vegetation.py")
    run_editor_script("create_tree_wind_materials.py")


def grass_done():
    """The grass tufts and their materials exist."""
    return os.path.exists(os.path.join(REPO, "Content", "Grass", "Materials", "M_GrassTuft.uasset"))


def grass_run():
    """Imports the Poly Haven grass tufts and creates the grass materials."""
    run_editor_script("import_grass_models.py")
    run_editor_script("create_grass_materials.py")


def leaves_done():
    """The leaf card meshes and material exist."""
    return os.path.exists(os.path.join(REPO, "Content", "Leaves", "M_LeafCard.uasset"))


def leaves_run():
    """Builds the leaf atlas from ambientCG leaf sets, then the leaf card meshes and material."""
    run([data_root.venv_python("osmimport"), "-I", "Tools/leafatlas/build_leaf_atlas.py"])
    run_editor_script("create_leaf_assets.py")


def world_done():
    """The world data and horizon of every default region have been compiled."""
    return all(os.path.exists(os.path.join(data_root.world_dir(region), "world.json"))
               and os.path.exists(os.path.join(data_root.world_dir(region), "horizon", "horizon.json"))
               and os.path.exists(os.path.join(data_root.world_dir(region), "lanes.json"))
               for region in WORLD_REGIONS)


def world_run():
    """Compiles the world data tiles the game streams, the low-detail horizon around them and the AI traffic lanes."""
    for region in WORLD_REGIONS:
        run([data_root.venv_python("osmimport"), "-I", "Tools/osmimport/build_world.py", region])
        run([data_root.venv_python("osmimport"), "-I", "Tools/osmimport/build_horizon_world.py", region])
        run([data_root.venv_python("osmimport"), "-I", "Tools/osmimport/build_lanes.py", region])


STEPS = {
    "links": (links_done, links_run),
    "downloads": (downloads_done, downloads_run),
    "python": (python_done, python_run),
    "geodata": (geodata_done, geodata_run),
    "openxr": (openxr_done, openxr_run),
    "build": (build_done, build_run),
    "car": (car_done, car_run),
    "textures": (textures_done, textures_run),
    "materials": (materials_done, materials_run),
    "facades": (facades_done, facades_run),
    "trees": (trees_done, trees_run),
    "grass": (grass_done, grass_run),
    "leaves": (leaves_done, leaves_run),
    "world": (world_done, world_run),
}


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("steps", nargs="*", help=f"steps to run (default: all missing): {', '.join(STEPS)}")
    parser.add_argument("--status", action="store_true", help="only show which steps are done")
    args = parser.parse_args()
    unknown = [name for name in args.steps if name not in STEPS]
    if unknown:
        raise SystemExit(f"unknown steps: {', '.join(unknown)}")

    if args.status:
        for name, (is_done, _) in STEPS.items():
            print(f"{name:10} {'done' if is_done() else 'missing'}")
        return
    for name, (is_done, run_step) in STEPS.items():
        if args.steps and name not in args.steps:
            continue
        if not args.steps and is_done():
            print(f"{name}: done")
            continue
        print(f"{name}: running", flush=True)
        run_step()


if __name__ == "__main__":
    main()
