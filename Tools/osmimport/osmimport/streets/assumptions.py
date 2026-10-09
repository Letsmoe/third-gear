"""The numbers the street model assumes where OSM says nothing, in metres unless named otherwise.

Each table follows German design practice and is tuned against Hamburg's street survey: the median measured kerb to
kerb width of each kind of road outside bergedorf_core, which is kept for checking (compare_survey.py). Urban means
buildings line the road (roads.road_context); rural roads have none within reach or a limit of 70 km/h and more.
"""

# Width of one marked travel lane, by road class: (urban, rural). Links (slip roads) use their base class.
MARKED_LANE_WIDTH = {
    "motorway": (3.75, 3.75),
    "trunk": (3.5, 3.5),
    "primary": (3.25, 3.5),
    "secondary": (3.35, 3.35),
    "tertiary": (2.85, 3.25),
    "unclassified": (2.8, 3.0),
    "residential": (2.8, 3.0),
}
MARKED_LANE_WIDTH_OTHER = (2.75, 3.0)

# A single-lane slip road (highway=*_link) is paved wide enough to pass a broken-down car and for lorries to make
# the curve: its one travel lane gets this width (survey: 5 to 7.5 m kerb to kerb).
SINGLE_LANE_LINK_WIDTH = 5.5

# Width of a single-lane one-way carriageway without painted lanes, by road class: room for one lane and a parked
# car or a cyclist beside it.
ONE_WAY_WIDTH = {
    "secondary": 4.0,
    "tertiary": 4.0,
    "unclassified": 4.25,
    "residential": 4.4,
    "living_street": 3.5,
    "service": 3.0,
}
ONE_WAY_WIDTH_OTHER = 3.5

# Width of a two-way carriageway without painted lanes (RASt's "Begegnungsfall": two cars or a car and a lorry
# pass each other on one surface), by road class: (urban, rural).
UNMARKED_TWO_WAY_WIDTH = {
    "tertiary": (4.9, 5.0),
    "unclassified": (4.0, 3.0),
    "residential": (5.1, 5.0),
    "living_street": (3.4, 3.4),
    "service": (2.7, 3.0),
}
UNMARKED_TWO_WAY_WIDTH_OTHER = (5.0, 5.0)
# A residential street signed for 50 km/h or more is a collector street (RASt "Sammelstraße") and wider.
COLLECTOR_STREET_WIDTH = 6.3
COLLECTOR_STREET_SPEED = 50.0

# An unmarked two-way carriageway narrower than this is one shared lane: oncoming cars slow down and pass each other
# half on the verge, as on country lanes and narrow residential streets.
SHARED_LANE_MAX_WIDTH = 4.5

# Service roads only buses may use (bus stations, bus gates) have lanes as wide as bus lanes.
BUS_ROAD_LANE_WIDTH = 3.25

# Service roads by their service= tag: (two-way, one-way) carriageway width.
SERVICE_WIDTH = {
    "parking_aisle": (5.4, 3.0),
    "driveway": (2.7, 2.7),
    "alley": (2.7, 2.7),
}

# Importance of the road classes, lowest first: the higher class wins the junction surface and goes straight through.
ROAD_CLASS_RANK = {road_class: rank for rank, road_class in enumerate([
    "service", "living_street", "road", "residential", "unclassified", "tertiary_link", "tertiary", "secondary_link",
    "secondary", "primary_link", "primary", "trunk_link", "trunk", "motorway_link", "motorway"])}

# Classes whose lanes are painted when OSM does not say (no lanes= and no lane_markings=). Below tertiary, roads
# are unmarked; tertiary roads mapped without lanes= in Hamburg are mostly unmarked too.
MARKED_BY_DEFAULT = {"motorway", "trunk", "primary", "secondary"}
DEFAULT_LANES_ONE_WAY = 1
DEFAULT_LANES_TWO_WAY = 2

# Most lanes a road of each class plausibly has in one direction; more is a tagging error such as lanes=12 on a
# two-lane approach. A two-way road may have twice as many.
MAX_LANES_PER_DIRECTION = {"motorway": 4, "trunk": 4, "primary": 4, "secondary": 3, "tertiary": 3}
MAX_LANES_PER_DIRECTION_OTHER = 2

# Strips beside the travel lanes, inside the kerbs.
CYCLE_LANE_WIDTH = 1.85           # exclusive cycle lane (Radfahrstreifen) including its 0.25 m solid line
ADVISORY_CYCLE_LANE_WIDTH = 1.5   # advisory cycle lane (Schutzstreifen) including its dashed line
BUS_LANE_WIDTH = 3.25
# Gutter (Rinne) at the kerb on urban roads, paved shoulder strip (Randstreifen) on rural ones: (urban, rural).
MARGIN_WIDTH = (0.3, 0.3)

# A width= tag is taken as the kerb to kerb width where mappers measured it that way. On the classified roads of the
# survey area it is 1.2 to 2.5 m short of the kerbs (lanes only), so there it can only widen the assumption.
WIDTH_TAG_TRUSTED_CLASSES = {"residential", "unclassified", "living_street", "service", "road", "track"}
WIDTH_TAG_RANGE = (2.0, 30.0)
# Even where the tag is trusted, one this much narrower than the assumption most likely measures a single lane
# (common on one-way streets) and is ignored.
WIDTH_TAG_LANE_ONLY_GAP = 1.0
# No travel lane gets narrower than this when the strips are fitted to a width= tag.
MIN_TRAVEL_LANE_WIDTH = 2.0

