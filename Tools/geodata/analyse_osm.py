"""Analyse clipped OSM extract. usage: analyse_osm.py IN.osm.pbf OUT.json"""
import sys, json, math
from collections import Counter, defaultdict
import osmium

W, S, E, N = 10.05, 53.40, 10.33, 53.53
LAT0 = 53.465
KX = 111320 * math.cos(math.radians(LAT0)); KY = 110574.0
def inb(lon, lat): return W <= lon <= E and S <= lat <= N

DRIVABLE = {"motorway","trunk","primary","secondary","tertiary","unclassified","residential","service","living_street",
            "motorway_link","trunk_link","primary_link","secondary_link","tertiary_link","road"}
TOPN = 25

def way_len_in(coords):
    tot = 0.0; ins = 0.0; any_in = False
    for (x1,y1),(x2,y2) in zip(coords, coords[1:]):
        d = math.hypot((x2-x1)*KX, (y2-y1)*KY); tot += d
        if inb((x1+x2)/2, (y1+y2)/2): ins += d
    any_in = any(inb(x,y) for x,y in coords)
    return tot, ins, any_in

def ring_area(coords):
    a = 0.0
    for (x1,y1),(x2,y2) in zip(coords, coords[1:]):
        a += (x1*KX)*(y2*KY) - (x2*KX)*(y1*KY)
    return abs(a)/2

def tagdict(o): return {t.k: t.v for t in o.tags}

path, outp = sys.argv[1], sys.argv[2]
R = {}

# ---------- ways (linear) ----------
hw_cnt = Counter(); hw_km = Counter(); hw_km_in = Counter()
drv = []   # (tags, km_in)
other_lin = defaultdict(Counter); other_lin_km = defaultdict(Counter)
bridges = Counter(); tunnels = Counter(); roundabouts = 0; roundabout_ids=[]
rail_cnt = Counter(); rail_km = Counter()
tree_rows = 0; barriers = Counter()
fp = osmium.FileProcessor(path, osmium.osm.NODE | osmium.osm.WAY).with_locations()
for w in fp:
    if not w.is_way(): continue
    try: coords = [(n.lon, n.lat) for n in w.nodes]
    except Exception: continue
    t = tagdict(w)
    if len(coords) < 2: continue
    tot, ins, anyin = way_len_in(coords)
    if not anyin: continue
    hw = t.get("highway")
    if hw:
        hw_cnt[hw] += 1; hw_km[hw] += tot/1000; hw_km_in[hw] += ins/1000
        if t.get("junction") in ("roundabout","circular"): roundabouts += 1
        if "bridge" in t and t["bridge"] != "no": bridges[hw] += 1
        if "tunnel" in t and t["tunnel"] != "no": tunnels[hw] += 1
        if hw in DRIVABLE or hw == "track": drv.append((hw, t, ins/1000))
    if "railway" in t:
        rail_cnt[t["railway"]] += 1; rail_km[t["railway"]] += ins/1000
        if "bridge" in t and t["bridge"] != "no": bridges["railway"] += 1
        if "tunnel" in t and t["tunnel"] != "no": tunnels["railway"] += 1
    if "waterway" in t:
        other_lin["waterway"][t["waterway"]] += 1; other_lin_km["waterway"][t["waterway"]] += ins/1000
        if "bridge" in t and t["bridge"] != "no": bridges["waterway"] += 1
        if "tunnel" in t and t["tunnel"] != "no": tunnels["waterway"] += 1
    if t.get("natural") == "tree_row": tree_rows += 1; other_lin["natural"]["tree_row"] += 1; other_lin_km["natural"]["tree_row"] += ins/1000
    if "barrier" in t and coords[0] != coords[-1]: barriers[t["barrier"]] += 1
    if "power" in t and t["power"] in ("line","minor_line"):
        other_lin["power"][t["power"]] += 1; other_lin_km["power"][t["power"]] += ins/1000
R["highway_ways"] = {k: {"ways": hw_cnt[k], "km_total_ways": round(hw_km[k],1), "km_in_bbox": round(hw_km_in[k],1)} for k,_ in hw_cnt.most_common()}
R["drivable_km_in_bbox"] = round(sum(v for k,v in hw_km_in.items() if k in DRIVABLE),1)
R["drivable_ways"] = sum(v for k,v in hw_cnt.items() if k in DRIVABLE)
R["bridges"] = dict(bridges); R["tunnels"] = dict(tunnels)
R["roundabout_ways"] = roundabouts
R["railway_ways"] = {k: {"ways": rail_cnt[k], "km": round(rail_km[k],1)} for k,_ in rail_cnt.most_common(12)}
R["linear_other"] = {k: {v: {"ways": c, "km": round(other_lin_km[k][v],1)} for v,c in cc.most_common(12)} for k,cc in other_lin.items()}
R["barrier_lines_top"] = barriers.most_common(10)

