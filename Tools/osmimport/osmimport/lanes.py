"""Lane graph for AI traffic, derived from the OSM road graph and the signal junctions of furniture.py.

Every drivable way is cut into segments between graph nodes (way ends and nodes shared with other ways). Each segment
gets one lane per allowed direction of travel: the right-hand lane, a centre line offset from the way's centre line.
At every node, connection lanes (Bezier curves) link each incoming lane to each outgoing lane, so a car's route is a
chain road lane, connection, road lane, connection...

Rules are attached to the graph rather than left to the runtime:

* the end of a road lane at a junction has a control: signal (stop lines come from the signal approaches), stop or
  give way (from the signs furniture.py placed), priority, or equal (right before left);
* every connection lists the connections it crosses or merges with inside the same junction, with the stretch of both
  paths where they meet, and says whether this connection has to give way to the other one;
* lane points carry a curvature speed limit, the road's legal speed limit travels with the lane.

Coordinates are world metres (x east, y south), right of a direction (dx, dy) is (-dy, dx), like everywhere else.
"""
import math
from collections import defaultdict
from dataclasses import dataclass, field

import numpy as np
import shapely
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import connected_components

from . import furniture, parking, roads
from .streets.cross_section import Travel

LANE_SPACING = 1.5
CONNECTION_SPACING = 0.75
MIN_LANE_OFFSET = 1.2
JUNCTION_TRIM_BASE = 2.5
# Lanes beside parked cars keep the normal offset this far from a segment end and reach the shifted one PARKING_TAPER_LENGTH later.
PARKING_TAPER_START = 6.5
PARKING_TAPER_LENGTH = 2.5
STOP_TRIM_EXTRA = 0.3
MAX_STOP_BEFORE_LANE = 8.0
CONFLICT_DISTANCE = 1.9
CONFLICT_MARGIN = 1.2
LATERAL_ACCELERATION = 2.0
STRAIGHT_ANGLE_DEGREES = 28.0
MAX_TURN_DEGREES = 155.0
SIGN_SNAP_DISTANCE = 9.0
EDGE_INSET = 2.0

# Priority of an arm; a higher level has right of way over a lower one.
LEVEL_SIGNAL, LEVEL_PRIORITY, LEVEL_EQUAL, LEVEL_YIELD = 4, 3, 2, 1
CONTROL_NAMES = {LEVEL_SIGNAL: "signal", LEVEL_PRIORITY: "priority", LEVEL_EQUAL: "equal", LEVEL_YIELD: "yield"}

# Importance of a road class for who has right of way and where cars like to go.
CLASS_TIER = {
    "service": 0, "living_street": 0, "residential": 1, "unclassified": 1, "road": 1, "tertiary_link": 2, "tertiary": 2,
    "secondary_link": 3, "secondary": 3, "primary_link": 4, "primary": 4, "trunk_link": 4, "trunk": 4,
}
ROUTE_WEIGHT_BY_TIER = {0: 0.25, 1: 1.0, 2: 1.6, 3: 2.2, 4: 2.5}
PRIVATE_SERVICES = {"driveway", "parking_aisle", "drive-through", "emergency_access", "slipway"}


@dataclass
class Lane:
    """One road lane (a way segment in one direction) or one connection through a junction."""
    id: int
    kind: str                       # "road", "connection" or "uturn"
    way: int
    xy: np.ndarray                  # (n, 2)
    z: np.ndarray = None
    limit_kmh: float = 50.0
    curve_kmh: np.ndarray = None    # per point
    roundabout: bool = False
    tier: int = 1
    next: list = field(default_factory=list)
    # road lanes
    start_node: object = None
    end_node: object = None
    control: str = "none"
    stops: list = field(default_factory=list)   # [(s, approach id)]
    # connections
    from_lane: int = -1
    to_lane: int = -1
    node: object = None
    turn: int = 0                   # -1 left, 0 straight, +1 right
    level: int = LEVEL_EQUAL
    phase: int = -1
    conflicts: list = field(default_factory=list)
    good: bool = False

    def arclength(self):
        """Cumulative distance along the lane at every point."""
        steps = np.hypot(*np.diff(self.xy, axis=0).T)
        return np.concatenate([[0.0], np.cumsum(steps)])

    def length(self):
        """Length of the lane in metres."""
        return float(self.arclength()[-1])