# A road piece shorter than this that is narrower than the road on both sides of it is a tagging mistake (a real
# narrowing needs two tapers of up to 30 m and some road between them): it takes the narrower neighbour's
# cross-section.
SHORT_NARROWING_MAX_LENGTH = 80.0
# Narrower means by at least this much.
NARROWING_MIN_DIFFERENCE = 0.3

# ---- Lines (sources: <data root>/downloads/road_rules/NOTES.md) ----

# Classes whose lane, centre and edge lines are painted at all.
PAINTED_CLASSES = {"motorway", "motorway_link", "trunk", "trunk_link", "primary", "primary_link", "secondary",
                   "secondary_link", "tertiary", "tertiary_link"}
# Edge lines on these classes, and on rural roads from RURAL_EDGE_LINE_MIN_WIDTH.
EDGE_LINE_CLASSES = {"motorway", "trunk", "primary", "motorway_link", "trunk_link"}
RURAL_EDGE_LINE_MIN_WIDTH = 5.5
# No centre line on carriageways narrower than this, nor in Tempo 30 zones (VwV-StVO zu Zeichen 340, StVO §45 1c).
CENTRE_LINE_MIN_WIDTH = 5.5
# Lanes narrower than this are not marked (RASt 06 corrections p. 126).
MIN_MARKED_LANE_WIDTH = 2.75

# Where a road widens, narrows or gains or loses a lane, its lines move sideways over a taper ("Verziehung") of
# speed x shift / 3 metres (RASt 06 6.1.4.3: lz = V * i / 3); inside towns 10 to 20 m usually suffice and the taper
# is at most 30 m. Lines of a lane that only one road has start or end where the taper does.
TAPER_SPEED_FACTOR = 1.0 / 3.0
TAPER_MIN_LENGTH = 10.0
URBAN_TAPER_MAX_LENGTH = 30.0
# A taper takes at most this share of its road segment, so the two ends of a short segment don't overlap.
TAPER_MAX_SEGMENT_SHARE = 0.45

# Speed for the tapers where maxspeed is not tagged: the German defaults inside and outside towns.
DEFAULT_SPEED_URBAN = 50.0
DEFAULT_SPEED_RURAL = 100.0

# Between the directions, roads with two or more lanes in one direction get a double solid line (Fahrstreifen-
# begrenzung, Zeichen 295, VwV-StVO): two narrow lines this far apart, centre to centre.
DOUBLE_LINE_SPACING = 0.24
# A lane that turns where its neighbour doesn't is marked off with a broad line (0.25 m): broken along the road, solid
# over the queueing length before the junction (RASt 06 Tabelle 45: 20 m as a rule). The broken line's dashes are not
# in the free sources.
TURN_LANE_QUEUE_LENGTH = 20.0
TURN_LANE_DASH = (3.0, 3.0)

# Cycle lane lines (VwV-StVO zu Zeichen 340, Bayern Musterblatt 1): an exclusive lane has a solid broad line (0.25 m),
# an advisory one a narrow line of 1 m dashes and 1 m gaps; across junctions both become a cycle crossing ("Furt") of
# two broad lines with 0.5 m dashes and 0.2 m gaps.
ADVISORY_CYCLE_LINE_DASH = (1.0, 1.0)
CYCLE_FURT_DASH = (0.5, 0.2)

# Kerb radius at junction corners: lines stop where the corner's curve begins.
CORNER_RADIUS = 4.0
# Left-turn guide lines inside a junction: dash and gap 1:1 (RMS Teil 1 3.2.2.3); the dash length is not in the free
# sources.
GUIDE_LINE_DASH = (1.5, 1.5)
# Lane lines stop at a junction; a cross of two bars this long marks where the lines of crossing roads would meet
# (the size is not in the free sources).
JUNCTION_CROSS_LENGTH = 1.0
# The edge line of the through road continues across the mouth of a side road as a broken broad line.
EDGE_GUIDE_DASH = (1.5, 1.5)
# Where a road joins or leaves another at a shallow angle (merging and diverging lanes), the other road's edge line on
# that side stays broken until the joining carriageway is MERGE_GAP away from it.
MERGE_MAX_ANGLE = 45.0
MERGE_GAP = 2.0
# A two-way road and two one-way carriageways meeting at a node, each within this angle of straight on, are a dual
# carriageway split (splits.py).
SPLIT_MAX_DEFLECTION = 45.0
# Two arms are one road going straight through a junction when their directions differ by at most this much.
THROUGH_MAX_DEFLECTION_DEGREES = 35.0
# Side arms of these classes (driveways, parking aisles) don't interrupt the lines of the road they join.
MINOR_ARM_CLASSES = {"service", "track"}
# Left turns get guide lines around the corner at signalised junctions (a signal within this distance) when the
# approach has its own left-turn lanes (turn:lanes).
SIGNAL_JUNCTION_RADIUS = 40.0
# Spacing of the points of generated lines.
LINE_POINT_SPACING = 1.0
