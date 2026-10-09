"""Street furniture from OSM: traffic signal junctions with their approaches and phases, traffic signs, street lamps
and speed limits.

Everything is derived from the OSM points and the road graph (net.ways):

* A signalised junction is a cluster of junction nodes (OSM nodes with three or more road ends, merged when closer
  than JUNCTION_MERGE_DISTANCE) that has at least one highway=traffic_signals node on an arm or on the junction node
  itself. Every arm that vehicles can enter gets an approach with a stop line. Crossing signals on a plain road
  become their own small junction, so they get a pedestrian phase.
* Approaches that run (anti)parallel share a phase; crossing approaches get different phases. Timings follow German
  practice (amber 3 s at 50 km/h, red-amber 1 s before green, all-red clearance).
* Signs: speed limit signs where a way's limit differs from its straight continuation, Halt/Vorfahrt gewähren from
  highway=stop and give_way nodes, zebra crossing signs, and whatever traffic_sign=DE:* nodes name that we have a
  graphic for.
* Lamps: OSM street_lamp nodes, plus evenly spaced lamps along lit=yes roads where OSM has none (OSM maps only a
  fraction of the real lamps).

Coordinates: world metres, x east, y south. Right of a travel direction (dx, dy) is (-dy, dx).
"""
import math
import re
from collections import defaultdict
from dataclasses import dataclass, field

import numpy as np
import shapely
from shapely.strtree import STRtree

from . import roads
from .streets import tags as osm_tags

JUNCTION_MERGE_DISTANCE = 30.0
APPROACH_SEARCH_DISTANCE = 90.0
STOP_LINE_MIN_SETBACK = 5.0
# Two stop lines on one carriageway closer than this along the travel direction are one signal group (OSM maps
# a signal per carriageway, per direction or per pole, often a few metres apart, plus the arm's own stop line).
SAME_GROUP_ALONG = 25.0
SAME_GROUP_ACROSS = 4.0
# Closer than this, stop lines on one carriageway are one group even when the way is split or the junctions differ.
SAME_GROUP_ANY_WAY_ALONG = 12.0
SAME_GROUP_DOT = 0.9
POLE_SETBACK = 0.75
KERB_CLEARANCE = 0.55
LAMP_SPACING = 36.0
# Every street in a German town is lit, but OSM tags lit=yes on only a few; these get lamps unless tagged lit=no
# or signed faster than a town street.
TOWN_STREETS = {"primary", "secondary", "tertiary", "unclassified", "residential", "living_street",
                "primary_link", "secondary_link", "tertiary_link"}
LAMP_OSM_MIN_GAP = 14.0
# A generated lamp keeps this far from every other lamp, so parallel carriageways and slip lanes share one row.
LAMP_MIN_GAP = 0.6 * LAMP_SPACING
# A pedestrian crossing light this close to a vehicle signal belongs to that junction's signals.
CROSSING_SIGNAL_ABSORB_DISTANCE = 35.0
# Half width of the cycleway strip poles keep clear of, around the OSM centre line.
CYCLEWAY_CLEAR_HALF_WIDTH = 1.2
# How far a pole may be moved off the road surface and cycleways before it is dropped.
POLE_PUSH_LIMIT = 6.0
# Signal poles may also stand across a slip lane and a few metres before the stop line, because the road surface
# has no traffic islands yet where the real ones stand.
SIGNAL_POLE_PUSH_LIMIT = 12.0
SIGNAL_POLE_SETBACKS = (0.0, 3.0, 6.0, 9.0, 12.0)
SIGN_DISTANCE_FROM_JUNCTION = 9.0
PARALLEL_DOT = 0.82

# Speed values we have a graphic for (Zeichen_274-<n>).
SPEED_SIGNS = (10, 20, 30, 40, 50, 60, 70, 80, 100, 120)
# Graphics we can show for traffic_sign=DE:<code> (code without the bracketed value).
KNOWN_SIGNS = {
    "101", "102", "103-10", "103-20", "108", "110", "120", "123", "131", "133", "136", "138", "205", "206", "208",
    "209", "211", "214", "220", "222", "237", "239", "240", "241", "250", "267", "274", "274.1", "274.2", "276", "278",
    "282", "283", "286", "301", "306", "307", "310", "311", "325.1", "325.2", "350", "357", "1010-51", "1000-10",
    "1000-20", "1001-30", "1020-30", "1022-10", "1040-30", "1052-30",
}