@dataclass
class Segment:
    """A piece of a way between two graph nodes, possibly clipped to the area."""
    id: int
    way: object
    xy: np.ndarray
    start_node: object
    end_node: object
    bridge: bool = False


def right_of(direction):
    """The unit vector to the right of a direction in the x east, y south frame."""
    return np.array([-direction[1], direction[0]])


def unit(vector):
    """The vector scaled to length one; (1, 0) for a zero vector."""
    norm = float(np.hypot(*vector))
    return vector / norm if norm > 1e-9 else np.array([1.0, 0.0])


def offset_polyline(xy, distance):
    """Moves a polyline sideways by distance to the right of its direction, with mitred corners.
    distance is one number, or one number per point."""
    if np.ndim(distance) == 0 and abs(distance) < 1e-6:
        return xy.copy()
    steps = np.diff(xy, axis=0)
    lengths = np.hypot(steps[:, 0], steps[:, 1])
    keep = lengths > 1e-9
    steps, lengths = steps[keep], lengths[keep]
    directions = steps / lengths[:, None]
    normals = np.stack([-directions[:, 1], directions[:, 0]], axis=1)
    count = len(xy)
    vertex_normals = np.zeros((count, 2))
    vertex_normals[0] = normals[0]
    vertex_normals[-1] = normals[-1]
    scale = np.ones(count)
    for i in range(1, count - 1):
        before = normals[min(i - 1, len(normals) - 1)]
        after = normals[min(i, len(normals) - 1)]
        mean = unit(before + after)
        vertex_normals[i] = mean
        scale[i] = 1.0 / max(float(np.dot(mean, before)), 0.5)
    return xy + vertex_normals * (distance * scale)[:, None]



def resample(xy, spacing):
    """Equally spaced points along a polyline, keeping its end points."""
    steps = np.hypot(*np.diff(xy, axis=0).T)
    along = np.concatenate([[0.0], np.cumsum(steps)])
    total = along[-1]
    if total < 1e-6:
        return xy[:1].copy()
    count = max(int(round(total / spacing)), 1) + 1
    targets = np.linspace(0.0, total, count)
    return np.stack([np.interp(targets, along, xy[:, 0]), np.interp(targets, along, xy[:, 1])], axis=1)


def smooth(xy, passes=3):
    """Rounds corners: repeated 1-2-1 averaging with the end points pinned."""
    out = xy.copy()
    if len(out) < 4:
        return out
    for _ in range(passes):
        out[1:-1] = 0.25 * out[:-2] + 0.5 * out[1:-1] + 0.25 * out[2:]
    return out


def cut_polyline(xy, start_s, end_s):
    """The part of a polyline between two arc lengths."""
    steps = np.hypot(*np.diff(xy, axis=0).T)
    along = np.concatenate([[0.0], np.cumsum(steps)])
    total = along[-1]
    start_s = min(max(start_s, 0.0), total)
    end_s = min(max(end_s, start_s), total)
    inside = (along > start_s + 1e-6) & (along < end_s - 1e-6)
    points = [np.array([np.interp(start_s, along, xy[:, 0]), np.interp(start_s, along, xy[:, 1])])]
    points.extend(xy[inside])
    points.append(np.array([np.interp(end_s, along, xy[:, 0]), np.interp(end_s, along, xy[:, 1])]))
    return np.array(points)


def project_on_polyline(xy, point):
    """(arc length, distance) of the closest point of a polyline to a point."""
    best = (0.0, 1e18)
    travelled = 0.0
    for a, b in zip(xy[:-1], xy[1:]):
        edge = b - a
        length = float(np.hypot(*edge))
        if length < 1e-9:
            continue
        t = min(max(float(np.dot(point - a, edge)) / (length * length), 0.0), 1.0)
        distance = float(np.hypot(*(a + edge * t - point)))
        if distance < best[1]:
            best = (travelled + t * length, distance)
        travelled += length
    return best


