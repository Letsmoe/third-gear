"""Street parking: which sides of a way cars park on (from the OSM tags or a default by street width) and where the
parked cars stand.

Tags: the new scheme (parking:left, parking:right, parking:both with the orientation in parking:<side>:orientation)
and the old one (parking:lane:<side>=parallel|diagonal|perpendicular|no). Only parallel parking on the carriageway
is placed; diagonal, perpendicular and separate parking (lay-bys, lots) is left to the area polygons. A way with any
parking tag is taken as tagged: untagged sides get no cars. Untagged residential streets and living streets get
cars on one side from PARKING_MIN_WIDTH and on both from PARKING_BOTH_WIDTH.

The same decision feeds the lane builder (lanes.py), so AI traffic drives beside the cars instead of through them.
Right of a way means right of its node order, as everywhere else.
"""
import math
from collections import namedtuple

import numpy as np
import shapely
from shapely.strtree import STRtree

from . import roads

# Untagged streets only get parked cars where two moving cars still fit beside them (two cars of 1.8 m plus 0.2 m, and
# the 1.95 m strip of each parked row), so AI traffic keeps its lanes there.
PARKING_MIN_WIDTH = 5.75
PARKING_BOTH_WIDTH = 7.7
# Tagged streets keep their cars even when only a single file passes; two rows need this much width to fit at all.
TAGGED_BOTH_MIN_WIDTH = 6.4
DEFAULT_PARKING_CLASSES = {"residential", "living_street"}
NO_PARKING_CLASSES = {"motorway", "motorway_link", "trunk", "trunk_link"}
# Width of the strip a parked car takes from the kerb, including the gap to the kerb and the mirror.
PARKING_STRIP = 1.95
CAR_WIDTH = 1.85
# Half the width of a moving car plus a hand's breadth.
LANE_CLEARANCE = 0.9

PARALLEL_VALUES = {"parallel", "lane", "street_side", "marked", "yes", "half_on_kerb", "on_kerb"}
ANGLED_VALUES = {"diagonal", "perpendicular"}
NONE_VALUES = {"no", "separate", "no_parking", "no_stopping", "no_standing", "none"}

# Models in the order of ParkedCars.cpp (the City Sample models of the AI traffic): length m, pick weight.
Model = namedtuple("Model", "name length weight")
MODELS = [
    Model("vehicle07_Car", 4.45, 1.6), Model("vehicle02_Car", 4.65, 1.0), Model("vehicle03_Car", 4.85, 0.8),
    Model("vehicle05_Car", 4.70, 1.2), Model("vehicle06_Car", 4.40, 0.5), Model("vehicle01_Van", 5.20, 0.45),
]

SCHEME_KEYS = ("parking:", "parking:lane:", "parking:condition:")


def _stable_random(way_id, salt=0):
    """A random generator that gives the same numbers for the same way on every run."""
    return np.random.default_rng((int(way_id) * 2654435761 + salt) & 0xFFFFFFFF)


def _has_parking_tags(tags):
    return any(key.startswith("parking:") and not key.startswith("parking:condition") for key in tags) \
        or "parking:lane" in tags


def _side_value(tags, side):
    """(value, orientation) tagged for one side, or (None, None)."""
    for key in (f"parking:{side}", "parking:both"):
        if key in tags:
            orientation = tags.get(f"parking:{side}:orientation") or tags.get("parking:both:orientation") or "parallel"
            return tags[key], orientation
    for key in (f"parking:lane:{side}", "parking:lane:both"):
        if key in tags:
            return tags[key], tags[key]
    return None, None


def _tagged_position(value, orientation):
    """'lane', 'street_side', 'half_on_kerb' or 'on_kerb' for a tagged side, or None when no car parks there."""
    if value is None or value in NONE_VALUES or orientation in ANGLED_VALUES or value in ANGLED_VALUES:
        return None
    if value in {"half_on_kerb", "on_kerb"}:
        return value
    if value in PARALLEL_VALUES or orientation == "parallel":
        return "street_side" if value == "street_side" else "lane"
    return None


def parking_sides(tags, way_id, width):
    """{+1 (right) or -1 (left): position} of the sides cars park on along a way."""
    highway = tags.get("highway")
    if highway in NO_PARKING_CLASSES or tags.get("junction") in {"roundabout", "circular"}:
        return {}
    if _has_parking_tags(tags):
        sides = {}
        for sign, name in ((+1, "right"), (-1, "left")):
            position = _tagged_position(*_side_value(tags, name))
            if position:
                sides[sign] = position
    elif highway in DEFAULT_PARKING_CLASSES and width >= PARKING_MIN_WIDTH:
        chosen = +1 if _stable_random(way_id, 7).random() < 0.5 else -1
        sides = {chosen: "lane"}
        if width >= PARKING_BOTH_WIDTH:
            sides[-chosen] = "lane"
    else:
        return {}
    minimum_both = TAGGED_BOTH_MIN_WIDTH if _has_parking_tags(tags) else PARKING_BOTH_WIDTH
    if len(sides) == 2 and width < minimum_both:
        keep = +1 if _stable_random(way_id, 7).random() < 0.5 else -1
        sides = {keep: sides[keep]}
    return sides


