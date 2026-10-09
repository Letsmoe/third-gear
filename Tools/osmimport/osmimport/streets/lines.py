"""The lines along a road's cross-section, named so they can be followed from one road into the next.

A line is a kerb, the edge of the travel lanes, the centre line between the two directions, or the k-th line between
lanes of one direction counted from that direction's kerb. Offsets are from the way's centre line, positive to the
right of the way's node order.

Seen from a node, every road leaving it is an arm; turning a road's lines to face out of the node mirrors them
(offsets change sign, left and right and the two directions swap). Two arms meeting at a node continue each other's
lines: an arm's line continues as the mirror of it on the other arm, at the mirrored offset.
"""
from dataclasses import dataclass
from enum import Enum

from . import assumptions, tags as osm_tags
from .cross_section import CrossSection, StripKind, Travel


class LineKind(Enum):
    KERB = "kerb"
    EDGE = "edge"        # outer edge of the travel lanes, where an edge line is painted
    CENTRE = "centre"    # between the two directions
    LANE = "lane"        # between two lanes of one direction


class Side(Enum):
    LEFT = "left"
    RIGHT = "right"


@dataclass(frozen=True)
class LineKey:
    kind: LineKind
    side: Side = None        # kerbs and edges
    travel: Travel = None    # lane lines: the direction of the lanes on both sides of the line
    index: int = 0           # lane lines: 1 for the line nearest that direction's kerb


MIRRORED_SIDE = {Side.LEFT: Side.RIGHT, Side.RIGHT: Side.LEFT}
MIRRORED_TRAVEL = {Travel.FORWARD: Travel.BACKWARD, Travel.BACKWARD: Travel.FORWARD, Travel.BOTH: Travel.BOTH,
                   Travel.NONE: Travel.NONE, None: None}


def mirrored(key: LineKey) -> LineKey:
    """The same line seen facing the other way."""
    side = MIRRORED_SIDE.get(key.side)
    return LineKey(key.kind, side, MIRRORED_TRAVEL[key.travel], key.index)


def mirrored_lines(lines: dict) -> dict:
    """{key: offset} seen facing the other way."""
    return {mirrored(key): -offset for key, offset in lines.items()}


def section_lines(section: CrossSection) -> dict:
    """{LineKey: offset} of every line of a cross-section."""
    width = section.width()
    lines = {LineKey(LineKind.KERB, Side.LEFT): -width / 2, LineKey(LineKind.KERB, Side.RIGHT): width / 2}
    lanes = [(strip, left, right) for strip, left, right in section.strip_edges()
             if strip.kind == StripKind.TRAVEL_LANE]
    if not lanes:
        return lines
    lines[LineKey(LineKind.EDGE, Side.LEFT)] = lanes[0][1]
    lines[LineKey(LineKind.EDGE, Side.RIGHT)] = lanes[-1][2]
    for position in range(len(lanes) - 1):
        key = _divider_key(lanes, position)
        if key is not None:
            lines[key] = lanes[position][2]
    return lines


def _divider_key(lanes: list, position: int):
    """The key of the line between lanes[position] and lanes[position + 1] (left to right), or None between lanes
    that are not divided (shared lanes)."""
    left_travel = lanes[position][0].travel
    right_travel = lanes[position + 1][0].travel
    if left_travel == Travel.BACKWARD and right_travel == Travel.FORWARD:
        return LineKey(LineKind.CENTRE)
    if left_travel != right_travel or left_travel not in (Travel.FORWARD, Travel.BACKWARD):
        return None
    if left_travel == Travel.FORWARD:
        # Forward lanes keep to the right kerb: count the forward lanes right of the line.
        index = sum(1 for strip, _, _ in lanes[position + 1:] if strip.travel == Travel.FORWARD)
        return LineKey(LineKind.LANE, travel=Travel.FORWARD, index=index)
    index = sum(1 for strip, _, _ in lanes[:position + 1] if strip.travel == Travel.BACKWARD)
    return LineKey(LineKind.LANE, travel=Travel.BACKWARD, index=index)


def painted_lines(tags, section: CrossSection, rural: bool) -> dict:
    """{LineKey: marking kind} of the lines painted on a road: lane and centre lines on marked roads with two or more
    lanes wide enough to mark (two-way roads from CENTRE_LINE_MIN_WIDTH, never in a Tempo 30 zone), edge lines on
    major roads and on wide rural ones."""
    painted = {}
    road_class = osm_tags.highway(tags)
    if road_class not in assumptions.PAINTED_CLASSES or tags.get("lane_markings") == "no" or tags.get("area") == "yes":
        return painted
    width = section.width()
    dash = "dash_urban"
    if rural:
        dash = "dash_rural"
    lines = section_lines(section)
    if _lane_lines_painted(tags, section):
        for key in lines:
            if key.kind in (LineKind.CENTRE, LineKind.LANE):
                painted[key] = dash
    if road_class in assumptions.EDGE_LINE_CLASSES or (rural and width >= assumptions.RURAL_EDGE_LINE_MIN_WIDTH):
        for key in lines:
            if key.kind == LineKind.EDGE:
                painted[key] = "edge"
    return painted


def _lane_lines_painted(tags, section: CrossSection) -> bool:
    """True when the centre and lane lines of a marked-class road are painted."""
    if section.lane_count() < 2 or osm_tags.is_zone30(tags):
        return False
    if min(strip.width for strip in section.travel_lanes()) < assumptions.MIN_MARKED_LANE_WIDTH:
        return False
    return osm_tags.is_oneway(tags) or section.width() >= assumptions.CENTRE_LINE_MIN_WIDTH


def taper_length(shift: float, speed_kmh: float, urban: bool) -> float:
    """How long a road takes to move its lines sideways by shift metres (a lane appearing or ending, the road
    widening): speed x shift / 3, at least TAPER_MIN_LENGTH, inside towns at most URBAN_TAPER_MAX_LENGTH."""
    length = max(speed_kmh * shift * assumptions.TAPER_SPEED_FACTOR, assumptions.TAPER_MIN_LENGTH)
    if urban:
        return min(length, assumptions.URBAN_TAPER_MAX_LENGTH)
    return length
