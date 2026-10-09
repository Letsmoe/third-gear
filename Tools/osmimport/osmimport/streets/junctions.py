"""Junctions: where each arm's painted lines stop, which arms are one road going straight through, and the guide
lines that carry lines across the junction.

* An arm's mouth is where its carriageway leaves the other arms' carriageways, plus the corner radius: the lane and
  centre lines stop there instead of running into the crossing road, which keeps the junction clear.
* The road going straight through continues its centre and lane lines across the junction as guide lines (1:1
  dashes), and its edge lines as broken broad lines across the mouths of side roads. Driveways and other minor arms
  don't interrupt its lines at all.
* At signalised junctions, approaches with their own left-turn lanes get guide lines around the corner, along the
  right-hand edge of each left-turn lane's path into the road it turns into.
"""
import math

import numpy as np

from . import assumptions, polyline, tags as osm_tags
from .cross_section import Travel
from .layout import arm_keys, arm_lines
from .lines import LineKey, LineKind, Side, mirrored
from .network import NodeKind, SegmentEnd, SegmentNetwork

# How far out along an arm the clearance search looks, and its step.
MOUTH_SEARCH_LIMIT = 60.0
MOUTH_SEARCH_STEP = 0.5
# Points across an arm's carriageway tested against the other arms.
MOUTH_PROBE_POINTS = 9
LEFT_TURN_ANGLES = (45.0, 135.0)


