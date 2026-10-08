"""Merge sorted OSM PBFs (dedup by id/version). usage: merge_osm.py OUT IN1 IN2 ..."""
import sys, osmium
out = sys.argv[1]
mr = osmium.MergeInputReader()
for f in sys.argv[2:]:
    mr.add_file(f)
with osmium.SimpleWriter(out, overwrite=True) as w:
    mr.apply(w)