def lane_offset_with_parking(base_offset, width, travel, sides):
    """Offset of the lane of one direction of travel (positive towards its own kerb) after making room for parked cars.

    The cars standing on the lane's own side push it towards the centre line; cars on the opposite side push it
    towards its own kerb. Without parked cars the base offset is returned unchanged."""
    if not sides:
        return base_offset
    free_edge = width / 2.0 - PARKING_STRIP
    own_side_parked = travel in sides
    opposite_side_parked = -travel in sides
    if own_side_parked and opposite_side_parked:
        offset = min(base_offset, max(free_edge - LANE_CLEARANCE, LANE_CLEARANCE))
    elif own_side_parked:
        offset = max(0.0, min(base_offset, free_edge - LANE_CLEARANCE))
    else:
        # Clear of the cars on the opposite side, and one car width from the oncoming lane that is pushed to the middle.
        oncoming_offset = max(0.0, min(base_offset, free_edge - LANE_CLEARANCE))
        offset = max(base_offset, LANE_CLEARANCE - free_edge, 2.0 * LANE_CLEARANCE - oncoming_offset)
    return float(np.clip(offset, -(width / 2.0 - LANE_CLEARANCE), width / 2.0 - LANE_CLEARANCE))


class ParkedCarBuilder:
    """Places parked cars along the ways of a FurnitureBuilder's road graph, keeping the legal gaps of § 12 StVO."""

    JUNCTION_GAP = 6.5
    SIGNAL_GAP = 10.5
    CROSSING_GAP = 8.5
    BUS_STOP_GAP = 15.0
    WAY_END_GAP = 1.5
    EMPTY_STRETCH_CHANCE = 0.07
    ON_KERB_CHANCE = 0.10

    def __init__(self, data, net, furniture_builder, building_union, carriageway):
        self.data = data
        self.net = net
        self.graph = furniture_builder.graph
        self.zebras = furniture_builder.zebras
        self.buildings = building_union
        self.carriageway = carriageway
        shapely.prepare(carriageway)
        if building_union is not None:
            shapely.prepare(building_union)
        self.keepout = self._keepout_zones()
        shapely.prepare(self.keepout)
        self.cars = []

    def _keepout_zones(self):
        """Discs around junctions, crossings, signals, stop signs and bus stops that cars keep clear."""
        discs = []
        for node, entries in self.graph.at_node.items():
            if not self.graph.is_junction(node):
                continue
            half_width = max(self.graph.widths[way.id] for way, _ in entries) / 2.0
            discs.append(shapely.Point(*self.graph.node_xy[node]).buffer(half_width + self.JUNCTION_GAP))
        for point in self.data.points:
            tags = point.tags
            highway = tags.get("highway")
            radius = {"traffic_signals": self.SIGNAL_GAP, "stop": self.SIGNAL_GAP, "give_way": self.JUNCTION_GAP,
                      "crossing": self.CROSSING_GAP, "bus_stop": self.BUS_STOP_GAP, "mini_roundabout": 9.0}.get(highway)
            if radius is None and tags.get("public_transport") == "platform":
                radius = self.BUS_STOP_GAP
            if radius is not None:
                discs.append(shapely.Point(point.x, point.y).buffer(radius))
        for zebra in self.zebras:
            discs.append(shapely.Point(zebra["x"], zebra["y"]).buffer(self.CROSSING_GAP))
        return shapely.union_all(discs) if discs else shapely.Polygon()

    def build(self, region=None):
        """Places the cars of every way (only those touching the region polygon, if given); returns dicts with x, y, z,
        yaw, roll, pitch (degrees) and model."""
        for way in self.graph.ways:
            if region is not None and not region.intersects(self.graph.lines[way.id]):
                continue
            width = self.graph.widths[way.id]
            sides = parking_sides(way.tags, way.id, width)
            for sign, position in sides.items():
                self._fill_side(way, width, sign, position)
        self._drop_overlaps()
        return self.cars

    def _pick_model(self, rng):
        weights = np.array([model.weight for model in MODELS])
        return int(rng.choice(len(MODELS), p=weights / weights.sum()))

    def _fill_side(self, way, width, sign, position):
        """Walks along one side of a way and parks cars with uneven gaps, now and then leaving a stretch empty."""
        rng = _stable_random(way.id, sign + 3)
        length = self.graph.lines[way.id].length
        oneway = roads.is_oneway(way.tags)
        s = self.WAY_END_GAP + rng.uniform(0.0, 3.0)
        while True:
            if rng.random() < self.EMPTY_STRETCH_CHANCE:
                s += rng.uniform(6.0, 24.0)
            model = self._pick_model(rng)
            car_length = MODELS[model].length
            if s + car_length > length - self.WAY_END_GAP:
                return
            car = self._try_car(way, width, sign, position, oneway, s, car_length, model, rng)
            if car is None:
                s += 0.8
                continue
            self.cars.append(car)
            gap = rng.uniform(1.8, 4.0) if rng.random() < 0.12 else rng.uniform(0.35, 1.3)
            s += car_length + gap

    def _centre_at(self, way, s, offset):
        """Point at distance s along the way, moved offset to the right of its direction, and the unit direction there."""
        position, direction = self.graph.point_and_direction(way, s, +1)
        right = (-direction[1], direction[0])
        return np.array([position[0] + right[0] * offset, position[1] + right[1] * offset]), np.array(direction)

    def _try_car(self, way, width, sign, position, oneway, s, car_length, model, rng):
        """One parked car at s, or None when it would block a gap, stand in a building or leave the road."""
        kerb_tilt = 0.0
        extra = rng.uniform(0.0, 0.18)
        if position in {"half_on_kerb", "on_kerb"} or rng.random() < self.ON_KERB_CHANCE:
            extra = 0.55 if position != "on_kerb" else 0.9
            kerb_tilt = 3.0
        offset = sign * (width / 2.0 - CAR_WIDTH / 2.0 - 0.15 + extra)
        rear, _ = self._centre_at(way, s, offset)
        front, _ = self._centre_at(way, s + car_length, offset)
        along = front - rear
        if np.hypot(*along) < 1e-3:
            return None
        along /= np.hypot(*along)
        if rng.random() < 0.15:
            wobble = math.radians(rng.uniform(-4.0, 4.0))
            along = np.array([along[0] * math.cos(wobble) - along[1] * math.sin(wobble),
                              along[0] * math.sin(wobble) + along[1] * math.cos(wobble)])
        centre = (rear + front) / 2.0
        facing = 1.0 if (sign > 0 or oneway) else -1.0
        heading = along * facing
        box = self._footprint(centre, along, car_length, CAR_WIDTH)
        if box.intersects(self.keepout):
            return None
        if self.buildings is not None and box.intersects(self.buildings):
            return None
        if not shapely.covers(self.carriageway, box) and box.intersection(self.carriageway).area < 0.75 * box.area:
            return None
        return self._car_record(centre, heading, car_length, model, kerb_tilt, sign, facing)

    @staticmethod
    def _footprint(centre, along, car_length, car_width):
        side = np.array([-along[1], along[0]])
        half_l, half_w = along * car_length / 2.0, side * car_width / 2.0
        return shapely.Polygon([centre - half_l - half_w, centre + half_l - half_w, centre + half_l + half_w, centre - half_l + half_w])

    def _car_record(self, centre, heading, car_length, model, kerb_tilt, sign, facing):
        """Pose of a car whose middle is at centre: yaw, pitch from the road slope, roll when a side stands on the kerb."""
        reach = car_length * 0.31   # half the wheelbase
        z_rear = float(self.net.height.sample(*(centre - heading * reach)))
        z_front = float(self.net.height.sample(*(centre + heading * reach)))
        pitch = math.degrees(math.atan2(z_front - z_rear, 2 * reach))
        # Unreal rolls the right side down; the kerb side is the car's right when it faces with the traffic of its side.
        kerb_on_right = sign * facing > 0
        roll = -kerb_tilt if kerb_on_right else kerb_tilt
        return {"x": float(centre[0]), "y": float(centre[1]), "z": (z_rear + z_front) / 2.0 + (0.04 if kerb_tilt else 0.0),
                "yaw": math.degrees(math.atan2(heading[1], heading[0])), "roll": roll, "pitch": pitch, "model": model,
                "length": car_length, "along": (float(heading[0]), float(heading[1])), "box": self._footprint(centre, heading, car_length, CAR_WIDTH)}

    def _drop_overlaps(self):
        """Removes cars that overlap an earlier one (two ways meeting, or a wobbling car against its neighbour)."""
        tree = STRtree([car["box"] for car in self.cars])
        kept = []
        removed = set()
        for index, car in enumerate(self.cars):
            if index in removed:
                continue
            for other in tree.query(car["box"].buffer(0.25)):
                if other > index and self.cars[other]["box"].buffer(0.25).intersects(car["box"]):
                    removed.add(int(other))
            kept.append(car)
        self.cars = kept