def curvature_speeds(xy, limit_kmh, cap_kmh=None):
    """Per point speed (km/h) that keeps the lateral acceleration under LATERAL_ACCELERATION, from the turning angle."""
    count = len(xy)
    speeds = np.full(count, limit_kmh, dtype=np.float64)
    if count < 3:
        return speeds
    window = 2
    for i in range(count):
        a = xy[max(i - window, 0)]
        b = xy[i]
        c = xy[min(i + window, count - 1)]
        ab, bc = b - a, c - b
        length_ab, length_bc = np.hypot(*ab), np.hypot(*bc)
        if length_ab < 1e-6 or length_bc < 1e-6:
            continue
        cos_angle = float(np.clip(np.dot(ab, bc) / (length_ab * length_bc), -1.0, 1.0))
        angle = math.acos(cos_angle)
        chord = np.hypot(*(c - a))
        if angle < 1e-3 or chord < 1e-6:
            continue
        radius = chord / (2.0 * math.sin(min(angle, math.pi / 2 - 1e-3)) + 1e-9) if angle < math.pi / 2 else chord / 2.0
        speeds[i] = min(speeds[i], math.sqrt(LATERAL_ACCELERATION * max(radius, 1.0)) * 3.6)
    if cap_kmh is not None:
        speeds = np.minimum(speeds, cap_kmh)
    return speeds


def bezier(p0, p1, p2, p3, spacing):
    """Cubic Bezier curve sampled about every spacing metres."""
    coarse = np.linspace(0.0, 1.0, 40)[:, None]
    points = ((1 - coarse) ** 3 * p0 + 3 * (1 - coarse) ** 2 * coarse * p1 + 3 * (1 - coarse) * coarse ** 2 * p2
              + coarse ** 3 * p3)
    return resample(points, spacing)