@dataclass
class Approach:
    """One direction of travel into a signalised junction."""
    way: object
    travel: int            # +1 when vehicles move along increasing node index
    stop_s: float          # distance along the way's line where the stop line is
    explicit: bool         # a traffic_signals node sits on the arm
    junction_key: object = None
    width: float = 6.0
    lanes: int = 1
    oneway: bool = False
    speed_kmh: float = 50.0
    rank: int = 0
    stop_xy: tuple = (0.0, 0.0)
    direction: tuple = (1.0, 0.0)
    phase: int = 0


@dataclass
class Junction:
    id: int
    x: float
    y: float
    approaches: list = field(default_factory=list)
    crossing_only: bool = False


def _float(value, default=None):
    return osm_tags.number(value, default)


def parse_speed(tags, urban):
    """Speed limit in km/h from the way's tags or the German default for its class and surroundings. 0 = no limit."""
    raw = str(tags.get("maxspeed", "")).strip().lower()
    named = {"de:urban": 50, "de:rural": 100, "de:living_street": 7, "walk": 7, "de:walk": 7, "none": 0,
             "de:motorway": 0}
    if raw in named:
        return float(named[raw])
    value = _float(raw.split(";")[0].replace("mph", "")) if raw else None
    if value is not None and 1 <= value <= 130:
        return float(value)
    zone = str(tags.get("maxspeed:type", "")) + str(tags.get("zone:maxspeed", ""))
    match = re.search(r"zone(\d+)|DE:(\d+)", zone)
    if match:
        return float(match.group(1) or match.group(2))
    highway = tags.get("highway")
    if highway == "living_street":
        return 7.0
    if highway in {"motorway", "motorway_link"}:
        return 0.0
    if urban:
        return 30.0 if highway == "service" else 50.0
    if highway in {"residential", "service", "living_street"}:
        return 50.0 if highway == "residential" else 30.0
    return 100.0