class JunctionLines:
    """Mouths and guide lines of every junction of a segment network."""

    def __init__(self, network: SegmentNetwork, signal_points):
        self.network = network
        self.signal_points = np.asarray(signal_points, dtype=np.float64).reshape(-1, 2)
        self.junctions = [node for node in network.ends_at if network.node_kind(node) == NodeKind.JUNCTION]

    # ------------------------------------------------------------ mouths

    def mouths(self) -> dict:
        """SegmentEnd -> distance from the junction node where the arm's painted lines stop."""
        result = {}
        for node in self.junctions:
            ends = self.network.ends_at[node]
            for end in ends:
                others = self._arms_that_interrupt(end, ends)
                if not others:
                    result[end] = 0.0
                    continue
                result[end] = self._clear_distance(end, others) + assumptions.CORNER_RADIUS
        return result

    def is_minor(self, end: SegmentEnd) -> bool:
        """True for driveways and similar arms, which don't interrupt the lines of the road they join."""
        return osm_tags.base_class(self._segment(end).way.tags) in assumptions.MINOR_ARM_CLASSES

    def _arms_that_interrupt(self, end: SegmentEnd, ends: list) -> list:
        """The other arms whose carriageway this arm's lines must stop short of: all of them for a minor arm; for
        the others the non-minor ones, and nothing when the only one left is the road's own continuation."""
        others = [other for other in ends if other != end]
        if self.is_minor(end):
            return others
        major = [other for other in others if not self.is_minor(other)]
        if len(major) == 1 and self._deflection(end, major[0]) <= assumptions.THROUGH_MAX_DEFLECTION_DEGREES:
            return []
        return major

    def _clear_distance(self, end: SegmentEnd, others: list) -> float:
        """How far from the node the arm's carriageway, across its whole width, is clear of the other arms'."""
        line = self.network.arm_line(end)
        half_width = self._segment(end).section.width() / 2
        other_lines = [(polyline.cut_polyline(self.network.arm_line(other), 0.0, MOUTH_SEARCH_LIMIT),
                        self._segment(other).section.width() / 2) for other in others]
        limit = min(MOUTH_SEARCH_LIMIT, polyline.length(line) * assumptions.TAPER_MAX_SEGMENT_SHARE)
        across = np.linspace(-half_width, half_width, MOUTH_PROBE_POINTS)
        for distance in np.arange(0.0, limit, MOUTH_SEARCH_STEP):
            centre = polyline.point_at(line, distance)
            right = polyline.right_of(polyline.direction_at(line, distance))
            probes = centre[None, :] + across[:, None] * right[None, :]
            if all(polyline.distances_to(other_line, probes).min() >= other_half
                   for other_line, other_half in other_lines):
                return float(distance)
        return float(limit)

    # ------------------------------------------------------------ guide lines

    def guide_lines(self, layouts: list, mouths: dict) -> list:
        """(marking kind, (n, 2) points) of every guide line through the junctions, from the segment layouts."""
        result = []
        for node in self.junctions:
            ends = self.network.ends_at[node]
            for entering, leaving in self._through_pairs(ends):
                result += self._through_guides(layouts, entering, leaving, ends, mouths)
            if self._is_signalised(node):
                for entering in ends:
                    result += self._left_turn_guides(layouts, entering, ends, mouths)
        return result

    def _through_pairs(self, ends: list) -> list:
        """Pairs of non-minor arms that are one road going straight through, the most important road first."""
        candidates = []
        for index, first in enumerate(ends):
            for second in ends[index + 1:]:
                if self.is_minor(first) or self.is_minor(second):
                    continue
                deflection = self._deflection(first, second)
                if deflection <= assumptions.THROUGH_MAX_DEFLECTION_DEGREES:
                    rank = min(self._rank(first), self._rank(second))
                    candidates.append((-rank, deflection, first, second))
        candidates.sort(key=lambda candidate: (candidate[0], candidate[1]))
        pairs = []
        used = set()
        for _, _, first, second in candidates:
            if first in used or second in used:
                continue
            pairs.append((first, second))
            used.update((first, second))
        return pairs

    def _through_guides(self, layouts: list, entering: SegmentEnd, leaving: SegmentEnd, ends: list,
                        mouths: dict) -> list:
        """The painted lines of the entering arm continued to the matching lines of the leaving arm."""
        entering_lines = arm_lines(layouts[entering.segment].lines, entering)
        entering_painted = arm_keys(layouts[entering.segment].painted, entering)
        leaving_lines = arm_lines(layouts[leaving.segment].lines, leaving)
        leaving_painted = arm_keys(layouts[leaving.segment].painted, leaving)
        side_arms = [end for end in ends if end not in (entering, leaving) and not self.is_minor(end)]
        result = []
        for key, kind in entering_painted.items():
            counterpart = mirrored(key)
            if counterpart not in leaving_painted:
                continue
            start = self._mouth_point(entering, entering_lines[key], mouths)
            end = self._mouth_point(leaving, leaving_lines[counterpart], mouths)
            curve = polyline.smooth_curve(start, -self._direction_at_mouth(entering, mouths), end,
                                          self._direction_at_mouth(leaving, mouths), assumptions.LINE_POINT_SPACING)
            result.append((self._guide_kind(key, kind, entering, leaving, side_arms), curve))
        return result

    def _guide_kind(self, key: LineKey, kind: str, entering: SegmentEnd, leaving: SegmentEnd, side_arms: list) -> str:
        """Guide dashes inside a junction; an edge line stays solid on a side no other road joins."""
        if not side_arms:
            return kind
        if key.kind != LineKind.EDGE:
            return "guide"
        # Facing out of the entering arm is facing against the through traffic, so its left edge is on the right
        # of the traffic going from entering to leaving.
        through = polyline.unit(self.network.arm_direction(leaving) - self.network.arm_direction(entering))
        right = polyline.right_of(through)
        on_right = key.side == Side.LEFT
        for arm in side_arms:
            arm_on_right = float(np.dot(self.network.arm_direction(arm), right)) > 0
            if arm_on_right == on_right:
                return "edge_guide"
        return kind

    def _left_turn_guides(self, layouts: list, entering: SegmentEnd, ends: list, mouths: dict) -> list:
        """Guide lines along the right-hand edge of the paths of the entering arm's exclusive left-turn lanes."""
        left_lanes = _exclusive_left_lanes(self._segment(entering).way.tags, entering)
        if left_lanes == 0:
            return []
        target = self._left_turn_target(entering, ends)
        if target is None:
            return []
        entering_lines = arm_lines(layouts[entering.segment].lines, entering)
        target_lines = arm_lines(layouts[target.segment].lines, target)
        incoming = _lane_count(entering_lines, Travel.BACKWARD)
        outgoing = _lane_count(target_lines, Travel.FORWARD)
        result = []
        for lane in range(1, left_lanes + 1):
            start_key = _line_right_of_lane(Travel.BACKWARD, incoming, lane)
            end_key = _line_right_of_lane(Travel.FORWARD, outgoing, min(lane, outgoing))
            if start_key not in entering_lines or end_key not in target_lines:
                continue
            start = self._mouth_point(entering, entering_lines[start_key], mouths)
            end = self._mouth_point(target, target_lines[end_key], mouths)
            curve = polyline.smooth_curve(start, -self._direction_at_mouth(entering, mouths), end,
                                          self._direction_at_mouth(target, mouths), assumptions.LINE_POINT_SPACING)
            result.append(("guide", curve))
        return result

    def _left_turn_target(self, entering: SegmentEnd, ends: list):
        """The arm a left turn from the entering arm goes into: the one nearest a right angle to the left."""
        arriving = -self.network.arm_direction(entering)
        best = None
        for end in ends:
            if end == entering or self.is_minor(end):
                continue
            angle = _turn_angle(arriving, self.network.arm_direction(end))
            low, high = LEFT_TURN_ANGLES
            if not low <= angle <= high:
                continue
            if best is None or abs(angle - 90.0) < best[0]:
                best = (abs(angle - 90.0), end)
        if best is None:
            return None
        return best[1]

    # ------------------------------------------------------------ helpers

    def _segment(self, end: SegmentEnd):
        return self.network.segments[end.segment]

    def _rank(self, end: SegmentEnd) -> int:
        return assumptions.ROAD_CLASS_RANK.get(osm_tags.highway(self._segment(end).way.tags), 0)

    def _deflection(self, first: SegmentEnd, second: SegmentEnd) -> float:
        """How many degrees a path from one arm into the other turns (0 for a straight continuation)."""
        through = float(np.dot(self.network.arm_direction(first), -self.network.arm_direction(second)))
        return math.degrees(math.acos(max(-1.0, min(1.0, through))))

    def _is_signalised(self, node) -> bool:
        if len(self.signal_points) == 0:
            return False
        distances = np.hypot(*(self.signal_points - self.network.node_xy[node]).T)
        return bool(distances.min() <= assumptions.SIGNAL_JUNCTION_RADIUS)

    def _mouth_point(self, end: SegmentEnd, offset: float, mouths: dict) -> np.ndarray:
        """The point of a line (offset facing out of the node) at the arm's mouth."""
        line = self.network.arm_line(end)
        distance = min(mouths.get(end, 0.0), polyline.length(line))
        return polyline.point_at(line, distance) + polyline.right_of(polyline.direction_at(line, distance)) * offset

    def _direction_at_mouth(self, end: SegmentEnd, mouths: dict) -> np.ndarray:
        """The direction out of the node at the arm's mouth."""
        line = self.network.arm_line(end)
        return polyline.direction_at(line, min(mouths.get(end, 0.0), polyline.length(line)))


