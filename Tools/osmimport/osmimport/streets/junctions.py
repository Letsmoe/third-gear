"""Junctions: where each arm's painted lines stop, which arms are one road going straight through, and the guide
lines inside them.

* An arm's mouth is where its carriageway leaves the other arms' carriageways, plus the corner radius: the lane and
  centre lines stop there instead of running into the crossing road, which keeps the junction clear.
* Lane and centre lines don't run through a junction: where the lines of two crossing roads would meet inside it,
  a small cross marks the spot. The road going straight through continues its edge lines as broken broad lines
  across the mouths of side roads. Driveways and other minor arms don't interrupt its lines at all.
* At signalised junctions, approaches with their own left-turn lanes get guide lines around the corner, along the
  right-hand edge of each left-turn lane's path into the road it turns into.
"""
import math

import numpy as np

from . import assumptions, corrections, polyline, tags as osm_tags
from .cross_section import Travel
from .layout import arm_keys, arm_lines
from .lines import LineKey, LineKind, Side, mirrored
from .network import NodeKind, SegmentEnd, SegmentNetwork
from .splits import find_split

LEFT_TURN_ANGLES = (45.0, 135.0)
# Lines the through road carries across a junction (edge lines, broken across side roads), and the lines whose
# crossings with the other road's get a cross instead.
CONTINUED_KINDS = {LineKind.EDGE, LineKind.CYCLE}
CROSSED_KINDS = {LineKind.CENTRE, LineKind.LANE}
# Lines meeting at a shallower angle than this get no cross.
CROSS_MIN_ANGLE = 30.0


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
            if find_split(self.network, node) is not None:
                continue  # the carriageways continue the two-way road's lines (layout.py)
            for end in ends:
                others = self._arms_that_interrupt(end, ends)
                if not others:
                    result[end] = 0.0
                    continue
                result[end] = self._clear_distance(end, others) + assumptions.CORNER_RADIUS
            for through_pair in self._through_pairs(ends):
                for end in through_pair:
                    result.update(self._merge_mouths(end, ends, through_pair, result[end]))
        return result

    def _merge_mouths(self, end: SegmentEnd, ends: list, through_pair: tuple, mouth: float) -> dict:
        """{(end, LineKey facing out of the node): distance} for the edge and cycle lines of a through arm on the
        side where another road joins or leaves at a shallow angle: they stay broken until that road's carriageway
        is MERGE_GAP away, not just clear."""
        result = {}
        direction = self.network.arm_direction(end)
        for other in ends:
            if other in through_pair or self.is_minor(other):
                continue
            if corrections.deflection(self.network, end, other) < 180.0 - assumptions.MERGE_MAX_ANGLE:
                continue
            side = Side.LEFT
            if float(np.dot(self.network.arm_direction(other), polyline.right_of(direction))) > 0:
                side = Side.RIGHT
            distance = max(self._clear_distance(end, [other], assumptions.MERGE_GAP), mouth)
            for kind in (LineKind.EDGE, LineKind.CYCLE, LineKind.CYCLE_OUTER):
                key = LineKey(kind, side)
                result[(end, key)] = max(distance, result.get((end, key), 0.0))
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

    def _clear_distance(self, end: SegmentEnd, others: list, gap: float = 0.0) -> float:
        """How far from the node the arm's carriageway is clear of the other arms' (network.clear_distance)."""
        return self.network.clear_distance(end, others, gap)

    # ------------------------------------------------------------ guide lines

    def guide_lines(self, layouts: list, mouths: dict) -> list:
        """(marking kind, (n, 2) points) of every line inside the junctions, from the segment layouts."""
        result = []
        for node in self.junctions:
            ends = self.network.ends_at[node]
            if find_split(self.network, node) is not None:
                continue
            pairs = self._through_pairs(ends)
            for entering, leaving in pairs:
                result += self._through_guides(layouts, entering, leaving, ends, mouths)
            result += self._crosses(layouts, ends, pairs, mouths)
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
            if counterpart not in leaving_painted or key.kind not in CONTINUED_KINDS:
                continue
            guide_kind = self._guide_kind(key, kind, entering, leaving, side_arms)
            continued = [key]
            if guide_kind == "cycle_furt":
                continued.append(LineKey(LineKind.CYCLE_OUTER, key.side))
            for line_key in continued:
                entering_mouth = line_mouth(mouths, entering, line_key)
                leaving_mouth = line_mouth(mouths, leaving, mirrored(line_key))
                start = self._mouth_point(entering, entering_lines[line_key], entering_mouth)
                end = self._mouth_point(leaving, leaving_lines[mirrored(line_key)], leaving_mouth)
                curve = polyline.smooth_curve(start, -self._direction_at_mouth(entering, entering_mouth), end,
                                              self._direction_at_mouth(leaving, leaving_mouth),
                                              assumptions.LINE_POINT_SPACING)
                result.append((guide_kind, curve))
        return result

    def _crosses(self, layouts: list, ends: list, pairs: list, mouths: dict) -> list:
        """Crosses where the lane and centre lines of two crossing roads would meet inside the junction (German
        junctions don't carry the lines through). Each road's lines run straight across the junction: from mouth to
        mouth for a road going through, from the mouth into the junction for one that ends there."""
        chords = []   # (road number, start, end)
        paired = set()
        for road, (entering, leaving) in enumerate(pairs):
            paired.update((entering, leaving))
            chords += [(road, start, end) for start, end in self._through_chords(layouts, entering, leaving, mouths)]
        for road, end in enumerate(ends, start=len(pairs)):
            if end in paired or self.is_minor(end):
                continue
            chords += [(road, start, stop) for start, stop in self._entry_chords(layouts, end, mouths)]
        result = []
        for index, (road, start, end) in enumerate(chords):
            for other_road, other_start, other_end in chords[index + 1:]:
                if other_road == road:
                    continue
                crossing = _segment_intersection(start, end, other_start, other_end)
                if crossing is None or _crossing_angle(end - start, other_end - other_start) < CROSS_MIN_ANGLE:
                    continue
                result += _cross(crossing, end - start, other_end - other_start)
        return result

    def _through_chords(self, layouts: list, entering: SegmentEnd, leaving: SegmentEnd, mouths: dict) -> list:
        """(start, end) of the straight lines joining the painted lane and centre lines of a through road's arms."""
        entering_lines = arm_lines(layouts[entering.segment].lines, entering)
        leaving_lines = arm_lines(layouts[leaving.segment].lines, leaving)
        leaving_painted = arm_keys(layouts[leaving.segment].painted, leaving)
        chords = []
        for key in arm_keys(layouts[entering.segment].painted, entering):
            if key.kind not in CROSSED_KINDS or mirrored(key) not in leaving_painted:
                continue
            chords.append((self._mouth_point(entering, entering_lines[key], mouths.get(entering, 0.0)),
                           self._mouth_point(leaving, leaving_lines[mirrored(key)], mouths.get(leaving, 0.0))))
        return chords

    def _entry_chords(self, layouts: list, end: SegmentEnd, mouths: dict) -> list:
        """(start, end) of the painted lane and centre lines of an arm ending at the junction, run on straight
        across it."""
        lines = arm_lines(layouts[end.segment].lines, end)
        inward = -self._direction_at_mouth(end, mouths.get(end, 0.0))
        reach = 2.0 * mouths.get(end, 0.0) + self._segment(end).section.width()
        chords = []
        for key in arm_keys(layouts[end.segment].painted, end):
            if key.kind in CROSSED_KINDS:
                start = self._mouth_point(end, lines[key], mouths.get(end, 0.0))
                chords.append((start, start + inward * reach))
        return chords

    def _guide_kind(self, key: LineKey, kind: str, entering: SegmentEnd, leaving: SegmentEnd, side_arms: list) -> str:
        """How a line continues across a junction: an edge line as a broken broad line and a cycle lane as a cycle
        crossing (furt) where a side road joins on its side, unchanged on a side no other road joins."""
        if not side_arms:
            return kind
        # Facing out of the entering arm is facing against the through traffic, so its left edge is on the right
        # of the traffic going from entering to leaving.
        through = polyline.unit(self.network.arm_direction(leaving) - self.network.arm_direction(entering))
        right = polyline.right_of(through)
        on_right = key.side == Side.LEFT
        for arm in side_arms:
            arm_on_right = float(np.dot(self.network.arm_direction(arm), right)) > 0
            if arm_on_right != on_right:
                continue
            if key.kind == LineKind.CYCLE:
                return "cycle_furt"
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
            entering_mouth = line_mouth(mouths, entering, start_key)
            target_mouth = line_mouth(mouths, target, end_key)
            start = self._mouth_point(entering, entering_lines[start_key], entering_mouth)
            end = self._mouth_point(target, target_lines[end_key], target_mouth)
            curve = polyline.smooth_curve(start, -self._direction_at_mouth(entering, entering_mouth), end,
                                          self._direction_at_mouth(target, target_mouth),
                                          assumptions.LINE_POINT_SPACING)
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
        return corrections.deflection(self.network, first, second)

    def _is_signalised(self, node) -> bool:
        if len(self.signal_points) == 0:
            return False
        distances = np.hypot(*(self.signal_points - self.network.node_xy[node]).T)
        return bool(distances.min() <= assumptions.SIGNAL_JUNCTION_RADIUS)

    def _mouth_point(self, end: SegmentEnd, offset: float, distance: float) -> np.ndarray:
        """The point of a line (offset facing out of the node) at a distance out along the arm."""
        line = self.network.arm_line(end)
        distance = min(distance, polyline.length(line))
        return polyline.point_at(line, distance) + polyline.right_of(polyline.direction_at(line, distance)) * offset

    def _direction_at_mouth(self, end: SegmentEnd, distance: float) -> np.ndarray:
        """The direction out of the node at a distance out along the arm."""
        line = self.network.arm_line(end)
        return polyline.direction_at(line, min(distance, polyline.length(line)))


