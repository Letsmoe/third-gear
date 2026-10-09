"""Download an Unreal Engine asset pack from the user's Fab library (Linux has no Epic launcher).

Uses the Epic login that Heroic/legendary stores (~/.config/heroic/legendaryConfig/legendary/user.json) and
legendary's manifest/chunk parsers (pip install legendary-gl into a venv):

    <data root>/venvs/fab/bin/python -I Tools/asset_fetch/fab_download.py <title substring> <out dir> [--list]

Library: GET  https://www.fab.com/e/accounts/{account}/ue/library
Manifest: POST https://www.fab.com/e/artifacts/{artifactId}/manifest {item_id, namespace, platform}
-> Epic chunked build manifest; files are reassembled from chunks on the CDN.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

from legendary.models.chunk import Chunk
from legendary.models.json_manifest import JSONManifest
from legendary.models.manifest import Manifest

USER_JSON = os.path.expanduser("~/.config/heroic/legendaryConfig/legendary/user.json")
UA = "UELauncher/17.0.1-37584233+++Portal+Release-Live Windows/10.0.19045.1.256.64bit"


def request(url, token=None, body=None, timeout=120):
    headers = {"User-Agent": UA}
    if token:
        headers["Authorization"] = "Bearer " + token
    if body is not None:
        headers["Content-Type"] = "application/json"
        body = json.dumps(body).encode()
    req = urllib.request.Request(url, data=body, method="POST" if body is not None else "GET", headers=headers)
    for attempt in range(5):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except (urllib.error.URLError, TimeoutError) as e:
            if isinstance(e, urllib.error.HTTPError) and e.code < 500:
                raise
            time.sleep(2 ** attempt)
    raise RuntimeError("giving up on " + url)


def library(token, account):
    items, cursor = [], None
    while True:
        url = f"https://www.fab.com/e/accounts/{account}/ue/library?count=100" + (f"&cursor={cursor}" if cursor else "")
        d = json.loads(request(url, token))
        items += d.get("results", [])
        cursor = (d.get("cursors") or {}).get("next")
        if not cursor:
            return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("title")
    ap.add_argument("out")
    ap.add_argument("--list", action="store_true", help="only print the files")
    ap.add_argument("--jobs", type=int, default=16)
    a = ap.parse_args()

    user = json.load(open(USER_JSON))
    token = user["access_token"]
    print("token expires", user.get("expires_at"))
    items = [i for i in library(token, user["account_id"]) if a.title.lower() in i.get("title", "").lower()]
    if not items:
        sys.exit("not in library: " + a.title)
    item = items[0]
    version = item["projectVersions"][-1]  # newest engine version
    print(item["title"], "|", version["artifactId"], version.get("engineVersions"))

    info = json.loads(request(f"https://www.fab.com/e/artifacts/{version['artifactId']}/manifest", token,
                              {"item_id": item["assetId"], "namespace": item["assetNamespace"],
                               "platform": "Windows"}))
    dl = info["downloadInfo"][0]
    print("build", dl["buildVersion"])
    data, base = None, None
    for dp in dl["distributionPoints"]:
        try:
            data = request(dp["manifestUrl"])
            base = dp["manifestUrl"].rsplit("/", 1)[0]
            break
        except Exception as e:
            print("manifest mirror failed:", e)
    if data[:4] == b"\x0c\xc0\xbe\x44":
        man = Manifest.read_all(data)
    else:
        man = JSONManifest.read_all(data)
    if hashlib.sha1(data).hexdigest() != dl["manifestHash"]:
        print("warning: manifest hash differs")

    files = man.file_manifest_list.elements
    total = sum(f.file_size for f in files)
    print(f"{len(files)} files, {total / 1e9:.2f} GB, {len(man.chunk_data_list.elements)} chunks")
    if a.list:
        for f in files:
            print(f"{f.file_size:>12}  {f.filename}")
        return

    chunk_dir = os.path.join(a.out, ".chunks")
    os.makedirs(chunk_dir, exist_ok=True)
    chunks = {c.guid_num: c for c in man.chunk_data_list.elements}
    bases = [base] + [u for u in dl.get("distributionPointBaseUrls", []) if u != base]

    def fetch(c):
        path = os.path.join(chunk_dir, f"{c.guid_num:032x}")
        if os.path.exists(path):
            return
        last = None
        for b in bases:
            try:
                raw = request(b + "/" + c.path)
                payload = Chunk.read_buffer(raw).data
                with open(path + ".part", "wb") as fh:
                    fh.write(payload)
                os.replace(path + ".part", path)
                return
            except Exception as e:
                last = e
        raise RuntimeError(f"chunk {c.path}: {last}")

    done = 0
    with ThreadPoolExecutor(a.jobs) as pool:
        for _ in pool.map(fetch, chunks.values()):
            done += 1
            if done % 200 == 0 or done == len(chunks):
                print(f"  chunks {done}/{len(chunks)}", flush=True)

    bad = 0
    for f in files:
        dst = os.path.join(a.out, f.filename)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        sha = hashlib.sha1()
        with open(dst, "wb") as out:
            for part in f.chunk_parts:
                with open(os.path.join(chunk_dir, f"{part.guid_num:032x}"), "rb") as cf:
                    cf.seek(part.offset)
                    buf = cf.read(part.size)
                out.write(buf)
                sha.update(buf)
        if f.hash and sha.digest() != f.hash:
            print("HASH MISMATCH", f.filename)
            bad += 1
    print("assembled", len(files), "files in", a.out, f"({bad} hash mismatches)")
    if not bad:
        shutil.rmtree(chunk_dir)


if __name__ == "__main__":
    main()
