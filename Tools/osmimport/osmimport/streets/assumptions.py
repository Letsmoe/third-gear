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
