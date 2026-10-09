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
"""
