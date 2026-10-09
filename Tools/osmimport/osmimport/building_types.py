"""Building typology from OSM alone: class, roof, storeys and orientation for every building.

The classes are those of Tools/buildingkit/typology.md. Nothing here needs more than the OSM extract of the region,
so the same rules run for any German region. A tag is trusted when it is there (building, building:levels, height,
roof:shape, roof:levels, roof:angle, roof:orientation, start_date); everything else is inferred from

* the footprint: area, rectangle, how rectangular it is, how many corners it has,
* the neighbours: party walls (a closed row or a terrace), the pairs of a semi-detached house, repeated identical
  footprints (a planned estate), free space around the building (plot size),
* the surroundings: how densely built the area is (centre, suburb, village), landuse, the nearest street and its class
  (a pedestrian street means shops), street names ("Deich" means the marsh dykes of Vierlande).

Where the evidence does not decide, the choice is a deterministic draw (hash of the OSM id) from the shares in the
typology, so a street gets a plausible mix and the same building always comes out the same.

Frame of a building. Every building gets a facade axis t (along the street or the party walls) and a depth axis n
pointing from the street into the building's back; W and D are its extents along them. The ridge runs along the long
side unless the building is part of a row, then it runs along the row.
"""
import math
import re
from dataclasses import dataclass

import numpy as np
import shapely
from scipy.spatial import cKDTree
from shapely.strtree import STRtree

from .buildings import _float, _hash01

# Typology classes (ids are stored in the tile, never renumber).
CLASS_NAMES = [
    "gruenderzeit_clinker",    # 0  Gründerzeit and Jugendstil town house
    "brick_block_1920s",       # 1  1920s and 30s brick block
    "postwar_plaster",         # 2  postwar plaster house and row
    "slab_block",              # 3  1960s to 1970s slab block and high-rise
    "terraced",                # 4
    "semidetached",            # 5
    "detached_postwar",        # 6
    "villa",                   # 7
    "modern",                  # 8  1990s town house and modern plaster and glass
    "commercial_groundfloor",  # 9  shop and office house
    "vierlande_farmhouse",     # 10 farmhouse and barn
    "shed_garage",             # 11 garage, shed, carport, outbuilding
    "industrial_hall",         # 12
    "public",                  # 13 school, church, station and other institutional buildings
    "retail_centre",           # 14 shopping centre, department store, supermarket, DIY store
    "halftimbered_town",       # 15 historic half-timbered town house of the old centre (Sachsentor)
]
CLASS_ID = {name: index for index, name in enumerate(CLASS_NAMES)}

# Roof shapes (0 and 1 are the ones the tile format had before).
ROOF_NAMES = ["flat", "gabled", "hipped", "half_hipped", "mansard", "gambrel", "pyramidal", "skillion", "round"]
ROOF_ID = {name: index for index, name in enumerate(ROOF_NAMES)}

OSM_ROOF_SHAPES = {
    "flat": "flat", "gabled": "gabled", "hipped": "hipped", "half-hipped": "half_hipped", "mansard": "mansard",
    "gambrel": "gambrel", "pyramidal": "pyramidal", "skillion": "skillion", "round": "round", "dome": "round",
    "saltbox": "gabled", "double_saltbox": "hipped", "quadruple_saltbox": "hipped", "side_hipped": "half_hipped",
    "side_half-hipped": "half_hipped", "gabled_height_moved": "gabled", "hip_and_gable": "half_hipped",
    "butterfly": "gabled", "sawtooth": "gabled",
}

# Flag bits of a classified building.
FLAG_ATTIC_HABITABLE = 1      # a living level under the roof: dormers and roof windows
FLAG_SHOP_GROUND_FLOOR = 2    # ground floor is a shop front
FLAG_CLOSED_LEFT = 4          # party wall on the left of the facade, seen from the street
FLAG_CLOSED_RIGHT = 8         # party wall on the right of the facade
FLAG_CORNER = 16              # stands at a street corner (bay window, chamfered corner)
FLAG_COMPLEX_FOOTPRINT = 32   # not a rectangle: L, U or many corners, the roof is built from the rectangle anyway
FLAG_BARN = 64                # farm building without living space
FLAG_TOWER = 128              # high-rise point block

# Which values came from OSM tags, as bits (the rest is inferred).
TAG_CLASS = 1
TAG_LEVELS = 2
TAG_ROOF_SHAPE = 4
TAG_HEIGHT = 8
TAG_START_DATE = 16
TAG_ROOF_LEVELS = 32

OUTBUILDING_TAGS = {"garage", "garages", "carport", "shed", "hut", "roof", "greenhouse", "allotment_house", "cabin",
                    "service", "kiosk", "container", "toilets", "storage_tank", "gazebo", "pavilion", "boathouse",
                    "ruins", "construction", "digester", "silo", "tank"}
PUBLIC_TAGS = {"school", "kindergarten", "university", "college", "hospital", "public", "civic", "government",
               "church", "chapel", "cathedral", "mosque", "synagogue", "temple", "train_station", "fire_station",
               "transportation", "stadium", "sports_hall", "sports_centre", "museum", "theatre", "townhall",
               "clinic", "religious", "monastery", "bridge", "castle", "police", "prison", "grandstand"}
PUBLIC_AMENITIES = {"school", "kindergarten", "university", "college", "hospital", "place_of_worship", "townhall",
                    "fire_station", "police", "library", "theatre", "community_centre", "courthouse", "clinic",
                    "social_facility", "arts_centre", "museum"}
INDUSTRIAL_TAGS = {"industrial", "warehouse", "manufacture", "factory", "hangar", "depot", "storage", "logistics"}
COMMERCIAL_TAGS = {"retail", "commercial", "office", "supermarket", "kiosk", "mall", "hotel"}
FARM_TAGS = {"farm", "barn", "farm_auxiliary", "stable", "cowshed", "sty", "livestock"}
RESIDENTIAL_TAGS = {"house", "detached", "semidetached_house", "terrace", "apartments", "residential", "yes",
                    "dormitory", "bungalow", "duplex", "semi", "townhouse", "villa", "block"}
BIG_SHOP_VALUES = {"mall", "supermarket", "department_store", "doityourself", "trade", "wholesale", "furniture",
                   "electronics", "garden_centre"}