def _turn_angle(arriving, leaving) -> float:
    """Degrees a path turns to the left from the arriving direction into the leaving one (negative to the right).
    The world frame is mirrored (y south), so a left turn has a negative cross product."""
    cross = arriving[0] * leaving[1] - arriving[1] * leaving[0]
    dot = float(np.dot(arriving, leaving))
    return math.degrees(math.atan2(-cross, dot))


def _lane_count(lines: dict, travel: Travel) -> int:
    """How many lanes go one way, from the lane lines between them (facing out of the node: BACKWARD comes in)."""
    dividers = [key for key in lines if key.kind == LineKind.LANE and key.travel == travel]
    return len(dividers) + 1


def _line_right_of_lane(travel: Travel, lane_count: int, lane_from_centre: int) -> LineKey:
    """The line on the driver's right of the lane-th lane counted from the centre, among lane_count lanes going one
    way (lane lines are counted from the kerb, so it is the (lane_count - lane)-th; the kerb-side lane has the edge)."""
    index = lane_count - lane_from_centre
    if index <= 0:
        if travel == Travel.FORWARD:
            return LineKey(LineKind.EDGE, Side.RIGHT)
        return LineKey(LineKind.EDGE, Side.LEFT)
    return LineKey(LineKind.LANE, travel=travel, index=index)


def _exclusive_left_lanes(tags, end: SegmentEnd) -> int:
    """How many lanes coming into the node at this end only turn left, counted from the driver's left."""
    toward_node = Travel.FORWARD
    if end.at_start:
        toward_node = Travel.BACKWARD  # the segment leaves the node along the way, so traffic comes in against it
    turns = _turn_lanes(tags, toward_node)
    count = 0
    for value in turns:
        directions = set(value.split(";"))
        if not directions or not directions <= {"left", "slight_left", "sharp_left"}:
            break
        count += 1
    return count


def _turn_lanes(tags, travel: Travel) -> list:
    """The turn:lanes values of the lanes going one way along the way, from the driver's left."""
    if osm_tags.is_oneway(tags):
        forward_is_travel = not osm_tags.is_reversed_oneway(tags)
        if (travel == Travel.FORWARD) != forward_is_travel:
            return []
        value = tags.get("turn:lanes", "")
    elif travel == Travel.FORWARD:
        value = tags.get("turn:lanes:forward", "")
    else:
        value = tags.get("turn:lanes:backward", "")
    if not value:
        return []
    return str(value).split("|")
