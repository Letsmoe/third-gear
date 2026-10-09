"""The street model: how a road looks across and along, built from OSM tags and assumptions (issue #91).

OSM rarely says how wide a road is or how its space is divided, so most of the model is assumptions: German
design practice, tuned against Hamburg's street survey (compare_survey.py). The survey only measures; the model never
reads it, so it works wherever there is OSM.

The package is written to be ported to a compiled language later, so it keeps to a small subset of Python:

* plain data (dataclasses, enums, numbers, strings) and plain functions over it, no inheritance or dynamic tricks;
* every tuned number lives in assumptions.py, next to where it comes from;
* the rules (tags.py, cross_section.py) never touch geometry; geometry modules take their results as input.

Modules:

* tags.py: reading the OSM tags the model uses.
* assumptions.py: the tables of widths and counts the rules fall back on.
* cross_section.py: a road's cross-section, its strips (travel lanes, cycle lanes, bus lanes, margins) from left to
  right, and so its carriageway width.
* lines.py: the kerbs and painted lines of a cross-section, named so they can be followed into the next road, and
  which of them are painted.
* polyline.py: offsets, cuts and curves on numpy polylines.
* network.py: ways cut at junctions into segments, and what lies at each segment end.
* layout.py: each segment's lines along it, with the tapers between roads and where lines start and stop.
* corrections.py: fixes for OSM mistakes recognisable from the network (short mis-tagged narrowings).
* splits.py: dual carriageway splits, where a two-way road divides into two one-way carriageways.
* junctions.py: junction mouths, crosses and the guide lines inside junctions.
* road_lines.py: builds all of the above for a set of ways.
"""