SHOP_AMENITIES = {"restaurant", "cafe", "fast_food", "bank", "pharmacy", "bar", "pub", "post_office", "dentist",
                  "doctors", "ice_cream", "biergarten", "marketplace", "bureau_de_change"}

# Nominal roof pitch range in degrees per class, and the storey data (storey height, ground floor height, plinth).
CLASS_PITCH = {
    "gruenderzeit_clinker": (40, 50), "brick_block_1920s": (45, 55), "postwar_plaster": (30, 40),
    "terraced": (30, 45), "semidetached": (35, 50), "detached_postwar": (30, 45), "villa": (45, 58),
    "commercial_groundfloor": (35, 48), "vierlande_farmhouse": (48, 55), "shed_garage": (18, 28),
    "public": (35, 50), "modern": (20, 28), "slab_block": (3, 5), "industrial_hall": (3, 8),
    "retail_centre": (3, 5), "halftimbered_town": (45, 55),
}
# class -> (storey height m, ground floor height m, plinth above terrain m)
CLASS_STOREY = {
    "gruenderzeit_clinker": (3.3, 3.8, 0.9), "brick_block_1920s": (3.0, 3.2, 0.5), "postwar_plaster": (2.75, 2.75, 0.5),
    "slab_block": (2.8, 2.8, 0.3), "terraced": (2.75, 2.75, 0.4), "semidetached": (2.8, 2.8, 0.5),
    "detached_postwar": (2.75, 2.75, 0.4), "villa": (3.4, 3.6, 0.8), "modern": (2.8, 2.8, 0.2),
    "commercial_groundfloor": (3.2, 4.0, 0.1), "vierlande_farmhouse": (2.3, 2.3, 0.5), "shed_garage": (2.8, 2.8, 0.1),
    "industrial_hall": (7.0, 7.0, 0.6), "public": (3.5, 3.8, 0.5),
    "retail_centre": (4.5, 5.0, 0.2), "halftimbered_town": (2.9, 3.2, 0.5),
}

# Estate detection: this many identical footprints nearby mean a planned estate.
ESTATE_REPEATS = 6
DENSITY_RADIUS = 100.0
PARTY_WALL_MIN_LENGTH = 2.0
PARTY_WALL_TOLERANCE = 0.35


@dataclass
class BuildingType:
    """Result of the classification of one building."""
    class_id: int
    roof_shape: int
    pitch_degrees: float
    storeys: int
    attic_levels: int
    storey_height: float
    ground_height: float
    plinth: float
    eave_height: float
    ridge_yaw: float
    front_yaw: float
    flags: int
    tag_bits: int


class _Street:
    """One street polyline with the attributes the rules look at."""

    def __init__(self, line, highway, name):
        self.line = line
        self.highway = highway
        self.name = name or ""


def _unit(vector):
    length = float(np.hypot(vector[0], vector[1]))
    if length < 1e-9:
        return np.array([1.0, 0.0])
    return np.asarray(vector, dtype=np.float64) / length


def _yaw_degrees(vector):
    """Direction of a world vector as a yaw in Unreal's sense (x east, y south, positive turns toward +y)."""
    return math.degrees(math.atan2(vector[1], vector[0])) % 360.0


def _parse_year(value):
    """First four-digit year in a start_date value such as '1898', '1890-1905' or 'C19'; None if there is none."""
    if not value:
        return None
    century = re.match(r"^\s*~?C(\d\d)\s*$", str(value))
    if century:
        return (int(century.group(1)) - 1) * 100 + 50
    digits = ""
    for character in str(value):
        if character.isdigit():
            digits += character
            if len(digits) == 4:
                return int(digits)
        else:
            digits = ""
    return None


def _weighted_pick(osm_id, salt, options):
    """Deterministic draw from [(value, weight), ...]."""
    total = sum(weight for _, weight in options)
    draw = _hash01(osm_id, salt) * total
    for value, weight in options:
        draw -= weight
        if draw <= 0:
            return value
    return options[-1][0]


def _range_pick(osm_id, salt, low, high):
    return low + (high - low) * _hash01(osm_id, salt)


class _Footprint:
    """Geometry facts of one building footprint."""

    def __init__(self, polygon):
        self.polygon = polygon
        self.area = polygon.area
        self.centroid = np.array(polygon.centroid.coords[0])
        rect = polygon.minimum_rotated_rectangle
        if isinstance(rect, shapely.Polygon):
            corners = np.asarray(rect.exterior.coords)[:4]
        else:
            minx, miny, maxx, maxy = polygon.bounds
            corners = np.array([[minx, miny], [maxx, miny], [maxx, maxy], [minx, maxy]])
        first = corners[1] - corners[0]
        second = corners[2] - corners[1]
        if np.hypot(*first) >= np.hypot(*second):
            self.long_axis, self.short_axis = _unit(first), _unit(second)
            self.long_length, self.short_length = float(np.hypot(*first)), float(np.hypot(*second))
        else:
            self.long_axis, self.short_axis = _unit(second), _unit(first)
            self.long_length, self.short_length = float(np.hypot(*second)), float(np.hypot(*first))
        rect_area = max(self.long_length * self.short_length, 1e-6)
        self.rectangularity = min(1.0, self.area / rect_area)
        self.corner_count = len(polygon.exterior.coords) - 1
        self.aspect = self.long_length / max(self.short_length, 0.1)
        self.rect_corners = corners


