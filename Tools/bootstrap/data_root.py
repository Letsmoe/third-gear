"""Where the project's large data lives: downloads, geodata, generated map data and Unreal content that isn't in git.

The data root is $THIRD_GEAR_DATA if set, otherwise the `External` link in the checkout, which points to
/mnt/storage/third-gear on the development machine. Keeping the data outside the git tree means no git
operation can delete it; at worst it removes the link.
"""
import os

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_TARGET = "/mnt/storage/third-gear"


def data_root():
    """Absolute path of the data root; raises if it doesn't exist yet (see `ensure_link`)."""
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
    """Prepared geodata in the layout the OSM pipeline reads (osm/, raw/, build/)."""
    return os.path.join(data_root(), "geodata")


def ensure_link(target=DEFAULT_TARGET):
    """Creates the target folder and the checkout's `External` link to it, if missing."""
    os.makedirs(target, exist_ok=True)
    link = os.path.join(REPO_ROOT, "External")
    if os.path.islink(link) or os.path.exists(link):
        print(f"{link} -> {os.path.realpath(link)}")
        return
    os.symlink(target, link)
    print(f"created {link} -> {target}")


if __name__ == "__main__":
    ensure_link()