class RoadGraph:
    """Road ends, node degrees and line geometry of the drivable ways."""

    def __init__(self, ways, widths, building_union):
        self.ways = ways
        self.widths = widths
        self.lines = {w.id: shapely.LineString(w.xy) for w in ways}
        self.at_node = defaultdict(list)
        self.node_xy = {}
        for w in ways:
            for index, (node, xy) in enumerate(zip(w.node_ids, w.xy)):
                self.at_node[node].append((w, index))
                self.node_xy[node] = (float(xy[0]), float(xy[1]))
        self.degree = roads.node_degrees(ways)
        self.urban = {w.id: roads._is_urban(w, building_union) for w in ways}
        self.speed = {w.id: parse_speed(w.tags, self.urban[w.id]) for w in ways}

    def is_junction(self, node):
        return self.degree[node] >= 3 and not any(w.tags.get("junction") in {"roundabout", "circular"}
                                                  for w, _ in self.at_node[node])

    def can_travel(self, way, travel):
        """Whether vehicles may move along increasing (travel=+1) or decreasing (-1) node index."""
        tags = way.tags
        if tags.get("junction") in {"roundabout", "circular"} or tags.get("oneway") in {"yes", "1", "true"} \
                or tags.get("highway") in {"motorway", "motorway_link"}:
            return travel == +1
        if tags.get("oneway") == "-1":
            return travel == -1
        return True

    def point_and_direction(self, way, s, travel):
        """Position at distance s along the way and the unit travel direction there."""
        line = self.lines[way.id]
        s = min(max(s, 0.0), line.length)
        here = line.interpolate(s)
        ahead = line.interpolate(min(max(s + travel * 3.0, 0.0), line.length))
        behind = line.interpolate(min(max(s - travel * 3.0, 0.0), line.length))
        d = np.array([ahead.x - behind.x, ahead.y - behind.y])
        norm = np.hypot(*d)
        d = d / norm if norm > 1e-6 else np.array([1.0, 0.0])
        return (here.x, here.y), (float(d[0]), float(d[1]))

    def node_s(self, way, index):
        """Distance along the way's line of its index-th node."""
        line = self.lines[way.id]
        return float(line.project(shapely.Point(*way.xy[index])))

    def lanes_per_direction(self, way):
        total = roads.road_lanes(way.tags)
        if roads.is_oneway(way.tags):
            return max(total, 1)
        return max(total // 2, 1)


def _right(direction):
    return (-direction[1], direction[0])


class FurnitureBuilder:
    """Derives all street furniture and the signal junctions for one area."""

    def __init__(self, data, net, building_union):
        self.data = data
        self.net = net
        self.building_union = building_union
        ways = [w for w in net.ways if roads._is_ground(w)]
        self.graph = RoadGraph(ways, net.widths, building_union)
        self.ground = net.ground
        shapely.prepare(self.ground)
        self.blocked = shapely.union(self.ground, self._cycleway_strips(data))
        shapely.prepare(self.blocked)
        self.junctions = []
        self.signs = []
        self.lamps = []
        self.zebras = []  # zebra crossings on a road: dict with x, y, direction (unit, along the road), width
        self.heads = []   # poles carrying signal heads: dict with approach, x, y, yaw, height, side
        self._road_tree = STRtree([self.graph.lines[w.id] for w in ways])
        self._road_ways = ways
        self._buildings = building_union

    # ------------------------------------------------------------ geometry helpers

    @staticmethod
    def _cycleway_strips(data):
        """The strips of the separately mapped cycleways (and cycle-designated paths) that poles keep clear of."""
        lines = [shapely.LineString(way.xy) for way in data.footways
                 if way.tags.get("highway") == "cycleway" or way.tags.get("bicycle") == "designated"]
        if not lines:
            return shapely.Polygon()
        return shapely.union_all(shapely.buffer(lines, CYCLEWAY_CLEAR_HALF_WIDTH, cap_style="flat"))

    def push_off_road(self, x, y, direction_right, clearance=KERB_CLEARANCE, limit=POLE_PUSH_LIMIT):
        """Moves a point along direction_right until it is clear of the road surface and the cycleways, plus
        clearance. Returns None when there is no free spot within limit (inside a big junction, say)."""
        def blocked(moved):
            target = shapely.Point(x + direction_right[0] * moved, y + direction_right[1] * moved)
            return self.blocked.contains(target) or self.blocked.distance(target) < clearance

        moved = 0.0
        while blocked(moved):
            moved += 0.25
            if moved > limit:
                return None
        return x + direction_right[0] * moved, y + direction_right[1] * moved

    def inside_building(self, x, y):
        return self._buildings is not None and self._buildings.contains(shapely.Point(x, y))

    def nearest_way(self, x, y, max_distance=30.0):
        """(way, distance along the line, signed offset to the right of the line direction) of the nearest road."""
        point = shapely.Point(x, y)
        index = self._road_tree.nearest(point)
        if index is None:
            return None
        way = self._road_ways[int(index)]
        line = self.graph.lines[way.id]
        distance = line.distance(point)
        if distance > max_distance:
            return None
        s = float(line.project(point))
        position, direction = self.graph.point_and_direction(way, s, +1)
        right = _right(direction)
        offset = (x - position[0]) * right[0] + (y - position[1]) * right[1]
        return way, s, offset

    # ------------------------------------------------------------ signals

    def _downstream_junction(self, way, index, travel):
        """The next junction node in the travel direction within the search distance, or None."""
        line = self.graph.lines[way.id]
        start_s = self.graph.node_s(way, index)
        i = index + travel
        while 0 <= i < len(way.node_ids):
            node = way.node_ids[i]
            distance = abs(self.graph.node_s(way, i) - start_s)
            if distance > APPROACH_SEARCH_DISTANCE:
                return None
            if self.graph.is_junction(node):
                return node
            i += travel
        return None

    def _make_approach(self, way, travel, stop_s, explicit, key):
        approach = Approach(way=way, travel=travel, stop_s=stop_s, explicit=explicit, junction_key=key)
        approach.width = self.graph.widths[way.id]
        approach.lanes = self.graph.lanes_per_direction(way)
        approach.oneway = roads.is_oneway(way.tags)
        approach.speed_kmh = self.graph.speed[way.id]
        approach.rank = roads.CLASS_RANK.get(way.tags.get("highway"), 0)
        approach.stop_xy, approach.direction = self.graph.point_and_direction(way, stop_s, travel)
        return approach

    def _explicit_approaches(self, signal_nodes):
        """Approaches from highway=traffic_signals nodes: [(approach, key)] and junction nodes with signals."""
        approaches = []
        junction_signals = set()
        for point in signal_nodes:
            entries = self.graph.at_node.get(point.id)
            if not entries:
                continue
            if self.graph.is_junction(point.id):
                junction_signals.add(point.id)
                continue
            way, index = entries[0]
            direction_tag = point.tags.get("traffic_signals:direction") or point.tags.get("direction")
            travels = [+1, -1]
            if direction_tag == "forward":
                travels = [+1]
            elif direction_tag == "backward":
                travels = [-1]
            for travel in travels:
                if not self.graph.can_travel(way, travel):
                    continue
                stop_s = self.graph.node_s(way, index)
                junction = self._downstream_junction(way, index, travel)
                key = junction if junction is not None else ("crossing", point.id)
                approaches.append(self._make_approach(way, travel, stop_s, True, key))
        return approaches, junction_signals

    def _arm_approach(self, node, way, index, step):
        """Approach on the arm of a junction node: vehicles move from index+step toward index."""
        travel = -step
        if not self.graph.can_travel(way, travel):
            return None
        if way.tags.get("highway") in {"service", "track"}:
            return None
        if way.tags.get("highway", "").endswith("_link"):
            return None  # slip lanes mostly turn free and give way; OSM maps a signal on them when there is one
        crossing_width = max([self.graph.widths[w.id] for w, _ in self.graph.at_node[node] if w.id != way.id],
                             default=6.0)
        setback = max(STOP_LINE_MIN_SETBACK, crossing_width / 2 + 2.0)
        stop_s = self.graph.node_s(way, index) - travel * setback
        line = self.graph.lines[way.id]
        if stop_s < 0.5 or stop_s > line.length - 0.5:
            return None
        return self._make_approach(way, travel, stop_s, False, node)

    def _assign_phases(self, junction):
        """Groups approaches into phases: parallel and anti-parallel approaches go together, the rest alternate."""
        ordered = sorted(junction.approaches, key=lambda a: -a.rank)
        phases = []
        for approach in ordered:
            for phase_index, members in enumerate(phases):
                if all(abs(approach.direction[0] * m.direction[0] + approach.direction[1] * m.direction[1])
                       >= PARALLEL_DOT for m in members):
                    members.append(approach)
                    approach.phase = phase_index
                    break
            else:
                approach.phase = len(phases)
                phases.append([approach])
        return len(phases)

    def _signal_nodes(self):
        """Nodes that place a stop line: highway=traffic_signals, and pedestrian crossing lights that stand alone.

        A crossing light within CROSSING_SIGNAL_ABSORB_DISTANCE of a vehicle signal is the pedestrian signal of that
        junction: it shows red to walkers while the junction's own signals stop the cars, so it adds no poles."""
        vehicle = [p for p in self.data.points if p.tags.get("highway") == "traffic_signals"]
        crossings = [p for p in self.data.points
                     if p.tags.get("highway") == "crossing" and p.tags.get("crossing") == "traffic_signals"]
        if not vehicle:
            return crossings
        vehicle_tree = STRtree([shapely.Point(p.x, p.y) for p in vehicle])
        standalone = [p for p in crossings
                      if len(vehicle_tree.query(shapely.Point(p.x, p.y), predicate="dwithin",
                                                distance=CROSSING_SIGNAL_ABSORB_DISTANCE)) == 0]
        return vehicle + standalone

    def build_junctions(self):
        """Clusters signal nodes into junctions and creates their approaches."""
        explicit, junction_signals = self._explicit_approaches(self._signal_nodes())

        # Merge junction nodes of one big junction (dual carriageways, slip roads).
        parent = {}

        def find(node):
            parent.setdefault(node, node)
            while parent[node] != node:
                parent[node] = parent[parent[node]]
                node = parent[node]
            return node

        keys = {a.junction_key for a in explicit if not isinstance(a.junction_key, tuple)} | junction_signals
        keys_list = sorted(keys)
        for node in keys_list:
            find(node)
        for i, a in enumerate(keys_list):
            for b in keys_list[i + 1:]:
                ax, ay = self.graph.node_xy[a]
                bx, by = self.graph.node_xy[b]
                if math.hypot(ax - bx, ay - by) < JUNCTION_MERGE_DISTANCE:
                    parent[find(a)] = find(b)

        clusters = defaultdict(list)
        for approach in explicit:
            key = approach.junction_key
            clusters[("crossing", key[1]) if isinstance(key, tuple) else find(key)].append(approach)
        for node in junction_signals:
            clusters.setdefault(find(node), [])

        pending = []
        for cluster_key, approaches in sorted(clusters.items(), key=lambda item: str(item[0])):
            junction = Junction(id=len(self.junctions), x=0.0, y=0.0)
            crossing = isinstance(cluster_key, tuple)
            junction.crossing_only = crossing
            if not crossing:
                member_nodes = [n for n in keys_list if find(n) == cluster_key]
                covered = {(a.way.id, a.travel) for a in approaches}
                for node in member_nodes:
                    for way, index in self.graph.at_node[node]:
                        for step in (-1, +1):
                            if not 0 <= index + step < len(way.node_ids):
                                continue
                            if self.graph.is_junction(way.node_ids[index + step]) and way.node_ids[index + step] in member_nodes:
                                continue  # link between two nodes of the same junction
                            if (way.id, -step) in covered:
                                continue
                            arm = self._arm_approach(node, way, index, step)
                            if arm is not None:
                                approaches.append(arm)
                                covered.add((way.id, arm.travel))
                xs = [self.graph.node_xy[n][0] for n in member_nodes]
                ys = [self.graph.node_xy[n][1] for n in member_nodes]
                junction.x, junction.y = float(np.mean(xs)), float(np.mean(ys))
            else:
                junction.x = float(np.mean([a.stop_xy[0] for a in approaches]))
                junction.y = float(np.mean([a.stop_xy[1] for a in approaches]))
            # An explicit approach on a way that also appears twice (the same travel) keeps the one nearest the junction.
            unique = {}
            for approach in approaches:
                identity = (approach.way.id, approach.travel)
                unique.setdefault(identity, approach)
            junction.approaches = list(unique.values())
            pending.append(junction)

        self._merge_stacked_approaches(pending)
        for junction in pending:
            if len(junction.approaches) < 1:
                continue
            junction.id = len(self.junctions)
            self._assign_phases(junction)
            self.junctions.append(junction)
        return self.junctions

    @staticmethod
    def _follows_on_carriageway(first, second, max_along=SAME_GROUP_ALONG):
        """True when the second stop line lies ahead of the first on the same carriageway, within max_along."""
        dot = first.direction[0] * second.direction[0] + first.direction[1] * second.direction[1]
        if dot < SAME_GROUP_DOT:
            return False
        offset_x = second.stop_xy[0] - first.stop_xy[0]
        offset_y = second.stop_xy[1] - first.stop_xy[1]
        along = offset_x * first.direction[0] + offset_y * first.direction[1]
        across = offset_x * first.direction[1] - offset_y * first.direction[0]
        return 0.0 <= along <= max_along and abs(across) <= SAME_GROUP_ACROSS

    @staticmethod
    def _preferred_stop_line(first, second):
        """Which of two stacked approaches to keep: the one OSM maps explicitly, else the one nearer the junction."""
        if first.explicit != second.explicit:
            return first if first.explicit else second
        return second

    def _merge_stacked_approaches(self, junctions):
        """Keeps one signal and stop line where several follow each other on one carriageway.

        Within SAME_GROUP_ALONG the approaches must share a way or a junction; on different ways of different
        junctions only SAME_GROUP_ANY_WAY_ALONG counts, so junctions that merely lie close together keep their signals."""
        owners = [(junction, approach) for junction in junctions for approach in junction.approaches]
        dropped = set()
        for i, (junction_a, a) in enumerate(owners):
            for junction_b, b in owners[i + 1:]:
                if id(a) in dropped or id(b) in dropped:
                    continue
                max_along = SAME_GROUP_ALONG
                if a.way.id != b.way.id and junction_a is not junction_b:
                    max_along = SAME_GROUP_ANY_WAY_ALONG
                if self._follows_on_carriageway(a, b, max_along):
                    first, second = a, b
                elif self._follows_on_carriageway(b, a, max_along):
                    first, second = b, a
                else:
                    continue
                keep = self._preferred_stop_line(first, second)
                dropped.add(id(second if keep is first else first))
        for junction in junctions:
            junction.approaches = [a for a in junction.approaches if id(a) not in dropped]

    def signal_timing(self, junction):
        """Phase list [{green, amber, clearance}] in seconds. A junction with a single vehicle phase gets a phase without approaches."""
        phase_count = max((a.phase for a in junction.approaches), default=0) + 1
        phases = []
        for phase_index in range(phase_count):
            members = [a for a in junction.approaches if a.phase == phase_index]
            major = max((a.rank for a in members), default=0) >= roads.CLASS_RANK["tertiary"]
            speed = max((a.speed_kmh for a in members), default=50.0)
            amber = 3.0 if speed <= 50 else 4.0 if speed <= 60 else 5.0
            green = 28.0 if major else 18.0
            extent = max((a.width for a in members), default=6.0) + 8.0
            clearance = float(np.clip(extent / max(speed / 3.6, 8.0), 2.0, 4.5))
            phases.append({"green": green, "amber": amber, "clearance": round(clearance, 1), "pedestrian": False})
        if junction.crossing_only:
            phases[0]["green"] = 35.0
        if len(phases) == 1:
            # Nothing crosses the only phase in the data (side streets or pedestrians are not mapped as approaches):
            # give them a phase of their own so the junction is not green forever.
            phases.append({"green": 10.0, "amber": 0.0, "clearance": 2.0, "pedestrian": True})
        return phases

    def head_poles(self, approach, junction_id, approach_id):
        """Pole positions carrying the signal heads of an approach: right of the lane, and left too on wide one-way
        roads when there is room beside the stop line."""
        poles = []
        right = _right(approach.direction)
        placed = self._signal_pole_spot(approach, right, SIGNAL_POLE_SETBACKS, SIGNAL_POLE_PUSH_LIMIT)
        if placed is not None:
            poles.append({"x": placed[0], "y": placed[1], "side": "right"})
        if approach.oneway and approach.lanes >= 2:
            placed = self._signal_pole_spot(approach, (-right[0], -right[1]), (0.0,), POLE_PUSH_LIMIT)
            if placed is not None:
                poles.append({"x": placed[0], "y": placed[1], "side": "left"})
        yaw = math.degrees(math.atan2(approach.direction[1], approach.direction[0]))
        return [{**p, "yaw": yaw, "approach": approach_id, "junction": junction_id} for p in poles
                if not self.inside_building(p["x"], p["y"])]

    def _signal_pole_spot(self, approach, outward, setbacks, limit):
        """The first free spot beside the carriageway edge in the outward direction, trying the stop line first and
        then points further back against the travel direction; None when all are on the road or a cycleway."""
        half_width = approach.width / 2
        for setback in setbacks:
            x = approach.stop_xy[0] - approach.direction[0] * setback + outward[0] * half_width
            y = approach.stop_xy[1] - approach.direction[1] * setback + outward[1] * half_width
            placed = self.push_off_road(x, y, outward, limit=limit)
            if placed is not None:
                return placed
        return None

    def stop_line(self, approach):
        """End points of the stop line: from the lane centre line (two-way) or left edge (one-way) to the right edge."""
        right = _right(approach.direction)
        x, y = approach.stop_xy
        half = approach.width / 2
        left_extent = half if approach.oneway else 0.0
        return [x - right[0] * left_extent, y - right[1] * left_extent, x + right[0] * half, y + right[1] * half]

    # ------------------------------------------------------------ signs

    def add_sign(self, x, y, yaw, names, height_m=None):
        """A sign pole at (x, y) facing against yaw (the travel direction); names are graphics stacked top to bottom."""
        if self.inside_building(x, y):
            return
        self.signs.append({"x": float(x), "y": float(y), "yaw": float(yaw), "names": list(names)})

    def sign_beside_road(self, way, s, travel, names, extra_offset=0.0):
        """Places a sign right of the carriageway at distance s of the way's line, facing traffic moving in travel."""
        position, direction = self.graph.point_and_direction(way, s, travel)
        right = _right(direction)
        half = self.graph.widths[way.id] / 2 + extra_offset
        placed = self.push_off_road(position[0] + right[0] * half, position[1] + right[1] * half, right)
        if placed is None:
            return
        self.add_sign(placed[0], placed[1], math.degrees(math.atan2(direction[1], direction[0])), names)

    @staticmethod
    def speed_sign_name(speed):
        nearest = min(SPEED_SIGNS, key=lambda value: abs(value - speed))
        return f"Zeichen_274-{nearest}"

    def limit_sign_name(self, limit, zone):
        """Graphic for a limit: the zone sign for Tempo 20 and 30 zones, else the plain speed limit sign."""
        if zone and limit == 30:
            return "Zeichen_274.1"
        if zone and limit == 20:
            return "Zeichen_274.1-20"
        return self.speed_sign_name(limit)

    def build_speed_signs(self):
        """A speed limit sign wherever a road is entered whose limit differs from the straight continuation."""
        graph = self.graph
        for way in graph.ways:
            if way.tags.get("highway") in {"service", "track"} and "maxspeed" not in way.tags:
                continue
            line = graph.lines[way.id]
            if line.length < 25.0:
                continue
            limit = graph.speed[way.id]
            zone = "zone" in str(way.tags.get("maxspeed:type", "")) or "zone" in str(way.tags.get("zone:maxspeed", ""))
            for end_index, travel in ((0, +1), (len(way.node_ids) - 1, -1)):
                if not graph.can_travel(way, travel):
                    continue
                node = way.node_ids[end_index]
                others = [(w, i) for w, i in graph.at_node[node] if w.id != way.id]
                if not others:
                    continue
                start_s = 0.0 if travel == +1 else line.length
                _, direction = graph.point_and_direction(way, start_s, travel)
                best = None
                for other, other_index in others:
                    outward = -1 if other_index == len(other.node_ids) - 1 else +1
                    inward_dir = graph.point_and_direction(other, graph.node_s(other, other_index), -outward)[1]
                    straightness = inward_dir[0] * direction[0] + inward_dir[1] * direction[1]
                    if best is None or straightness > best[0]:
                        best = (straightness, other)
                if best is None or abs(graph.speed[best[1].id] - limit) < 0.5:
                    continue
                if limit <= 0:
                    continue
                s = start_s + travel * SIGN_DISTANCE_FROM_JUNCTION
                self.sign_beside_road(way, s, travel, [self.limit_sign_name(limit, zone)])

    def build_point_signs(self):
        """Signs from highway=stop, give_way, zebra crossings and traffic_sign=* nodes."""
        for point in self.data.points:
            tags = point.tags
            names = self._names_from_tags(tags)
            if tags.get("highway") == "stop":
                names = names or ["Zeichen_206"]
            elif tags.get("highway") == "give_way":
                names = names or ["Zeichen_205"]
            elif tags.get("highway") == "crossing" and tags.get("crossing") in {"zebra", "marked"} \
                    or tags.get("crossing_ref") == "zebra":
                names = ["Zeichen_350"]
                self._add_zebra(point)
            if not names:
                continue
            self._place_point_sign(point, names)

    def _add_zebra(self, point):
        """Records a zebra crossing's centre, the road direction there and the carriageway width, for the stripes.
        Nodes on a road junction are skipped; there is no single crossing direction."""
        entries = self.graph.at_node.get(point.id)
        if entries:
            if self.graph.is_junction(point.id):
                return
            way, index = entries[0]
            s = self.graph.node_s(way, index)
        else:
            found = self.nearest_way(point.x, point.y, max_distance=6.0)
            if found is None:
                return
            way, s, _ = found
        position, direction = self.graph.point_and_direction(way, s, +1)
        self.zebras.append({"x": float(position[0]), "y": float(position[1]), "direction": (float(direction[0]), float(direction[1])),
                            "width": float(self.graph.widths[way.id])})

    def _names_from_tags(self, tags):
        """Graphic names for traffic_sign=DE:250,1020-30[...] values we can show."""
        value = tags.get("traffic_sign")
        if not value:
            return []
        names = []
        for part in value.split(";")[0].split(","):
            part = part.replace("DE:", "").strip()
            match = re.match(r"([0-9.]+(?:-[0-9]+)?)(?:\[(\d+)\])?", part)
            if not match:
                continue
            code, number = match.group(1), match.group(2)
            if code in {"274", "278"} and number:
                names.append(f"Zeichen_{code}-{min(SPEED_SIGNS, key=lambda v: abs(v - int(number)))}")
            elif code in KNOWN_SIGNS:
                prefix = "Zusatzzeichen" if code.startswith("10") else "Zeichen"
                names.append(f"{prefix}_{code}")
        return [n for n in names if n]

    def _place_point_sign(self, point, names):
        """Places signs for one OSM node: on its arm when it lies on a road end, else beside the nearest road."""
        direction_tag = point.tags.get("direction") or point.tags.get("traffic_sign:direction")
        entries = self.graph.at_node.get(point.id)
        if entries:
            way, index = entries[0]
            if self.graph.is_junction(point.id):
                self._place_junction_signs(point, names)
                return
            travels = [+1, -1]
            if direction_tag == "forward":
                travels = [+1]
            elif direction_tag == "backward":
                travels = [-1]
            elif point.tags.get("highway") == "crossing":
                travels = [t for t in (+1, -1) if self.graph.can_travel(way, t)]
            s = self.graph.node_s(way, index)
            for travel in travels:
                if self.graph.can_travel(way, travel):
                    self.sign_beside_road(way, s - travel * 3.0, travel, names)
            return
        found = self.nearest_way(point.x, point.y)
        if found is None:
            return
        way, s, offset = found
        travel = +1 if offset > 0 else -1  # the node is right of the line direction, so it serves that direction
        if not self.graph.can_travel(way, travel):
            return
        position, direction = self.graph.point_and_direction(way, s, travel)
        self.add_sign(point.x, point.y, math.degrees(math.atan2(direction[1], direction[0])), names)

    def _place_junction_signs(self, point, names):
        """A stop or give-way node at a junction: signs on the arms except the straightest through pair."""
        arms = []
        for way, index in self.graph.at_node[point.id]:
            for step in (-1, +1):
                if 0 <= index + step < len(way.node_ids) and self.graph.can_travel(way, -step):
                    arms.append((way, index, step))
        if len(arms) < 2:
            return
        directions = []
        for way, index, step in arms:
            _, d = self.graph.point_and_direction(way, self.graph.node_s(way, index), -step)
            directions.append(d)
        through = set()
        if len(arms) >= 3:
            best = None
            for i in range(len(arms)):
                for j in range(i + 1, len(arms)):
                    straightness = -(directions[i][0] * directions[j][0] + directions[i][1] * directions[j][1])
                    if best is None or straightness > best[0]:
                        best = (straightness, i, j)
            through = {best[1], best[2]}
        for i, (way, index, step) in enumerate(arms):
            if i in through:
                continue
            setback = SIGN_DISTANCE_FROM_JUNCTION * 0.6
            self.sign_beside_road(way, self.graph.node_s(way, index) + step * setback, -step, names)

    # ------------------------------------------------------------ lamps

    def build_lamps(self):
        """OSM lamps facing the road, then evenly spaced lamps along lit roads where OSM has none."""
        osm_lamps = []
        for point in self.data.points:
            if point.tags.get("highway") != "street_lamp":
                continue
            found = self.nearest_way(point.x, point.y, max_distance=40.0)
            yaw = 0.0
            if found is not None:
                way, s, offset = found
                position, direction = self.graph.point_and_direction(way, s, +1)
                right = _right(direction)
                toward = (-right[0], -right[1]) if offset > 0 else right
                yaw = math.degrees(math.atan2(toward[1], toward[0]))
            if not self.inside_building(point.x, point.y):
                osm_lamps.append((point.x, point.y, yaw))
        self.lamps = [{"x": x, "y": y, "yaw": yaw, "source": "osm"} for x, y, yaw in osm_lamps]
        osm_points = shapely.MultiPoint([(x, y) for x, y, _ in osm_lamps]) if osm_lamps else None
        for way in self.graph.ways:
            if self._is_lit(way):
                self._add_lit_way_lamps(way, osm_points)

    @staticmethod
    def _is_lit(way):
        """Whether a way has street lighting: tagged lit=yes, or an untagged town street."""
        lit = way.tags.get("lit")
        highway = way.tags.get("highway")
        if highway in {"motorway", "motorway_link"} or lit == "no":
            return False
        if lit == "yes":
            return True
        speed = parse_speed(way.tags, urban=True)
        return highway in TOWN_STREETS and (speed is None or 0 < speed <= 50)

    def _add_lit_way_lamps(self, way, osm_points):
        """Evenly spaced lamps along a lit way: right side only on narrow roads, alternating sides on wide ones."""
        line = self.graph.lines[way.id]
        if line.length < 12.0:
            return
        width = self.graph.widths[way.id]
        both_sides = width >= 7.5
        count = max(int(line.length // LAMP_SPACING), 1)
        for k in range(count):
            s = (k + 0.5) * line.length / count
            position, direction = self.graph.point_and_direction(way, s, +1)
            right = _right(direction)
            side = -1 if (both_sides and k % 2 == 1) else 1
            normal = (right[0] * side, right[1] * side)
            placed = self.push_off_road(position[0] + normal[0] * width / 2, position[1] + normal[1] * width / 2,
                                        normal, clearance=0.6)
            if placed is None:
                continue
            x, y = placed
            if self.inside_building(x, y):
                continue
            if osm_points is not None and osm_points.distance(shapely.Point(x, y)) < LAMP_OSM_MIN_GAP:
                continue
            if any(math.hypot(lamp["x"] - x, lamp["y"] - y) < LAMP_MIN_GAP for lamp in self.lamps):
                continue
            yaw = math.degrees(math.atan2(-normal[1], -normal[0]))
            self.lamps.append({"x": float(x), "y": float(y), "yaw": yaw, "source": "lit"})

    # ------------------------------------------------------------ output

    def speed_ways(self):
        """Every drivable way with its limit, for the runtime speed limit query."""
        out = []
        for way in self.graph.ways:
            out.append({"id": way.id, "limit": self.graph.speed[way.id], "width": round(self.graph.widths[way.id], 1),
                        "oneway": roads.is_oneway(way.tags),
                        "points": np.round(way.xy, 2).tolist()})
        return out

    def build(self):
        """Builds everything and returns the traffic network dictionary written to traffic.json."""
        self.build_junctions()
        self.build_speed_signs()
        self.build_point_signs()
        self.build_lamps()
        junction_records = []
        approach_id = 0
        for junction in self.junctions:
            phases = self.signal_timing(junction)
            approach_records = []
            for approach in junction.approaches:
                poles = self.head_poles(approach, junction.id, approach_id)
                self.heads.extend(poles)
                approach_records.append({
                    "id": approach_id, "phase": approach.phase, "way": approach.way.id,
                    "stop_line": [round(v, 2) for v in self.stop_line(approach)],
                    "direction": [round(approach.direction[0], 4), round(approach.direction[1], 4)],
                    "lanes": approach.lanes, "speed": approach.speed_kmh,
                })
                approach_id += 1
            junction_records.append({
                "id": junction.id, "x": round(junction.x, 1), "y": round(junction.y, 1),
                "crossing_only": junction.crossing_only, "phases": phases, "approaches": approach_records,
            })
        return {"junctions": junction_records, "speed_ways": self.speed_ways()}