class BuildingTyper:
    """Classifies the buildings of one region. Build it once with all buildings and streets, then call classify()."""

    def __init__(self, buildings, roads, footways, areas, points=()):
        """buildings: list of (osm_id, tags, footprint polygon); roads, footways: osm.Way lists;
        areas: (id, tags, geometry) list with the landuse polygons."""
        self.items = list(buildings)
        self.footprints = [_Footprint(footprint) for _, _, footprint in self.items]
        count = len(self.items)
        self.polygons = [f.polygon for f in self.footprints]
        self.tree = STRtree(self.polygons)
        self.centroids = np.array([f.centroid for f in self.footprints]) if count else np.zeros((0, 2))
        self.areas_m2 = np.array([f.area for f in self.footprints]) if count else np.zeros(0)
        self._build_streets(roads, footways)
        self._build_landuse(areas)
        self._build_density()
        self._build_contacts()
        self._build_estates()

    # ------------------------------------------------------------------ context

    def _build_streets(self, roads, footways):
        """Streets by class: main (drivable except service), service ways, and pedestrian streets."""
        main, service, pedestrian = [], [], []
        for way in roads:
            if len(way.xy) < 2:
                continue
            street = _Street(shapely.LineString(way.xy), way.tags.get("highway", ""), way.tags.get("name"))
            (service if street.highway == "service" else main).append(street)
        for way in footways:
            if way.tags.get("highway") == "pedestrian" and len(way.xy) >= 2:
                pedestrian.append(_Street(shapely.LineString(way.xy), "pedestrian", way.tags.get("name")))
        self.main_streets = main
        self.service_streets = service
        self.pedestrian_streets = pedestrian
        self.main_tree = STRtree([s.line for s in main]) if main else None
        self.service_tree = STRtree([s.line for s in service]) if service else None
        self.pedestrian_tree = STRtree([s.line for s in pedestrian]) if pedestrian else None

    def _build_landuse(self, areas):
        """Landuse polygons sorted so the smallest containing one wins."""
        wanted = {"residential", "commercial", "retail", "industrial", "farmland", "farmyard", "allotments",
                  "garages", "railway", "cemetery", "meadow", "orchard"}
        polygons, uses = [], []
        for _, tags, geom in areas:
            use = tags.get("landuse")
            if use in wanted:
                polygons.append(geom)
                uses.append(use)
        order = sorted(range(len(polygons)), key=lambda index: polygons[index].area)
        self.landuse_polygons = [polygons[i] for i in order]
        self.landuse_names = [uses[i] for i in order]
        self.landuse_tree = STRtree(self.landuse_polygons) if self.landuse_polygons else None

    def _build_density(self):
        """Share of the ground covered by buildings within DENSITY_RADIUS of every building, and the plot freedom."""
        if not len(self.items):
            self.built_ratio = np.zeros(0)
            return
        kd_tree = cKDTree(self.centroids)
        disc = math.pi * DENSITY_RADIUS ** 2
        neighbours = kd_tree.query_ball_point(self.centroids, DENSITY_RADIUS)
        self.built_ratio = np.array([self.areas_m2[idx].sum() / disc for idx in neighbours])
        self.kd_tree = kd_tree

    def _build_contacts(self):
        """Party walls: for every building the touching neighbours as (index, shared length, offset vector)."""
        count = len(self.items)
        self.contacts = [[] for _ in range(count)]
        query = self.tree.query(self.polygons, predicate="dwithin", distance=PARTY_WALL_TOLERANCE)
        boundaries = [p.boundary for p in self.polygons]
        for first, second in zip(query[0], query[1]):
            if first == second:
                continue
            band = self.polygons[first].buffer(PARTY_WALL_TOLERANCE)
            shared = band.intersection(boundaries[second])
            if shared.length < PARTY_WALL_MIN_LENGTH:
                continue
            point = shared.representative_point()
            self.contacts[first].append((int(second), float(shared.length) / 1.0, np.array(point.coords[0])))
        self.component = self._components()
        self.group_sizes = {}
        for index, area in enumerate(self.areas_m2):
            if area >= 35:
                self.group_sizes[self.component[index]] = self.group_sizes.get(self.component[index], 0) + 1

    def _components(self):
        """Connected groups of party-wall neighbours ignoring tiny outbuildings; returns the group id per building."""
        parent = list(range(len(self.items)))

        def find(index):
            while parent[index] != index:
                parent[index] = parent[parent[index]]
                index = parent[index]
            return index

        for index, contact_list in enumerate(self.contacts):
            if self.areas_m2[index] < 35:
                continue
            for other, _, _ in contact_list:
                if self.areas_m2[other] >= 35:
                    parent[find(index)] = find(other)
        return [find(index) for index in range(len(self.items))]

    def _build_estates(self):
        """Count identical footprints (same area and aspect) within 120 m: planned estates repeat one plan."""
        count = len(self.items)
        self.repeats = np.zeros(count, dtype=int)
        if not count:
            return
        aspects = np.array([f.aspect for f in self.footprints])
        neighbours = self.kd_tree.query_ball_point(self.centroids, 120.0)
        for index, others in enumerate(neighbours):
            others = np.asarray(others, dtype=int)
            if self.areas_m2[index] < 35 or len(others) < ESTATE_REPEATS:
                continue
            same_area = np.abs(self.areas_m2[others] - self.areas_m2[index]) < 0.04 * self.areas_m2[index]
            same_aspect = np.abs(aspects[others] - aspects[index]) < 0.06 * aspects[index]
            self.repeats[index] = int(np.count_nonzero(same_area & same_aspect)) - 1

    def _landuse_at(self, point):
        if self.landuse_tree is None:
            return None
        hits = self.landuse_tree.query(shapely.Point(point), predicate="within")
        if not len(hits):
            return None
        return self.landuse_names[int(hits.min())]  # sorted by area, so the smallest comes first

    def _nearest_street(self, polygon):
        """(street, distance, tangent, nearest point) of the closest street, main streets preferred."""
        best = None
        for tree, streets, preference in ((self.main_tree, self.main_streets, 0.0),
                                          (self.pedestrian_tree, self.pedestrian_streets, 0.0),
                                          (self.service_tree, self.service_streets, 12.0)):
            if tree is None:
                continue
            found = tree.query_nearest(polygon, max_distance=120.0, return_distance=True, all_matches=False)
            if not len(found[0]):
                continue
            distance = float(found[1][0])
            if best is None or distance + preference < best[1] + (12.0 if best[0].highway == "service" else 0.0):
                best = (streets[int(found[0][0])], distance)
        if best is None:
            return None
        street, distance = best
        line = street.line
        nearest = line.interpolate(line.project(polygon.centroid))
        along = line.project(nearest)
        ahead = line.interpolate(min(along + 1.0, line.length))
        behind = line.interpolate(max(along - 1.0, 0.0))
        tangent = _unit(np.array(ahead.coords[0]) - np.array(behind.coords[0]))
        return street, distance, tangent, np.array(nearest.coords[0])

    def _near_pedestrian(self, polygon, distance):
        if self.pedestrian_tree is None:
            return False
        return len(self.pedestrian_tree.query(polygon, predicate="dwithin", distance=distance)) > 0

    # ------------------------------------------------------------------ frame

    def _frame(self, index, street_info):
        """Facade axis t, depth axis n (toward the back of the building) and the party-wall sides.

        Returns (t, n, width, depth, closed_left, closed_right, party_count)."""
        footprint = self.footprints[index]
        axes = [footprint.long_axis, footprint.short_axis]
        offsets = [0.0, 0.0]
        party_count = 0
        for other, length, point in self.contacts[index]:
            if self.areas_m2[other] < 20:
                continue
            party_count += 1
            direction = point - footprint.centroid
            for axis_index, axis in enumerate(axes):
                offsets[axis_index] += abs(float(direction @ axis)) * length
        if party_count:
            t = axes[0] if offsets[0] >= offsets[1] else axes[1]
        elif street_info is not None and street_info[1] < 40.0:
            t = axes[0] if abs(float(axes[0] @ street_info[2])) >= abs(float(axes[1] @ street_info[2])) else axes[1]
        else:
            t = axes[0]
        n = np.array([-t[1], t[0]])
        if street_info is not None:
            toward_street = street_info[3] - footprint.centroid
            if float(n @ toward_street) > 0:
                n = -n  # n points away from the street, into the building's back
        width = footprint.long_length if t is axes[0] else footprint.short_length
        depth = footprint.short_length if t is axes[0] else footprint.long_length
        closed_left = closed_right = False
        for other, length, point in self.contacts[index]:
            if self.areas_m2[other] < 20:
                continue
            side = float((point - footprint.centroid) @ t)
            if abs(side) < 0.3 * width:
                continue
            # left/right as seen from the street looking at the facade: facing -n, right is -n rotated
            right_axis = np.array([n[1], -n[0]])
            if float((point - footprint.centroid) @ right_axis) > 0:
                closed_right = True
            else:
                closed_left = True
        return t, n, width, depth, closed_left, closed_right, party_count

    # ------------------------------------------------------------------ classification

    def classify(self, index):
        """Classifies building number index (position in the list given to the constructor)."""
        osm_id, tags, _ = self.items[index]
        footprint = self.footprints[index]
        street_info = self._nearest_street(footprint.polygon)
        t, n, width, depth, closed_left, closed_right, party_count = self._frame(index, street_info)
        facts = _Facts(self, index, osm_id, tags, footprint, street_info, t, n, width, depth, closed_left,
                       closed_right, party_count)
        class_name, flags, tag_bits = _decide_class(facts)
        return _complete(facts, class_name, flags, tag_bits)

    def classify_all(self):
        return [self.classify(index) for index in range(len(self.items))]


