"""Where the project's large data lives: downloads, geodata, world data, Python environments and Unreal content that
isn't in git.

The data root is $THIRD_GEAR_DATA if set, otherwise the `External` link in the checkout, which points to
/mnt/storage/third-gear on the development machine. Keeping the data outside the git tree means no git
operation can delete it; at worst it removes a link.

  python3 -I Tools/bootstrap/data_root.py [target]   create the links of this checkout (default target below)

Besides `External`, a checkout gets ignored links where Unreal and the scripts expect things: the content folders
under Content/, the derived data cache and the Python environments.
"""
import os
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_TARGET = "/mnt/storage/third-gear"

# Checkout path -> path inside the data root. All of these are listed in .gitignore without a trailing slash.
CHECKOUT_LINKS = {
    "Content/Audio": "unreal/Audio",
    "Content/CitySampleVehicles": "unreal/CitySampleVehicles",
    "Content/Textures": "unreal/Textures",
    "Content/Vegetation": "unreal/Vegetation",
    "Content/Grass": "unreal/Grass",
    "Content/World": "unreal/World",
    "DerivedDataCache": "ddc",
    "Tools/osmimport/.venv": "venvs/osmimport",
}


def data_root():
    """Absolute path of the data root; raises if it doesn't exist yet (see `ensure_links`)."""
    override = os.environ.get("THIRD_GEAR_DATA")
    if override:
        return os.path.abspath(override)
    link = os.path.join(REPO_ROOT, "External")
    if not os.path.isdir(link):
        raise SystemExit(f"No data root: create it with `python3 -I Tools/bootstrap/data_root.py` "
                         f"(links {link} to {DEFAULT_TARGET}) or set THIRD_GEAR_DATA.")
    return os.path.realpath(link)


def downloads_dir(source_id=None):
    """Folder of the raw downloads, or of one source's files."""
    path = os.path.join(data_root(), "downloads")
    if source_id:
        path = os.path.join(path, source_id)
    return path


def geodata_dir():
    """Prepared geodata in the layout the OSM pipeline reads (osm/, raw/)."""
    return os.path.join(data_root(), "geodata")


def world_dir(region=None):
    """Compiled world data tiles that the game streams, or one region's folder."""
    path = os.path.join(data_root(), "world")
    if region:
        path = os.path.join(path, region)
    return path


def raw_assets_dir():
    """Downloaded CC0 textures, models, HDRIs and sign SVGs (see Data/raw_assets.md)."""
    return os.path.join(data_root(), "raw_assets")


def venv_python(name):
    """Python interpreter of one of the data root's virtual environments."""
    return os.path.join(data_root(), "venvs", name, "bin", "python")


def make_link(link, target):
    """Creates link -> target unless something already exists at link; reports what is there."""
    if os.path.islink(link) or os.path.exists(link):
        if os.path.realpath(link) != os.path.realpath(target):
            print(f"kept {link} (points to {os.path.realpath(link)}, not {target})")
        return
    os.makedirs(os.path.dirname(link), exist_ok=True)
    os.symlink(target, link)
    print(f"created {link} -> {target}")


def ensure_links(target=DEFAULT_TARGET):
    """Creates the data root folder and all links of this checkout into it, if missing."""
    os.makedirs(target, exist_ok=True)
    make_link(os.path.join(REPO_ROOT, "External"), target)
    for checkout_path, data_path in CHECKOUT_LINKS.items():
        os.makedirs(os.path.join(target, data_path), exist_ok=True)
        make_link(os.path.join(REPO_ROOT, checkout_path), os.path.join(target, data_path))


if __name__ == "__main__":
    ensure_links(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TARGET)