# ---------- tag coverage on drivable ----------
def has(t, pred): return any(pred(k) for k in t)
GROUPS = {
 "maxspeed": lambda k: k == "maxspeed" or k.startswith("maxspeed:forward") or k.startswith("maxspeed:backward"),
 "maxspeed(plain)": lambda k: k == "maxspeed",
 "maxspeed:type|zone:maxspeed|source:maxspeed": lambda k: k in ("maxspeed:type","zone:maxspeed","source:maxspeed","zone:traffic"),
 "lanes": lambda k: k == "lanes",
 "width": lambda k: k in ("width","est_width","width:carriageway"),
 "oneway": lambda k: k == "oneway",
 "surface": lambda k: k == "surface",
 "smoothness": lambda k: k == "smoothness",
 "sidewalk*": lambda k: k == "sidewalk" or k.startswith("sidewalk:"),
 "cycleway*": lambda k: k.startswith("cycleway"),
 "turn:lanes*": lambda k: k.startswith("turn:lanes"),
 "lit": lambda k: k == "lit",
 "name": lambda k: k == "name",
 "ref": lambda k: k == "ref",
 "layer": lambda k: k == "layer",
 "bridge": lambda k: k == "bridge",
 "tunnel": lambda k: k == "tunnel",
 "lane_markings": lambda k: k == "lane_markings",
 "parking:*": lambda k: k.startswith("parking:"),
 "access/motor_vehicle/vehicle": lambda k: k in ("access","motor_vehicle","vehicle"),
}
def cov(sel):
    n = len(sel); km = sum(x[2] for x in sel)
    res = {"ways": n, "km": round(km,1), "coverage": {}}
    for g, p in GROUPS.items():
        c = [x for x in sel if has(x[1], p)]
        res["coverage"][g] = {"ways_pct": round(100*len(c)/max(n,1),1), "km_pct": round(100*sum(x[2] for x in c)/max(km,1e-9),1)}
    return res
R["tag_coverage"] = {"drivable_all(excl track)": cov([x for x in drv if x[0] in DRIVABLE])}
for hw in ("motorway","trunk","primary","secondary","tertiary","unclassified","residential","service","living_street","track"):
    R["tag_coverage"][hw] = cov([x for x in drv if x[0] == hw])
dd = [x for x in drv if x[0] in DRIVABLE]
def vc(key, sel=dd, top=15, km=False):
    c = Counter()
    for hw,t,k in sel:
        if key in t: c[t[key]] += k if km else 1
    return [(a, round(b,1) if km else b) for a,b in c.most_common(top)]
for key in ("maxspeed","maxspeed:type","zone:maxspeed","source:maxspeed","lanes","oneway","surface","sidewalk","cycleway","lit","smoothness","width","junction","lane_markings"):
    R.setdefault("values", {})[key] = vc(key)
R["values"]["maxspeed_by_km"] = vc("maxspeed", km=True)
R["values"]["track_surface"] = vc("surface", [x for x in drv if x[0]=="track"])
R["values"]["track_tracktype"] = vc("tracktype", [x for x in drv if x[0]=="track"])
# sidewalk sub keys
c = Counter()
for hw,t,k in dd:
    for kk,v in t.items():
        if kk.startswith("sidewalk:") or kk.startswith("cycleway:"): c[f"{kk}={v}"] += 1
R["values"]["sidewalk_cycleway_subkeys_top"] = c.most_common(25)
# separate footway/cycleway ways (mapped sidewalks)
R["sidewalk_as_separate_ways"] = {"footway=sidewalk": 0, "footway=crossing": 0, "cycleway=crossing":0}

# ---------- nodes ----------
nd = Counter(); crossing_types = Counter(); crossing_marks=Counter(); dirinfo = Counter(); tags_other = Counter()
tree_nodes = 0; street_lamp = 0; lampkinds=Counter(); signal_sub=Counter(); tc=Counter(); busstop=0; mini_rb=0; ts_dir=0
for n in osmium.FileProcessor(path, osmium.osm.NODE):
    if not inb(n.location.lon, n.location.lat): continue
    if not len(n.tags): continue
    t = tagdict(n)
    hw = t.get("highway"); rw = t.get("railway")
    if hw: nd["highway=" + hw] += 1
    if rw: nd["railway=" + rw] += 1
    if hw == "crossing" or t.get("railway") == "crossing":
        crossing_types[t.get("crossing", "<none>")] += 1
        if "crossing:markings" in t: crossing_marks[t["crossing:markings"]] += 1
        for k in ("traffic_signals","tactile_paving","kerb","island","supervised","button_operated"):
            if k in t or f"crossing:{k}" in t: dirinfo["crossing_has_" + k] += 1
    if hw == "traffic_signals":
        for k in t:
            if k.startswith("traffic_signals"): signal_sub[k] += 1
        if "traffic_signals:direction" in t: ts_dir += 1
        if "direction" in t: dirinfo["traffic_signals_direction"] += 1
        if t.get("traffic_signals") == "signal": signal_sub["traffic_signals=signal"] += 1
        if "crossing" in t: dirinfo["traffic_signals_with_crossing_tag"] += 1
    if hw in ("stop","give_way") and "direction" in t: dirinfo[f"{hw}_with_direction={t['direction']}"] += 1
    if hw == "street_lamp": lampkinds[t.get("support","<none>")] += 1
    if t.get("natural") == "tree": tree_nodes += 1
    if "traffic_calming" in t: tc[t["traffic_calming"]] += 1
    if hw == "mini_roundabout": mini_rb += 1
    if "barrier" in t: tags_other["barrier=" + t["barrier"]] += 1
    if t.get("amenity") == "parking_entrance": tags_other["parking_entrance"] += 1
    for k in ("power","man_made","emergency","amenity","public_transport","traffic_sign"):
        if k in t: tags_other[f"{k}={t[k]}"] += 1
    if "maxspeed" in t and hw not in ("speed_camera",): tags_other["node_with_maxspeed"] += 1