@dataclass
class _Facts:
    """Everything the rules look at for one building."""
    typer: BuildingTyper
    index: int
    osm_id: int
    tags: dict
    footprint: _Footprint
    street_info: tuple
    t: np.ndarray
    n: np.ndarray
    width: float
    depth: float
    closed_left: bool
    closed_right: bool
    party_count: int

    def __post_init__(self):
        typer = self.typer
        self.area = self.footprint.area
        self.built_ratio = float(typer.built_ratio[self.index])
        self.repeats = int(typer.repeats[self.index])
        self.landuse = typer._landuse_at(self.footprint.centroid)
        self.row_size = self._row_size()
        self.building_tag = self.tags.get("building", "yes")
        self.year = _parse_year(self.tags.get("start_date"))
        self.levels = _float(self.tags.get("building:levels"))
        self.street_distance = self.street_info[1] if self.street_info else 999.0
        self.street_name = self.street_info[0].name if self.street_info else ""
        self.street_class = self.street_info[0].highway if self.street_info else ""
        self.near_pedestrian = typer._near_pedestrian(self.footprint.polygon, 25.0)
        self.free_distance = self._free_distance()

    def _row_size(self):
        """Number of main buildings in the party-wall group this building belongs to."""
        typer = self.typer
        return typer.group_sizes.get(typer.component[self.index], 1)

    def _free_distance(self):
        """Distance to the nearest other main building (a proxy for the plot size)."""
        typer = self.typer
        found = typer.tree.query(self.footprint.polygon.buffer(40.0))
        best = 40.0
        for other in found:
            if other == self.index or typer.areas_m2[other] < 45:
                continue
            best = min(best, float(self.footprint.polygon.distance(typer.polygons[other])))
        return best

    @property
    def is_closed_row(self):
        return self.closed_left and self.closed_right

    @property
    def is_attached(self):
        return self.closed_left or self.closed_right

    @property
    def is_urban(self):
        return self.built_ratio >= 0.22

    def draw(self, salt, options):
        """Draw for class decisions; attached buildings use their row's key so a street front stays one style."""
        return _weighted_pick(_roof_draw_key(self), salt, options)

    def uniform(self, salt):
        return _hash01(self.osm_id, salt)


def _decide_class(facts: _Facts):
    """The class name, flag bits and tag bits for a building; the tag rules come first, then geometry."""
    tag = facts.building_tag
    tags = facts.tags
    flags = 0
    tag_bits = 0
    if facts.year:
        tag_bits |= TAG_START_DATE
    if facts.levels:
        tag_bits |= TAG_LEVELS
    amenity = tags.get("amenity")
    shop = "shop" in tags or amenity in SHOP_AMENITIES or "office" in tags

    big_shop = tags.get("shop") in BIG_SHOP_VALUES or facts.landuse == "retail"
    if big_shop and facts.area >= 450 and tag not in OUTBUILDING_TAGS and tag not in PUBLIC_TAGS:
        return "retail_centre", flags, tag_bits | (TAG_CLASS if tag != "yes" else 0)
    if tag in OUTBUILDING_TAGS:
        return "shed_garage", flags, tag_bits | TAG_CLASS
    if tag in PUBLIC_TAGS or amenity in PUBLIC_AMENITIES:
        return "public", flags, tag_bits | TAG_CLASS
    if tag in INDUSTRIAL_TAGS:
        return "industrial_hall", flags, tag_bits | TAG_CLASS
    if tag in COMMERCIAL_TAGS:
        return _commercial_or_hall(facts, flags, tag_bits | TAG_CLASS)
    if tag in FARM_TAGS:
        return _farm(facts, flags, tag_bits | TAG_CLASS)
    if tag not in RESIDENTIAL_TAGS:
        return "shed_garage" if facts.area < 60 else "public", flags, tag_bits

    # Untagged or residential from here on.
    if tag == "yes":
        guessed = _guess_untagged(facts)
        if guessed is not None:
            name, extra = guessed
            return name, flags | extra, tag_bits
    return _residential(facts, shop, flags, tag_bits)


