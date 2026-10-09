"""Downloads Hamburg's surveyed street data for the OSM extract's area into <data root>/geodata/hh_street_survey/.

  python3 -I Tools/geodata/fetch_hh_street_survey.py [--force]

The street model is built from OSM and assumptions; this data only measures how close it gets (see issue #91).
Sources, all from the city's OGC API (api.hamburg.de), licence dl-de/by-2.0, © Freie und Hansestadt Hamburg:

* Feinkartierung Straße: areas of the street space (carriageway, cycle track, footway, parking, islands, tree pits)
  with their surface, kerb, gutter and railing lines, and points such as lamps and signal poles.
* Verkehrszeichen Hamburg: traffic signs surveyed from street imagery, with sign number, facing and pole.
* Lichtsignalanlagen: the signalised junctions.
"""
import argparse
import json
import os
import sys
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402

API = "https://api.hamburg.de/datasets/v1"
# (dataset, collection, output name)
COLLECTIONS = [
    ("feinkartierung_strasse", "strassenflaechen", "street_areas"),
    ("feinkartierung_strasse", "linien", "street_lines"),
    ("feinkartierung_strasse", "punkte", "street_points"),
    ("verkehrszeichen", "verkehrszeichen", "traffic_signs"),
    ("lichtsignalanlagen", "lsa_knotengrunddaten", "signal_junctions"),
]
# The OSM extract's area (Tools/geodata/analyse_osm.py), longitude and latitude.
BBOX = (10.05, 53.40, 10.33, 53.53)
PAGE_SIZE = 5000


def fetch_json(url: str) -> dict:
    """One GeoJSON page."""
    request = urllib.request.Request(url, headers={"Accept": "application/geo+json"})
    with urllib.request.urlopen(request, timeout=300) as response:
        return json.load(response)


def next_link(page: dict):
    """URL of the following page, or None on the last one."""
    for link in page.get("links", []):
        if link.get("rel") == "next":
            return link["href"]
    return None


def fetch_collection(dataset: str, collection: str) -> list:
    """Every feature of a collection inside BBOX, following the paging links."""
    bbox = ",".join(str(value) for value in BBOX)
    url = f"{API}/{dataset}/collections/{collection}/items?f=json&bbox={bbox}&limit={PAGE_SIZE}"
    features = []
    while url:
        page = fetch_json(url)
        features += page.get("features", [])
        print(f"  {collection}: {len(features)} features", flush=True)
        url = next_link(page) if page.get("features") else None
    return features


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--force", action="store_true", help="download again even when the file exists")
    args = parser.parse_args()
    out_dir = os.path.join(data_root.geodata_dir(), "hh_street_survey")
    os.makedirs(out_dir, exist_ok=True)
    for dataset, collection, name in COLLECTIONS:
        path = os.path.join(out_dir, f"{name}.geojson")
        if os.path.exists(path) and not args.force:
            print(f"{name}: already there")
            continue
        print(f"{name}: {dataset}/{collection}")
        features = fetch_collection(dataset, collection)
        with open(path + ".part", "w") as out:
            json.dump({"type": "FeatureCollection", "features": features}, out)
        os.replace(path + ".part", path)


if __name__ == "__main__":
    main()