R["nodes"] = {"highway_railway_counts": dict(nd.most_common(40)), "natural_tree_nodes": tree_nodes,
              "crossing_types": dict(crossing_types.most_common()), "crossing_markings": dict(crossing_marks.most_common()),
              "crossing_extra": dict(dirinfo), "traffic_signals_direction_tag": ts_dir, "traffic_signals_subkeys": dict(signal_sub),
              "street_lamp_support": dict(lampkinds), "traffic_calming": dict(tc.most_common()), "mini_roundabout": mini_rb,
              "other_top": tags_other.most_common(40)}

# ---------- relations ----------
rel = Counter(); restr = Counter(); rest_via=Counter()
for r in osmium.FileProcessor(path, osmium.osm.RELATION):
    t = tagdict(r); rel[t.get("type","?")] += 1
    if t.get("type", "").startswith("restriction"):
        restr[t.get("restriction", t.get("restriction:hgv", "?"))] += 1
        vias = [m.type for m in r.members if m.role == "via"]
        rest_via["via_" + ("node" if vias == ["n"] else "way" if "w" in vias else "none" if not vias else "other")] += 1
R["relations_in_clip"] = dict(rel)
R["restrictions"] = {"count": sum(restr.values()), "by_type": dict(restr.most_common()), "via": dict(rest_via)}

# ---------- areas ----------
bcnt = 0; bar = 0; btype = Counter(); bcov = Counter(); b_area = 0.0; b_from_rel = 0
lu = Counter(); lu_a = Counter(); nat = Counter(); nat_a = Counter(); wat = Counter(); leis = Counter(); leis_a=Counter(); amen = Counter()
for a in osmium.FileProcessor(path, osmium.osm.NODE | osmium.osm.WAY | osmium.osm.RELATION | osmium.osm.AREA).with_locations().with_areas():
    if not a.is_area(): continue
    t = tagdict(a)
    if not t: continue
    try:
        rings = [[(n.lon, n.lat) for n in r] for r in a.outer_rings()]
    except Exception: continue
    if not rings or not any(inb(x,y) for r in rings for x,y in r): continue
    ar = sum(ring_area(r) for r in rings)
    if "building" in t and t["building"] != "no":
        bcnt += 1; btype[t["building"]] += 1; b_area += ar
        if not a.from_way(): b_from_rel += 1
        for k in ("building:levels","height","roof:shape","roof:levels","building:colour","roof:colour","building:material","roof:material","min_height","building:min_level","addr:housenumber","name","roof:height","building:part"):
            if k in t: bcov[k] += 1
        if "height" in t or "building:levels" in t: bcov["height_or_levels"] += 1
    if "landuse" in t: lu[t["landuse"]] += 1; lu_a[t["landuse"]] += ar
    if "natural" in t: nat[t["natural"]] += 1; nat_a[t["natural"]] += ar
    if "water" in t: wat[t["water"]] += 1
    if "leisure" in t: leis[t["leisure"]] += 1; leis_a[t["leisure"]] += ar
    if "building" not in t:
        for k in ("amenity","aeroway","man_made","place","landcover"):
            if k in t: amen[f"{k}={t[k]}"] += 1
R["buildings"] = {"count": bcnt, "from_multipolygon_relations": b_from_rel, "total_footprint_km2": round(b_area/1e6,2),
                  "mean_footprint_m2": round(b_area/max(bcnt,1),1),
                  "tag_coverage_pct": {k: round(100*v/max(bcnt,1),2) for k,v in bcov.most_common()},
                  "type_top15": btype.most_common(15), "distinct_types": len(btype)}
R["landuse"] = {k: {"n": v, "km2": round(lu_a[k]/1e6,2)} for k,v in lu.most_common(TOPN)}
R["natural_areas"] = {k: {"n": v, "km2": round(nat_a[k]/1e6,2)} for k,v in nat.most_common(TOPN)}
R["water_subtype"] = wat.most_common(10)
R["leisure_areas"] = {k: {"n": v, "km2": round(leis_a[k]/1e6,2)} for k,v in leis.most_common(12)}
R["other_areas_top"] = amen.most_common(15)
json.dump(R, open(outp, "w"), indent=1, ensure_ascii=False)
print("ok")