def _commercial_or_hall(facts, flags, tag_bits):
    """Retail, commercial and office buildings: big single storey sheds are halls, the rest shop-and-office houses."""
    big = facts.area >= 500 and (facts.levels or 1) <= 3
    if big and facts.building_tag in {"retail", "supermarket", "mall"}:
        return "retail_centre", flags, tag_bits
    if big or facts.area >= 1200:
        return "industrial_hall", flags, tag_bits
    return "commercial_groundfloor", flags | FLAG_SHOP_GROUND_FLOOR, tag_bits


def _farm(facts, flags, tag_bits):
    """Farm buildings: the house and the barn are both class 11 of the typology; auxiliary buildings are sheds."""
    if facts.building_tag in {"farm_auxiliary", "stable", "cowshed", "sty", "livestock", "barn"}:
        if facts.area < 60:
            return "shed_garage", flags, tag_bits
        return "vierlande_farmhouse", flags | FLAG_BARN, tag_bits
    return "vierlande_farmhouse", flags, tag_bits


def _guess_untagged(facts):
    """building=yes without anything else: sheds, halls and farm buildings by size and surroundings.

    Returns (class name, extra flags) or None when it is a housing candidate."""
    area = facts.area
    if area < 35:
        return "shed_garage", 0
    if area < 70 and facts.free_distance > 3.0 and facts.row_size == 1 and not facts.levels:
        return "shed_garage", 0
    if area < 60 and facts.levels is None and facts.street_distance > 25.0:
        return "shed_garage", 0
    if area >= 600 and facts.landuse in {"industrial", "commercial", "retail"}:
        return "industrial_hall", 0
    if area >= 1500 and (facts.levels or 1) <= 2:
        return "industrial_hall", 0
    deich = "deich" in facts.street_name.lower()
    if facts.landuse in {"farmyard", "farmland"} and area >= 120 and facts.built_ratio < 0.1:
        barn = facts.footprint.aspect >= 1.8 and facts.levels is None
        return "vierlande_farmhouse", FLAG_BARN if barn else 0
    if deich and facts.built_ratio < 0.1 and area >= 200 and facts.footprint.aspect >= 1.8:
        return "vierlande_farmhouse", 0
    return None


def _residential(facts, shop, flags, tag_bits):
    """Housing: the order of the checks is from the most specific evidence to the default house."""
    tag = facts.building_tag
    area = facts.area
    footprint = facts.footprint
    year = facts.year
    levels = facts.levels
    heritage = "heritage" in facts.tags

    if tag == "terrace":
        if levels is not None and levels >= 4 and facts.width >= 7.0:
            return "gruenderzeit_clinker", flags, tag_bits | TAG_CLASS
        return "terraced", flags, tag_bits | TAG_CLASS
    if tag in {"semidetached_house", "duplex", "semi"}:
        return "semidetached", flags, tag_bits | TAG_CLASS

    if _is_old_town_house(facts):
        return "halftimbered_town", flags | (FLAG_SHOP_GROUND_FLOOR if shop or _shop_street_frontage(facts) else 0), tag_bits

    if shop or _shop_street_frontage(facts):
        return "commercial_groundfloor", flags | FLAG_SHOP_GROUND_FLOOR, tag_bits

    if year is not None:
        by_date = _class_by_year(facts, year)
        if by_date:
            return by_date, flags, tag_bits

    if _is_tower_or_slab(facts):
        tower = footprint.aspect < 1.8 and area >= 220
        return "slab_block", flags | (FLAG_TOWER if tower else 0), tag_bits

    if facts.repeats >= ESTATE_REPEATS and facts.building_tag in {"apartments", "residential", "house", "yes"} \
            and (levels or 3) >= 3 and facts.area > 90:
        return "modern", flags, tag_bits

    if facts.row_size == 2 and 6.0 <= facts.width <= 10.0 \
            and area <= 160 and (levels or 2) <= 3 and not facts.is_urban:
        return "semidetached", flags, tag_bits

    if _is_row_house(facts):
        return "terraced", flags, tag_bits

    if (facts.is_attached or facts.row_size >= 2) and facts.is_urban and area >= 60:
        return _urban_block_class(facts, flags, tag_bits, heritage)

    if tag == "apartments" or (levels is not None and levels >= 3) or area >= 260:
        return _apartment_or_big_house(facts, flags, tag_bits, heritage)

    return _detached_or_villa(facts, flags, tag_bits, heritage)


def _is_old_town_house(facts):
    """Half-timbered town house of the old centre: dated before 1870, or a low gabled house in a closed front on a
    pedestrian street (the Sachsentor type, which is mostly undated in OSM)."""
    if facts.area > 320 or facts.area < 50:
        return False
    if facts.year is not None and facts.year < 1870:
        return facts.is_urban or facts.near_pedestrian
    if not (facts.near_pedestrian and (facts.is_attached or facts.row_size >= 2)):
        return False
    low = (facts.levels or 2) <= 3
    pitched = facts.tags.get("roof:shape") in {None, "gabled", "hipped"}
    return low and pitched and facts.year is None and facts.uniform(18) < 0.7


def _shop_street_frontage(facts):
    """A building in a closed frontage on a pedestrian street or along a retail landuse is a shop house."""
    if not facts.is_attached and facts.row_size < 2:
        return False
    if facts.area < 70 or facts.area > 450:
        return False
    if facts.near_pedestrian and facts.street_distance < 15.0 and facts.built_ratio > 0.2:
        return True
    return facts.landuse == "retail" and facts.street_distance < 12.0


def _class_by_year(facts, year):
    """Class from a start_date, for housing-sized buildings."""
    area = facts.area
    if year >= 1985:
        return "modern" if area > 90 else None
    if year <= 1918:
        return _gruenderzeit_or_villa(facts)
    if year <= 1939:
        if area >= 400 or facts.footprint.long_length >= 30:
            return "brick_block_1920s"
        return None
    if year <= 1979:
        if area >= 250 and (facts.levels or 0) >= 4:
            return "slab_block"
        return "postwar_plaster" if area >= 100 else None
    return None


