"""List/download Niedersachsen DGM1 COG tiles (LGLN STAC) for a bbox. usage: fetch_ni_dgm1.py OUTDIR [--list]"""
import sys, json, urllib.request, urllib.parse, os
out = sys.argv[1]; listonly = "--list" in sys.argv
url = "https://dgm.stac.lgln.niedersachsen.de/search?" + urllib.parse.urlencode({"bbox": "10.05,53.40,10.33,53.53", "limit": 200})
items = []
while url:
    d = json.load(urllib.request.urlopen(url, timeout=60))
    items += d["features"]
    url = next((l["href"] for l in d["links"] if l["rel"] == "next"), None)
print("tiles", len(items))
for it in items:
    href = it["assets"]["dgm1-tif"]["href"]
    name = os.path.basename(href)
    if not name.startswith("dgm1_32_") or not name.endswith(".tif") or "/" in name: raise SystemExit("odd name " + name)
    if listonly: print(name); continue
    dst = os.path.join(out, name)
    if not os.path.exists(dst): urllib.request.urlretrieve(href, dst)
print("done")