class LaneBuilder:
    """Builds the lane graph for one area from the furniture builder's road graph, approaches and signs."""

    def __init__(self, area, net, builder):
        """Prepares an empty graph for an area; build() fills it."""
        self.area = area
        self.net = net
        self.builder = builder
        self.graph = builder.graph
        self.lanes = []
        self.segments = []
        self.node_xy = {}
        self.arms = defaultdict(list)   # node -> [(lane id of incoming road lane, lane id of outgoing road lane)]
        self.synthetic_nodes = 0

    # ------------------------------------------------------------ road segments

    def _is_traffic_way(self, way):
        """Whether cars drive on the way: not driveways, parking aisles and private or closed roads."""
        tags = way.tags
        if tags.get("highway") not in CLASS_TIER:
            return False
        if tags.get("highway") == "service" and tags.get("service") in PRIVATE_SERVICES:
            return False
        if tags.get("access") in {"private", "no"} and tags.get("motor_vehicle") not in {"yes", "destination"}:
            return False
        if tags.get("motor_vehicle") == "no" or tags.get("motorcar") == "no":
            return False
        return True

    def _graph_nodes(self, ways):
        """Nodes where a lane graph node is needed: way ends and nodes that more than one way touches."""
        occurrences = defaultdict(int)
        ends = set()
        for way in ways:
            ends.add(way.node_ids[0])
            ends.add(way.node_ids[-1])
            for node in way.node_ids:
                occurrences[node] += 1
        return {node for node in occurrences if node in ends or occurrences[node] >= 2}

    def _new_node(self, xy):
        """A node for a point where a way was cut at the area edge."""
        self.synthetic_nodes += 1
        node = -self.synthetic_nodes
        self.node_xy[node] = (float(xy[0]), float(xy[1]))
        return node

    def build_segments(self):
        """Cuts the traffic ways at graph nodes and clips them to the area."""
        ways = [w for w in self.net.ways if self._is_traffic_way(w)]
        graph_nodes = self._graph_nodes(ways)
        area = self.area
        box = shapely.box(area.x_min + EDGE_INSET, area.y_min + EDGE_INSET, area.x_max - EDGE_INSET, area.y_max - EDGE_INSET)
        for way in ways:
            bridge = not roads._is_ground(way)
            cut_indices = [i for i, node in enumerate(way.node_ids) if node in graph_nodes]
            for a, b in zip(cut_indices[:-1], cut_indices[1:]):
                xy = np.asarray(way.xy[a:b + 1], dtype=np.float64)
                if len(xy) < 2 or float(np.hypot(*np.diff(xy, axis=0).T).sum()) < 0.5:
                    continue
                for node, point in ((way.node_ids[a], xy[0]), (way.node_ids[b], xy[-1])):
                    self.node_xy[node] = (float(point[0]), float(point[1]))
                self._add_clipped(way, xy, way.node_ids[a], way.node_ids[b], box, bridge)

    def _add_clipped(self, way, xy, start_node, end_node, box, bridge):
        """Adds a segment, cut to the area when it leaves it; the new ends become dead ends."""
        line = shapely.LineString(xy)
        if box.contains(line):
            self.segments.append(Segment(len(self.segments), way, xy, start_node, end_node, bridge))
            return
        clipped = line.intersection(box)
        pieces = list(clipped.geoms) if hasattr(clipped, "geoms") else [clipped]
        for piece in pieces:
            if not isinstance(piece, shapely.LineString) or piece.length < 3.0:
                continue
            coords = np.asarray(piece.coords)
            first_node = start_node if np.hypot(*(coords[0] - xy[0])) < 1e-6 else self._new_node(coords[0])
            last_node = end_node if np.hypot(*(coords[-1] - xy[-1])) < 1e-6 else self._new_node(coords[-1])
            self.segments.append(Segment(len(self.segments), way, coords, first_node, last_node, bridge))

    # ------------------------------------------------------------ lanes

    def _directions(self, way):
        """The directions of travel (+1 along the node order, -1 against it) the way allows."""
        return [travel for travel in (+1, -1) if self.graph.can_travel(way, travel)]

    def _lane_offset(self, way, travel=+1):
        """Distance of the right-hand lane's centre line from the way's centre line, making room for parked cars."""
        width = self.net.widths[way.id]
        base = self._base_lane_offset(way, travel)
        return parking.lane_offset_with_parking(base, width, travel, parking.parking_sides(way.tags, way.id, width))

    def _tapered_offsets(self, xy, base, shifted):
        """Per point offsets of a lane that keeps its usual offset near both ends and moves to the parked-car offset
        in between, so the junction connections (and their conflicts) are the same as without parked cars."""
        steps = np.hypot(*np.diff(xy, axis=0).T)
        along = np.concatenate([[0.0], np.cumsum(steps)])
        from_end = np.minimum(along, along[-1] - along)
        blend = np.clip((from_end - PARKING_TAPER_START) / PARKING_TAPER_LENGTH, 0.0, 1.0)
        return base + (shifted - base) * blend

    def _base_lane_offset(self, way, travel):
        """The lane offset on a street without parked cars: the middle of the kerb-side travel lane of the cross-section
        (cycle and bus lanes beside it stay free), kept clear of the oncoming traffic on two-way roads."""
        section = self.net.sections[way.id]
        direction = Travel.FORWARD if travel > 0 else Travel.BACKWARD
        offset = section.kerb_lane_centre(direction)
        if roads.is_oneway(way.tags):
            return max(offset, 0.0)
        width = section.width()
        return float(np.clip(offset, MIN_LANE_OFFSET, max(width / 2.0 - 1.0, MIN_LANE_OFFSET)))

    def _raw_lane_path(self, segment, travel):
        """The lane centre line of a segment in one direction, before junction trimming."""
        xy = segment.xy if travel > 0 else segment.xy[::-1]
        base = self._base_lane_offset(segment.way, travel)
        shifted = self._lane_offset(segment.way, travel)
        if abs(shifted - base) < 1e-6:
            return offset_polyline(xy, base)
        xy = resample(xy, 1.0)
        return offset_polyline(xy, self._tapered_offsets(xy, base, shifted))

    def build_road_lanes(self):
        """A lane per segment and direction, trimmed at junctions, smoothed and resampled."""
        self.raw = {}
        for segment in self.segments:
            for travel in self._directions(segment.way):
                self.raw[(segment.id, travel)] = self._raw_lane_path(segment, travel)
        arm_count = defaultdict(int)
        for segment in self.segments:
            directions = self._directions(segment.way)
            if not directions:
                continue
            arm_count[segment.start_node] += 1
            arm_count[segment.end_node] += 1
        self.arm_count = arm_count
        self.node_width = defaultdict(float)
        for segment in self.segments:
            width = self.net.widths[segment.way.id]
            for node in (segment.start_node, segment.end_node):
                self.node_width[node] = max(self.node_width[node], width)
        self._link_signal_stops()
        self.lane_of = {}
        for segment in self.segments:
            for travel in self._directions(segment.way):
                self._make_road_lane(segment, travel)

    def _link_signal_stops(self):
        """Finds, for every signal approach, the lane it belongs to and where along the raw lane its stop line is."""
        self.signal_stops = defaultdict(list)    # (segment id, travel) -> [(raw arc length, approach id)]
        self.approach_phase = {}
        approach_id = 0
        for junction in self.builder.junctions:
            for approach in junction.approaches:
                self.approach_phase[approach_id] = (junction.id, approach.phase)
                best = None
                stop = np.array(approach.stop_xy)
                for segment in self.segments:
                    if segment.way.id != approach.way.id:
                        continue
                    raw = self.raw.get((segment.id, approach.travel))
                    if raw is None:
                        continue
                    s, distance = project_on_polyline(raw, stop)
                    # The stop point is on the way's centre line and the lane is offset from it: compare against that offset.
                    mismatch = abs(distance - self._base_lane_offset(segment.way, approach.travel))
                    if mismatch > 4.0:
                        continue
                    if best is None or mismatch < best[0]:
                        best = (mismatch, segment.id, s)
                if best is not None:
                    self.signal_stops[(best[1], approach.travel)].append((best[2], approach_id))
                approach_id += 1

    def _trim_for(self, segment, travel, raw_length):
        """(trim at the start, trim at the end) of a lane: the room a junction takes at either end."""
        start_node = segment.start_node if travel > 0 else segment.end_node
        end_node = segment.end_node if travel > 0 else segment.start_node

        def junction_trim(node):
            """Room a junction takes from a lane at the given node."""
            if self.arm_count[node] < 3:
                return 0.0
            return self.node_width[node] * 0.5 + JUNCTION_TRIM_BASE

        trim_start, trim_end = junction_trim(start_node), junction_trim(end_node)
        for s, _ in self.signal_stops.get((segment.id, travel), []):
            behind = raw_length - s
            if 0.0 <= behind < 40.0 and self.arm_count[end_node] >= 3:
                trim_end = max(trim_end, behind + STOP_TRIM_EXTRA)
        limit = raw_length * 0.45
        trim_start, trim_end = min(trim_start, limit), min(trim_end, limit)
        return trim_start, trim_end

    def _lane_limit(self, way):
        """Legal speed limit of the way in km/h; roads without one count as 100."""
        limit = self.graph.speed.get(way.id)
        if limit is None:   # bridges are not in the furniture graph
            limit = furniture.parse_speed(way.tags, True)
        return limit if limit > 0 else 100.0

    def _make_road_lane(self, segment, travel):
        """Creates the trimmed, smoothed lane of a segment and direction, with its stop lines."""
        raw = self.raw[(segment.id, travel)]
        raw_length = float(np.hypot(*np.diff(raw, axis=0).T).sum())
        trim_start, trim_end = self._trim_for(segment, travel, raw_length)
        path = cut_polyline(raw, trim_start, raw_length - trim_end)
        if len(path) < 2 or float(np.hypot(*np.diff(path, axis=0).T).sum()) < 0.3:
            return
        path = smooth(resample(path, LANE_SPACING))
        way = segment.way
        lane = Lane(id=len(self.lanes), kind="road", way=way.id, xy=path)
        lane.limit_kmh = self._lane_limit(way)
        lane.tier = CLASS_TIER.get(way.tags.get("highway"), 1)
        lane.roundabout = way.tags.get("junction") in {"roundabout", "circular"}
        lane.start_node = segment.start_node if travel > 0 else segment.end_node
        lane.end_node = segment.end_node if travel > 0 else segment.start_node
        lane.segment = segment.id
        lane.travel = travel
        lane.raw_length = raw_length
        lane.trim_start = trim_start
        length = lane.length()
        for s, approach_id in self.signal_stops.get((segment.id, travel), []):
            # A stop line may lie before the lane's start (the junction trim took the lane's first metres): negative s.
            stop_s = float(np.clip(s - trim_start, -MAX_STOP_BEFORE_LANE, length))
            lane.stops.append((stop_s, approach_id))
        lane.curve_kmh = curvature_speeds(path, lane.limit_kmh)
        self.lanes.append(lane)
        self.lane_of[(segment.id, travel)] = lane
        self.arms[lane.start_node].append(lane)
        self.arms[lane.end_node].append(lane)

    # ------------------------------------------------------------ heights

    def assign_heights(self):
        """Road surface height at every lane point; bridges ramp between the heights at their ends."""
        bridge_lines = {}
        for way, _ in self.net.bridges:
            bridge_lines[way.id] = (shapely.LineString(way.xy), float(self.net.height.sample(*way.xy[0])),
                                    float(self.net.height.sample(*way.xy[-1])))
        for lane in self.lanes:
            x, y = lane.xy[:, 0], lane.xy[:, 1]
            z = self.net.height.sample(x, y)
            if lane.way in bridge_lines:
                line, z_start, z_end = bridge_lines[lane.way]
                t = np.array([line.project(shapely.Point(px, py)) / max(line.length, 1e-6) for px, py in lane.xy])
                z = z_start + (z_end - z_start) * np.clip(t, 0.0, 1.0)
            lane.z = np.asarray(z, dtype=np.float64)

    # ------------------------------------------------------------ connections

    def _end_direction(self, lane, at_end):
        """Unit direction of travel at the end or the start of a lane."""
        pts = lane.xy
        if at_end:
            return unit(pts[-1] - pts[-2])
        return unit(pts[1] - pts[0])

    def build_connections(self):
        """Links every incoming lane to every outgoing lane at each node, and turns dead ends around."""
        self.node_connections = defaultdict(list)
        for node in sorted(self.arms, key=str):
            incoming = {lane.id: lane for lane in self.arms[node] if lane.end_node == node}
            outgoing = {lane.id: lane for lane in self.arms[node] if lane.start_node == node}
            segments_here = {lane.segment for lane in self.arms[node]}
            multi = len(segments_here) >= 3 or (self.arm_count[node] >= 3)
            for lane_in in incoming.values():
                made = False
                for lane_out in outgoing.values():
                    if lane_out.segment == lane_in.segment and lane_out.travel != lane_in.travel and lane_in.id != lane_out.id:
                        continue    # turning back on the same road is only for dead ends
                    if lane_out.id == lane_in.id:
                        continue
                    connection = self._connect(lane_in, lane_out, node, multi)
                    made = made or connection is not None
                if not made:
                    # dead end: turn around on the road when the opposite lane exists
                    partner = next((l for l in outgoing.values() if l.segment == lane_in.segment), None)
                    if partner is not None and self.arm_count[node] <= 1:
                        self._connect(lane_in, partner, node, multi, uturn=True)

    def _connect(self, lane_in, lane_out, node, multi, uturn=False):
        """Adds the Bezier connection from the end of one lane to the start of another; None when the turn is too sharp."""
        p0, p3 = lane_in.xy[-1], lane_out.xy[0]
        d_in, d_out = self._end_direction(lane_in, True), self._end_direction(lane_out, False)
        cross = d_in[0] * d_out[1] - d_in[1] * d_out[0]
        angle = math.degrees(math.atan2(cross, float(np.dot(d_in, d_out))))   # positive = right turn
        if not uturn and abs(angle) > MAX_TURN_DEGREES and multi:
            return None
        chord = float(np.hypot(*(p3 - p0)))
        handle = max(chord * 0.4, 0.5)
        if uturn:
            handle = 6.0
        path = bezier(p0, p0 + d_in * handle, p3 - d_out * handle, p3, CONNECTION_SPACING)
        lane = Lane(id=len(self.lanes), kind="uturn" if uturn else "connection", way=lane_in.way, xy=path)
        lane.from_lane, lane.to_lane, lane.node = lane_in.id, lane_out.id, node
        lane.limit_kmh = min(lane_in.limit_kmh, lane_out.limit_kmh)
        lane.roundabout = lane_in.roundabout and lane_out.roundabout
        lane.tier = lane_in.tier
        if abs(angle) < STRAIGHT_ANGLE_DEGREES:
            lane.turn = 0
        else:
            lane.turn = 1 if angle > 0 else -1
        lane.angle = angle
        turn_cap = 15.0 if uturn else 25.0 if lane.turn != 0 and multi else None
        lane.curve_kmh = curvature_speeds(path, lane.limit_kmh, turn_cap)
        if len(path) < 2:
            return None
        lane_in.next.append(lane.id)
        lane.next = [lane_out.id]
        self.lanes.append(lane)
        self.node_connections[node].append(lane)
        return lane

    # ------------------------------------------------------------ controls and priorities

    def _sign_controls(self):
        """Stop and give way signs resolved to the lane they apply to: lane id -> 'stop' | 'yield'."""
        controls = {}
        for sign in self.builder.signs:
            names = sign["names"]
            if "Zeichen_206" in names:
                kind = "stop"
            elif "Zeichen_205" in names:
                kind = "yield"
            else:
                continue
            position = np.array([sign["x"], sign["y"]])
            yaw = math.radians(sign["yaw"])
            direction = np.array([math.cos(yaw), math.sin(yaw)])
            best = None
            for lane in self.lanes:
                if lane.kind != "road":
                    continue
                s, distance = project_on_polyline(lane.xy, position)
                if distance > SIGN_SNAP_DISTANCE:
                    continue
                if lane.length() - s > 40.0:
                    continue
                if float(np.dot(self._end_direction(lane, True), direction)) < 0.7:
                    continue
                if best is None or distance < best[0]:
                    best = (distance, lane)
            if best is not None:
                controls[best[1].id] = kind
        return controls

    def assign_controls(self):
        """Gives the end of every road lane at a junction its control and every connection its priority level."""
        sign_controls = self._sign_controls()
        signal_lanes = {lane.id for lane in self.lanes if lane.stops}
        self.lane_signals = {}
        for node, connections in self.node_connections.items():
            incoming = [lane for lane in self.arms[node] if lane.end_node == node]
            if self.arm_count[node] < 3:
                continue
            node_has_signal = any(lane.id in signal_lanes for lane in incoming)
            node_has_sign = any(lane.id in sign_controls for lane in incoming)
            tiers = [lane.tier for lane in incoming]
            top = max(tiers)
            top_count = sum(1 for t in tiers if t == top)
            ring_node = any(lane.roundabout for lane in incoming) and any(not lane.roundabout for lane in incoming)
            for lane in incoming:
                if lane.id in signal_lanes:
                    level, name = LEVEL_SIGNAL, "signal"
                elif node_has_signal:
                    level, name = LEVEL_YIELD, "yield"
                elif ring_node:
                    level, name = (LEVEL_PRIORITY, "priority") if lane.roundabout else (LEVEL_YIELD, "yield")
                elif lane.id in sign_controls:
                    level, name = LEVEL_YIELD, sign_controls[lane.id]
                elif node_has_sign:
                    level, name = LEVEL_PRIORITY, "priority"
                elif top_count >= 2 and min(tiers) < top:
                    level, name = (LEVEL_PRIORITY, "priority") if lane.tier == top else (LEVEL_YIELD, "yield")
                else:
                    level, name = LEVEL_EQUAL, "equal"
                lane.control = name
                lane.control_level = level
            for connection in connections:
                source = self.lanes[connection.from_lane]
                connection.level = getattr(source, "control_level", LEVEL_EQUAL)
                if source.stops:
                    junction_id, phase = self.approach_phase[source.stops[0][1]]
                    connection.phase = phase
            # lanes with a signal stop need the control even where fewer than three arms meet (pedestrian crossings)
        for lane in self.lanes:
            if lane.kind == "road" and lane.stops and lane.control == "none":
                lane.control = "signal"

    def _yields_to(self, a, b):
        """How connection a has to treat connection b where their paths meet.

        Returns None when they are never in the way of each other, else (a gives way, strict). A strict relation comes from
        different priority levels or the left turn rule and never flips; the right before left tie is not strict, because
        around a junction it can run in a circle and then the car that arrived first has to go.
        """
        if a.phase >= 0 and b.phase >= 0 and a.level == LEVEL_SIGNAL and b.level == LEVEL_SIGNAL:
            if a.phase != b.phase:
                return None     # never green together
        if a.level != b.level:
            return a.level < b.level, True
        left_a, left_b = a.turn < 0, b.turn < 0
        if left_a != left_b:
            return left_a, True              # a left turn gives way to oncoming straight and right turns
        direction_a = self._end_direction(self.lanes[a.from_lane], True)
        direction_b = self._end_direction(self.lanes[b.from_lane], True)
        arm_b = -direction_b
        side = float(np.dot(right_of(direction_a), arm_b))
        if a.level == LEVEL_PRIORITY and not (a.roundabout and b.roundabout):
            return None
        if abs(side) > 0.25:
            return side > 0.0, False         # right before left: b comes from a's right
        return a.id > b.id, False

    def find_conflicts(self):
        """Pairs of connections at a node whose paths come closer than a car width."""
        for node, connections in self.node_connections.items():
            if len(connections) < 2:
                continue
            profiles = []
            for connection in connections:
                along = connection.arclength()
                profiles.append((connection, along))
            for i, (a, along_a) in enumerate(profiles):
                for b, along_b in profiles[i + 1:]:
                    if a.from_lane == b.from_lane:
                        continue
                    distances = np.hypot(a.xy[:, None, 0] - b.xy[None, :, 0], a.xy[:, None, 1] - b.xy[None, :, 1])
                    close = distances < CONFLICT_DISTANCE
                    if not close.any():
                        continue
                    rows = np.where(close.any(axis=1))[0]
                    columns = np.where(close.any(axis=0))[0]
                    a_range = (max(along_a[rows[0]] - CONFLICT_MARGIN, 0.0), min(along_a[rows[-1]] + CONFLICT_MARGIN, along_a[-1]))
                    b_range = (max(along_b[columns[0]] - CONFLICT_MARGIN, 0.0), min(along_b[columns[-1]] + CONFLICT_MARGIN, along_b[-1]))
                    relation = self._yields_to(a, b)
                    if relation is None:
                        continue
                    a_yields, strict = relation
                    kind = 2 if strict else 1       # 0 would be: does not give way
                    a.conflicts.append((b.id, a_range, b_range, kind if a_yields else 0))
                    b.conflicts.append((a.id, b_range, a_range, 0 if a_yields else kind))

    # ------------------------------------------------------------ connectivity

    def mark_good_lanes(self):
        """Lanes in the largest strongly connected component: routes and spawns stay on them so no car gets stuck."""
        count = len(self.lanes)
        rows, columns = [], []
        for lane in self.lanes:
            for target in lane.next:
                rows.append(lane.id)
                columns.append(target)
        graph = csr_matrix((np.ones(len(rows)), (rows, columns)), shape=(count, count))
        _, labels = connected_components(graph, directed=True, connection="strong")
        sizes = np.bincount(labels)
        main = int(np.argmax(sizes))
        for lane in self.lanes:
            lane.good = bool(labels[lane.id] == main)
        return int(sizes[main]), count

    def build(self):
        """Builds the whole graph; returns (lanes in the main loop, all lanes)."""
        self.build_segments()
        self.build_road_lanes()
        self.build_connections()
        self.assign_heights()
        self.assign_controls()
        self.find_conflicts()
        return self.mark_good_lanes()

    # ------------------------------------------------------------ output

    def to_dict(self):
        """The graph as the dictionary written to lanes.json."""
        out_lanes = []
        for lane in self.lanes:
            points = np.round(np.column_stack([lane.xy, lane.z]), 3).ravel().tolist()
            record = {"id": lane.id, "kind": {"road": 0, "connection": 1, "uturn": 2}[lane.kind], "way": lane.way,
                      "limit": lane.limit_kmh, "tier": lane.tier, "pts": points, "next": lane.next,
                      "good": 1 if lane.good else 0}
            if lane.roundabout:
                record["ring"] = 1
            speeds = np.round(lane.curve_kmh).astype(int)
            if int(speeds.min()) < lane.limit_kmh - 1:
                record["vc"] = speeds.tolist()
            if lane.kind == "road":
                record["ctl"] = lane.control
                if lane.stops:
                    record["stops"] = [[round(s, 2), approach] for s, approach in lane.stops]
            else:
                record["from"] = lane.from_lane
                record["to"] = lane.to_lane
                record["turn"] = lane.turn
                record["level"] = lane.level
                if lane.conflicts:
                    record["conf"] = [[other, round(a[0], 2), round(a[1], 2), round(b[0], 2), round(b[1], 2), yields]
                                      for other, a, b, yields in lane.conflicts]
            out_lanes.append(record)
        return {"version": 1, "lanes": out_lanes}