def _gruenderzeit_or_villa(facts):
    """A pre-1918 house: villa when it stands free on a big plot and has an irregular plan."""
    free = facts.free_distance >= 8.0 and not facts.is_attached
    irregular = facts.footprint.rectangularity < 0.85 or facts.footprint.corner_count >= 8
    if free and irregular and 120 <= facts.area <= 420:
        return "villa"
    return "gruenderzeit_clinker"


def _is_tower_or_slab(facts):
    """Free-standing big block in an open building pattern: long thin slab or a point block."""
    footprint = facts.footprint
    levels = facts.levels
    if facts.building_tag not in {"apartments", "residential", "yes", "dormitory"}:
        return False
    if (facts.is_attached or facts.row_size > 1) and facts.building_tag != "apartments":
        return False
    if levels is not None and levels >= 8:
        return True
    open_pattern = facts.built_ratio < 0.30 and (facts.free_distance >= 6.0 or facts.street_distance >= 9.0)
    if not open_pattern:
        return False
    slab = footprint.long_length >= 28.0 and 9.0 <= footprint.short_length <= 18.0
    point = footprint.aspect < 1.5 and footprint.short_length >= 16.0 and (levels or 0) >= 6
    tall_enough = levels is None or levels >= 4
    return (slab or point) and tall_enough and facts.area >= 280


def _is_row_house(facts):
    """Narrow deep units in a row of at least three."""
    if facts.row_size < 3 or facts.width > 10.5 or facts.depth < 4.5 or facts.area > 130:
        return False
    if facts.levels is not None and facts.levels >= 4:
        return False
    return not (facts.is_urban and facts.width >= 7.0 and (facts.levels or 0) >= 3)


def _urban_block_class(facts, flags, tag_bits, heritage):
    """A house in a street frontage in a built-up area: Gründerzeit, 1920s block or later infill."""
    levels = facts.levels
    roof_tag = facts.tags.get("roof:shape")
    footprint = facts.footprint
    if heritage:
        return "gruenderzeit_clinker", flags, tag_bits
    long_brick_block = footprint.long_length >= 30.0 and 9.0 <= footprint.short_length <= 15.0
    if roof_tag == "flat" and (levels or 3) >= 3:
        choice = facts.draw(11, [("postwar_plaster", 5), ("modern", 2), ("slab_block", 1)])
        return choice, flags, tag_bits
    if long_brick_block:
        choice = facts.draw(12, [("brick_block_1920s", 6), ("postwar_plaster", 3), ("gruenderzeit_clinker", 1)])
        return choice, flags, tag_bits
    centrality = min(1.0, facts.built_ratio / 0.4)
    weights = [("gruenderzeit_clinker", 4 + 4 * centrality), ("brick_block_1920s", 2), ("postwar_plaster", 3 - centrality),
               ("modern", 0.6)]
    return facts.draw(13, weights), flags, tag_bits


def _apartment_or_big_house(facts, flags, tag_bits, heritage):
    """Apartment buildings and large houses that are not in a closed frontage."""
    levels = facts.levels
    footprint = facts.footprint
    if heritage and facts.free_distance >= 6.0:
        return "villa", flags, tag_bits
    if facts.area >= 420 and facts.free_distance >= 8.0 and (levels or 0) <= 2 and facts.built_ratio < 0.12 \
            and facts.landuse in {None, "farmland", "farmyard"}:
        return "vierlande_farmhouse", flags | FLAG_BARN, tag_bits
    roof_tag = facts.tags.get("roof:shape")
    if roof_tag == "flat" and (levels or 3) >= 3:
        return facts.draw(14, [("modern", 5), ("postwar_plaster", 3)]), flags, tag_bits
    long_block = footprint.long_length >= 28.0 and footprint.short_length <= 16.0
    if long_block and facts.is_urban and facts.area >= 300:
        return facts.draw(15, [("brick_block_1920s", 4), ("postwar_plaster", 4), ("slab_block", 1)]), flags, tag_bits
    if facts.area >= 140 and footprint.rectangularity < 0.82 and footprint.corner_count >= 8 \
            and facts.free_distance >= 8.0:
        return "villa", flags, tag_bits
    if levels is not None and levels >= 4 or facts.area >= 320:
        return facts.draw(16, [("postwar_plaster", 5), ("brick_block_1920s", 2), ("modern", 1)]), flags, tag_bits
    if facts.area >= 130 and facts.building_tag == "apartments":
        return "postwar_plaster", flags, tag_bits
    return _detached_or_villa(facts, flags, tag_bits, heritage)


def _detached_or_villa(facts, flags, tag_bits, heritage):
    """Single family houses: villa on a large plot with a rich plan, farmhouse on the dykes, otherwise detached."""
    footprint = facts.footprint
    area = facts.area
    deich = "deich" in facts.street_name.lower()
    rural = facts.built_ratio < 0.08
    if deich and area >= 140 and footprint.aspect >= 1.5 and rural:
        return "vierlande_farmhouse", flags, tag_bits
    if area >= 150 and facts.free_distance >= 10.0 and (footprint.rectangularity < 0.84 or footprint.corner_count >= 10):
        return "villa", flags, tag_bits
    if heritage and area >= 120 and facts.free_distance >= 6.0:
        return "villa", flags, tag_bits
    if facts.is_attached and facts.row_size == 2:
        return "semidetached", flags, tag_bits
    if area >= 100 and facts.built_ratio > 0.10 and facts.uniform(17) < 0.2 and footprint.aspect >= 1.2:
        return "postwar_plaster", flags, tag_bits
    return "detached_postwar", flags, tag_bits


# ---------------------------------------------------------------------- roof, storeys, orientation