def line_mouth(mouths: dict, end: SegmentEnd, key: LineKey) -> float:
    """Where a line (key facing out of the node) of an arm stops: its own distance where it has one, else the arm's."""
    return mouths.get((end, key), mouths.get(end, 0.0))


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
    turns = osm_tags.turn_lanes(tags, toward_node)
    count = 0
    for value in turns:
        directions = set(value.split(";"))
        if not directions or not directions <= {"left", "slight_left", "sharp_left"}:
            break
        count += 1
    return count


def _segment_intersection(first_start, first_end, second_start, second_end):
    """The point where two straight segments cross, or None."""
    first = first_end - first_start
    second = second_end - second_start
    denominator = first[0] * second[1] - first[1] * second[0]
    if abs(denominator) < 1e-9:
        return None
    gap = second_start - first_start
    along_first = (gap[0] * second[1] - gap[1] * second[0]) / denominator
    along_second = (gap[0] * first[1] - gap[1] * first[0]) / denominator
    if not (0.0 <= along_first <= 1.0 and 0.0 <= along_second <= 1.0):
        return None
    return first_start + first * along_first


def _crossing_angle(first, second) -> float:
    """The angle between two lines in degrees, 0 to 90."""
    cosine = abs(float(np.dot(polyline.unit(first), polyline.unit(second))))
    return math.degrees(math.acos(min(cosine, 1.0)))


def _cross(centre, first_direction, second_direction) -> list:
    """("solid", points) of the two bars of a cross, each along one of the crossing lines."""
    half = assumptions.JUNCTION_CROSS_LENGTH / 2
    bars = []
    for direction in (first_direction, second_direction):
        along = polyline.unit(direction) * half
        bars.append(("solid", np.array([centre - along, centre + along])))
    return bars
