"""Clip one OSM PBF to a bbox, keeping complete ways/multipolygons/restrictions.

usage: clip_osm.py IN.osm.pbf OUT.osm.pbf [W S E N]
"""
import sys, time
import osmium

KEEP_REL = {"multipolygon", "restriction", "restriction:hgv", "restriction:conditional"}


def clip(src, dst, bbox):
    w, s, e, n = bbox
    t0 = time.time()
    tr = osmium.IdTracker()
    stats = dict(nodes_tagged=0, nodes_in=0, ways=0, rels=0)
    with osmium.BackReferenceWriter(dst, ref_src=src, overwrite=True, remove_tags=False) as wr:
        for o in osmium.FileProcessor(src, osmium.osm.NODE):
            loc = o.location
            if w <= loc.lon <= e and s <= loc.lat <= n:
                tr.add_node(o.id)
                stats["nodes_in"] += 1
                if len(o.tags):
                    wr.add_node(o)
                    stats["nodes_tagged"] += 1
        print("nodes done", time.time() - t0, stats, file=sys.stderr)
        for o in osmium.FileProcessor(src, osmium.osm.WAY):
            if tr.contains_any_references(o):
                tr.add_way(o.id)
                wr.add_way(o)
                stats["ways"] += 1
        print("ways done", time.time() - t0, stats, file=sys.stderr)
        for o in osmium.FileProcessor(src, osmium.osm.RELATION):
            if o.tags.get("type") in KEEP_REL and tr.contains_any_references(o):
                wr.add_relation(o)
                stats["rels"] += 1
    print("done", time.time() - t0, stats, file=sys.stderr)


if __name__ == "__main__":
    bbox = tuple(float(x) for x in sys.argv[3:7]) if len(sys.argv) >= 7 else (10.05, 53.40, 10.33, 53.53)
    clip(sys.argv[1], sys.argv[2], bbox)