ROOF_PRIORS = {
    # Shares per class of the roof shapes in Hamburg's LoD2 model (west half of bergedorf_core, x < 0, buildings
    # classified without optional tags; the other half is kept for the check in check_lod2.py). Hamburg has many
    # more flat roofs than the typology photos suggest, so the pre-war classes are nudged toward pitched roofs,
    # which is what the street photos show.
    "gruenderzeit_clinker": [("flat", 50), ("gabled", 28), ("hipped", 18), ("mansard", 4)],
    "brick_block_1920s": [("flat", 60), ("gabled", 28), ("hipped", 12)],
    "postwar_plaster": [("flat", 55), ("gabled", 27), ("hipped", 14), ("skillion", 4)],
    "slab_block": [("flat", 45), ("gabled", 40), ("hipped", 15)],
    "terraced": [("flat", 45), ("gabled", 40), ("hipped", 10), ("skillion", 5)],
    "semidetached": [("gabled", 60), ("flat", 22), ("hipped", 10), ("skillion", 8)],
    "detached_postwar": [("gabled", 49), ("flat", 29), ("hipped", 15), ("skillion", 5), ("pyramidal", 2)],
    "villa": [("flat", 40), ("gabled", 28), ("hipped", 25), ("skillion", 4), ("mansard", 3)],
    "modern": [("flat", 48), ("gabled", 48), ("skillion", 4)],
    "commercial_groundfloor": [("flat", 62), ("gabled", 19), ("hipped", 10), ("pyramidal", 4), ("skillion", 5)],
    "vierlande_farmhouse": [("half_hipped", 50), ("hipped", 35), ("gabled", 15)],
    "shed_garage": [("flat", 79), ("gabled", 11), ("skillion", 7), ("hipped", 3)],
    "industrial_hall": [("flat", 77), ("gabled", 19), ("skillion", 4)],
    "public": [("flat", 70), ("gabled", 20), ("hipped", 10)],
    "retail_centre": [("flat", 92), ("gabled", 8)],
    "halftimbered_town": [("gabled", 70), ("hipped", 25), ("half_hipped", 5)],
}

# class -> (storey weights, attic habitable probability)
STOREY_PRIORS = {
    # Storey counts of LoD2 per class (same half and method as ROOF_PRIORS), and the chance of a habitable attic.
    "gruenderzeit_clinker": ([(1, 21), (2, 27), (3, 52), (4, 17), (5, 7), (6, 2)], 0.85),
    "brick_block_1920s": ([(2, 13), (3, 21), (4, 7), (5, 7), (6, 5)], 0.6),
    "postwar_plaster": ([(1, 24), (2, 41), (3, 53), (4, 21), (5, 11), (6, 13)], 0.8),
    "slab_block": ([(2, 5), (3, 17), (4, 5), (5, 2), (6, 3), (7, 2)], 0.0),
    "terraced": ([(1, 25), (2, 15), (3, 3)], 0.5),
    "semidetached": ([(1, 65), (2, 34), (3, 6)], 0.9),
    "detached_postwar": ([(1, 172), (2, 137), (3, 41), (4, 8)], 0.8),
    "villa": ([(2, 6), (3, 3), (4, 1)], 0.7),
    "modern": ([(2, 3), (3, 11), (4, 5), (5, 2)], 0.0),
    "commercial_groundfloor": ([(1, 20), (2, 22), (3, 22), (4, 14), (5, 2)], 0.4),
    "vierlande_farmhouse": ([(1, 1)], 0.5),
    "shed_garage": ([(1, 1)], 0.0),
    "industrial_hall": ([(1, 1)], 0.0),
    "public": ([(1, 5), (2, 9), (3, 4), (4, 5), (5, 3)], 0.2),
    "retail_centre": ([(1, 6), (2, 3), (3, 1)], 0.0),
    "halftimbered_town": ([(2, 6), (3, 3)], 0.95),
}


# Share of flat roofs in LoD2 by footprint area (same sample as ROOF_PRIORS): points of (log2 of the area in m2, share).
FLAT_SHARE_BY_AREA = [(3.5, 0.97), (4.5, 0.90), (5.5, 0.85), (6.5, 0.70), (7.5, 0.36), (8.5, 0.48), (9.5, 0.66),
                      (10.5, 0.80), (11.5, 0.88), (12.5, 0.93)]
OVERALL_FLAT_SHARE = 0.52
AREA_EVIDENCE_WEIGHT = 0.6


def _logit(probability):
    probability = min(max(probability, 0.02), 0.98)
    return math.log(probability / (1.0 - probability))


def _flat_roof_probability(facts, class_name):
    """Chance of a flat roof: the class prior moved by how the footprint area changes it (big footprints are flat)."""
    prior = ROOF_PRIORS[class_name]
    class_share = sum(w for name, w in prior if name == "flat") / sum(w for _, w in prior)
    log_area = math.log2(max(facts.area, 8.0))
    area_share = float(np.interp(log_area, [x for x, _ in FLAT_SHARE_BY_AREA], [y for _, y in FLAT_SHARE_BY_AREA]))
    shift = AREA_EVIDENCE_WEIGHT * (_logit(area_share) - _logit(OVERALL_FLAT_SHARE))
    return 1.0 / (1.0 + math.exp(-(_logit(class_share) + shift)))


def _roof_draw_key(facts):
    """Id the roof draws are made with: the same for a whole row, so attached buildings share one roof type."""
    typer = facts.typer
    if facts.row_size >= 2 and facts.is_attached:
        return typer.items[typer.component[facts.index]][0]
    return facts.osm_id


def _roof_shape_name(facts, class_name, flags):
    """Roof shape: the OSM tag when it says something, otherwise a draw from the class prior, fitted to the plan."""
    tag = facts.tags.get("roof:shape")
    if tag in OSM_ROOF_SHAPES:
        return OSM_ROOF_SHAPES[tag], True
    key = _roof_draw_key(facts)
    flat_probability = _flat_roof_probability(facts, class_name)
    if _hash01(key, 30) < flat_probability:
        return "flat", False
    pitched = [(name, weight) for name, weight in ROOF_PRIORS[class_name] if name != "flat"]
    name = _weighted_pick(key, 31, pitched)
    footprint = facts.footprint
    if name == "gabled" and footprint.aspect < 1.2 and footprint.short_length < 16.0:
        name = "pyramidal" if footprint.aspect < 1.08 and footprint.short_length < 11.0 else "hipped"
    if footprint.short_length < 3.2 and name != "skillion":
        name = "skillion"
    return name, False


