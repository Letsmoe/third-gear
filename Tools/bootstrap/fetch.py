"""Downloads the data sources listed in Data/sources.toml into <data root>/downloads/<id>/.

  python3 -I Tools/bootstrap/fetch.py            fetch every missing required source
  python3 -I Tools/bootstrap/fetch.py --status   show what is there, without downloading
  python3 -I Tools/bootstrap/fetch.py --all      include optional sources
  python3 -I Tools/bootstrap/fetch.py <id> ...   fetch only these sources

A finished source has a `.complete` file with its URL, size and sha256; anything without it is fetched (again).
Single-file downloads resume where they stopped. Uses curl, so it works with the system Python.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tomllib
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import data_root  # noqa: E402

MANIFEST = os.path.join(data_root.REPO_ROOT, "Data", "sources.toml")
USER_AGENT = "third-gear-bootstrap (https://github.com/Letsmoe/third-gear)"
WFS_PAGE_SIZE = 5000


def load_manifest():
    """Sources from Data/sources.toml, keyed by id."""
    with open(MANIFEST, "rb") as handle:
        return tomllib.load(handle)


def completion_file(source_id):
    """Marker written after a source was fetched completely."""
    return os.path.join(data_root.downloads_dir(source_id), ".complete")


def is_complete(source_id):
    """True when the source was fetched completely before."""
    return os.path.exists(completion_file(source_id))


def sha256_of(path):
    """Hex sha256 of a file, read in chunks."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def curl_download(url, destination):
    """Downloads one URL to a file, resuming a partial download and retrying on network errors."""
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    partial = destination + ".part"
    command = ["curl", "--fail", "--location", "--retry", "5", "--retry-all-errors", "--continue-at", "-",
               "--user-agent", USER_AGENT, "--output", partial, url]
    if sys.stdout.isatty():
        command.insert(1, "--progress-bar")
    else:
        command.insert(1, "--silent")
        command.insert(2, "--show-error")
    subprocess.run(command, check=True)
    os.replace(partial, destination)


def http_json(url):
    """GET a URL and parse the JSON answer."""
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.load(response)


def fetch_url(source_id, source):
    """kind = "url": one file, checked against `size` and `sha256` when the manifest has them."""
    file_name = source.get("file") or os.path.basename(urllib.parse.urlparse(source["url"]).path)
    destination = os.path.join(data_root.downloads_dir(source_id), file_name)
    curl_download(source["url"], destination)
    size = os.path.getsize(destination)
    if "size" in source and size != source["size"]:
        raise RuntimeError(f"{source_id}: size {size}, expected {source['size']}")
    checksum = sha256_of(destination)
    if "sha256" in source and checksum != source["sha256"]:
        raise RuntimeError(f"{source_id}: sha256 {checksum}, expected {source['sha256']}")
    return {"files": [{"file": file_name, "size": size, "sha256": checksum}]}


def stac_items(search_url, bbox):
    """All items of a STAC search inside bbox, following the `next` links."""
    url = search_url + "?" + urllib.parse.urlencode({"bbox": ",".join(str(value) for value in bbox), "limit": 200})
    items = []
    while url:
        page = http_json(url)
        items += page.get("features", [])
        url = None
        for link in page.get("links", []):
            if link.get("rel") == "next":
                url = link["href"]
    return items


def fetch_stac(source_id, source):
    """kind = "stac": every asset of the matching items, one file each."""
    folder = data_root.downloads_dir(source_id)
    files = []
    items = [item for item in stac_items(source["url"], source["bbox"]) if source["item_filter"] in item["id"]]
    print(f"  {len(items)} items")
    for item in items:
        for asset in item.get("assets", {}).values():
            href = asset["href"]
            file_name = os.path.basename(urllib.parse.urlparse(href).path)
            destination = os.path.join(folder, file_name)
            if not os.path.exists(destination):
                curl_download(href, destination)
            files.append({"file": file_name, "size": os.path.getsize(destination)})
    return {"files": files}


def fetch_wfs(source_id, source):
    """kind = "wfs": all features in bbox as one GeoJSON FeatureCollection, fetched in pages."""
    features = []
    start_index = 0
    bbox = ",".join(str(value) for value in source["bbox"]) + ",urn:ogc:def:crs:" + source["crs"].replace(":", "::")
    while True:
        query = {"SERVICE": "WFS", "VERSION": "2.0.0", "REQUEST": "GetFeature", "TYPENAMES": source["typename"],
                 "OUTPUTFORMAT": "application/geo+json", "BBOX": bbox, "COUNT": WFS_PAGE_SIZE,
                 "STARTINDEX": start_index}
        page = http_json(source["url"] + "?" + urllib.parse.urlencode(query))
        page_features = page.get("features", [])
        features += page_features
        print(f"  {len(features)} features")
        if len(page_features) < WFS_PAGE_SIZE:
            break
        start_index += WFS_PAGE_SIZE
    destination = os.path.join(data_root.downloads_dir(source_id), source["file"])
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "w") as handle:
        json.dump({"type": "FeatureCollection", "features": features}, handle)
    return {"files": [{"file": source["file"], "size": os.path.getsize(destination), "features": len(features)}]}


FETCHERS = {"url": fetch_url, "stac": fetch_stac, "wfs": fetch_wfs}


def fetch(source_id, source):
    """Fetches one source and writes its completion marker."""
    print(f"{source_id}: fetching ({source['kind']})", flush=True)
    result = FETCHERS[source["kind"]](source_id, source)
    result["url"] = source["url"]
    with open(completion_file(source_id), "w") as handle:
        json.dump(result, handle, indent=1)
    total = sum(entry["size"] for entry in result["files"])
    print(f"{source_id}: done, {len(result['files'])} files, {total / 1e6:.0f} MB", flush=True)


def print_status(manifest):
    """One line per source: fetched or missing, and its size on disk."""
    for source_id, source in manifest.items():
        state = "missing"
        if is_complete(source_id):
            with open(completion_file(source_id)) as handle:
                total = sum(entry["size"] for entry in json.load(handle)["files"])
            state = f"ok ({total / 1e6:.0f} MB)"
        optional = " (optional)" if source.get("optional") else ""
        print(f"{source_id:24} {state}{optional}")


def main():
    """Command line entry point."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ids", nargs="*", help="sources to fetch (default: all required ones)")
    parser.add_argument("--all", action="store_true", help="include optional sources")
    parser.add_argument("--status", action="store_true", help="only show what is there")
    args = parser.parse_args()

    manifest = load_manifest()
    if args.status:
        print_status(manifest)
        return
    unknown = [source_id for source_id in args.ids if source_id not in manifest]
    if unknown:
        raise SystemExit(f"unknown sources: {', '.join(unknown)}")
    if args.ids:
        selected = args.ids
    else:
        selected = [source_id for source_id, source in manifest.items() if args.all or not source.get("optional")]

    failed = []
    for source_id in selected:
        if is_complete(source_id) and not args.ids:
            continue
        try:
            fetch(source_id, manifest[source_id])
        except (RuntimeError, subprocess.CalledProcessError, OSError) as error:
            print(f"{source_id}: FAILED: {error}", flush=True)
            failed.append(source_id)
    if failed:
        raise SystemExit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