def _pitch(facts, class_name, roof_name):
    """Roof pitch in degrees: the roof:angle tag, else a draw inside the class range."""
    tagged = _float(facts.tags.get("roof:angle"))
    if tagged is not None and 0 <= tagged <= 80:
        return tagged
    if roof_name == "flat":
        return 0.0
    if roof_name == "skillion":
        return _range_pick(facts.osm_id, 32, 5, 15)
    low, high = CLASS_PITCH[class_name]
    if class_name in {"shed_garage", "industrial_hall", "slab_block", "modern"} and roof_name in {"gabled", "hipped"}:
        low, high = (18, 28) if class_name == "shed_garage" else (5, 25) if class_name == "industrial_hall" else (20, 30)
    if roof_name == "mansard":
        return _range_pick(facts.osm_id, 32, 60, 70)
    return _range_pick(facts.osm_id, 32, low, high)


def _storeys(facts, class_name, roof_name, pitch):
    """(storeys, attic levels, tag bits) from the tags when present, otherwise from the class prior."""
    storey_height, ground_height, plinth = CLASS_STOREY[class_name]
    prior_levels, attic_probability = STOREY_PRIORS[class_name]
    tag_bits = 0
    pitched = roof_name not in {"flat", "skillion"}
    attic_tag = _float(facts.tags.get("roof:levels"))
    if attic_tag is not None:
        attic = int(min(max(attic_tag, 0), 2)) if pitched else 0
        tag_bits |= TAG_ROOF_LEVELS
    else:
        attic = 1 if pitched and facts.uniform(33) < attic_probability else 0
    height_tag = _float(facts.tags.get("height"))
    if facts.levels:
        storeys = int(max(1, round(facts.levels)))
        tag_bits |= TAG_LEVELS
    elif height_tag and 2.0 <= height_tag <= 200.0:
        rise = facts.footprint.short_length / 2.0 * math.tan(math.radians(pitch)) if pitched else 0.0
        eave = height_tag - rise
        storeys = int(max(1, round((eave - plinth - ground_height) / storey_height) + 1))
        tag_bits |= TAG_HEIGHT
    else:
        storeys = _weighted_pick(facts.osm_id, 34, prior_levels)
        storeys = _fit_storeys(facts, class_name, storeys)
    return storeys, attic, tag_bits


def _fit_storeys(facts, class_name, storeys):
    """Adjusts the drawn storey count to the footprint: slabs and big blocks are taller than small houses."""
    footprint = facts.footprint
    if class_name == "slab_block":
        if footprint.aspect < 1.6 and footprint.short_length >= 16.0:
            return 10 + int(facts.uniform(35) * 6)  # point block
        if footprint.long_length >= 60.0:
            return max(storeys, 5)
    if class_name in {"detached_postwar", "semidetached", "terraced"} and facts.area < 70:
        return 1
    if class_name == "industrial_hall":
        return 1
    return storeys


def _eave_height(class_name, storeys, roof_name, tagged_height):
    storey_height, ground_height, plinth = CLASS_STOREY[class_name]
    if class_name == "industrial_hall":
        return plinth + (tagged_height or ground_height)
    return plinth + ground_height + (storeys - 1) * storey_height


def _ridge_yaw(facts, roof_name):
    """Ridge direction (degrees, 0..180): along the row for attached buildings, else along the long side."""
    footprint = facts.footprint
    if facts.is_attached or facts.row_size >= 3:
        axis = facts.t
    else:
        axis = footprint.long_axis
        orientation = facts.tags.get("roof:orientation")
        if orientation == "across":
            axis = footprint.short_axis
        elif facts.street_info is not None and facts.street_distance < 35.0 and footprint.aspect < 1.5 \
                and orientation != "along":
            axis = facts.t  # near-square plan: parallel to the street
    return _yaw_degrees(axis) % 180.0


def _complete(facts: _Facts, class_name, flags, tag_bits):
    """Fills roof, storeys, heights and orientation for the chosen class."""
    footprint = facts.footprint
    roof_name, roof_from_tag = _roof_shape_name(facts, class_name, flags)
    if roof_from_tag:
        tag_bits |= TAG_ROOF_SHAPE
    pitch = _pitch(facts, class_name, roof_name)
    storeys, attic, extra_bits = _storeys(facts, class_name, roof_name, pitch)
    tag_bits |= extra_bits
    storey_height, ground_height, plinth = CLASS_STOREY[class_name]
    if class_name == "industrial_hall":
        hall_height = 6.0 + 4.0 * facts.uniform(36)
        eave = plinth + _float(facts.tags.get("height")) if _float(facts.tags.get("height")) else plinth + hall_height
        storey_height = ground_height = eave - plinth
    else:
        eave = _eave_height(class_name, storeys, roof_name, None)
    if class_name == "shed_garage":
        eave = plinth + 2.6 + 0.5 * facts.uniform(37)
        storey_height = ground_height = eave - plinth
    if class_name == "vierlande_farmhouse" and flags & FLAG_BARN:
        eave = plinth + 3.2 + 1.2 * facts.uniform(38)
        storeys = 1
    if attic:
        flags |= FLAG_ATTIC_HABITABLE
    if facts.closed_left:
        flags |= FLAG_CLOSED_LEFT
    if facts.closed_right:
        flags |= FLAG_CLOSED_RIGHT
    if footprint.rectangularity < 0.85 or footprint.corner_count > 6:
        flags |= FLAG_COMPLEX_FOOTPRINT
    if _is_corner(facts):
        flags |= FLAG_CORNER
    if class_name == "commercial_groundfloor":
        flags |= FLAG_SHOP_GROUND_FLOOR
    front_yaw = _yaw_degrees(-facts.n)
    return BuildingType(CLASS_ID[class_name], ROOF_ID[roof_name], float(pitch), int(storeys), int(attic),
                        float(storey_height), float(ground_height), float(plinth), float(eave),
                        _ridge_yaw(facts, roof_name), front_yaw, int(flags), int(tag_bits))


def _is_corner(facts):
    """True when a second street of a clearly different direction passes within 25 m of the footprint."""
    if facts.street_info is None or not facts.is_attached or facts.street_distance > 20.0:
        return False
    typer = facts.typer
    found = typer.main_tree.query(facts.footprint.polygon, predicate="dwithin", distance=22.0) \
        if typer.main_tree is not None else []
    for index in found:
        line = typer.main_streets[int(index)].line
        along = line.project(facts.footprint.polygon.centroid)
        tangent = _unit(np.array(line.interpolate(min(along + 1.0, line.length)).coords[0])
                        - np.array(line.interpolate(max(along - 1.0, 0.0)).coords[0]))
        if abs(float(tangent @ facts.street_info[2])) < 0.6:
            return True
    return False
