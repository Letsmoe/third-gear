"""Where each segment's lines run: their offsets along the segment, the tapers between roads of different
cross-sections, and where lines start and stop.

At a continuation node the wider of the two roads moves its lines over a taper, so that at the node they meet the
narrower road's lines; lines that only one of the two roads has (a lane that appears or ends) start or stop where the
taper does, never at the node. At a junction the painted lines stop at the junction's mouth (junctions.py); the
kerbs run on, since the junction's surface is built from them.
"""
from dataclasses import dataclass, field

import numpy as np

from . import assumptions, lines as line_rules, polyline, tags as osm_tags
from .lines import LineKey, LineKind, Side
from .network import NodeKind, SegmentEnd, SegmentNetwork
from .splits import Split, find_split


@dataclass
class EndShape:
    """How a segment's lines behave at one of its ends, in the segment's own frame."""
    taper: float = 0.0
    node_offsets: dict = field(default_factory=dict)   # LineKey -> offset at the node, reached over the taper
    cuts: dict = field(default_factory=dict)           # LineKey -> distance from the node where the line begins


@dataclass
class SegmentLayout:
    segment: object
    lines: dict                 # LineKey -> offset along the middle of the segment
    painted: dict               # LineKey -> marking kind
    start: EndShape = field(default_factory=EndShape)
    end: EndShape = field(default_factory=EndShape)

    def offsets(self, key: LineKey, along) -> np.ndarray:
        """The line's offset at the distances along the segment's centre line."""
        along = np.asarray(along, dtype=np.float64)
        own = self.lines[key]
        result = np.full(len(along), own)
        total = self.segment.length()
        for shape, distance_from_node in ((self.start, along), (self.end, total - along)):
            if key not in shape.node_offsets or shape.taper <= 0:
                continue
            blend = _taper_blend(distance_from_node / shape.taper)
            result += (shape.node_offsets[key] - own) * blend
        return result

    def line_range(self, key: LineKey) -> tuple:
        """(start, end) distance along the segment between which the line runs."""
        return self.start.cuts.get(key, 0.0), self.segment.length() - self.end.cuts.get(key, 0.0)

    def line_xy(self, key: LineKey):
        """The line as a polyline between its cuts, or None when nothing is left of it."""
        start_s, end_s = self.line_range(key)
        return self.line_between(key, start_s, end_s)

    def line_between(self, key: LineKey, start_s: float, end_s: float, shift: float = 0.0):
        """The part of a line between two distances along the segment, moved sideways by shift (to the right), or
        None when it is too short."""
        if end_s - start_s < 0.5:
            return None
        centre = polyline.resample(polyline.cut_polyline(self.segment.xy, start_s, end_s),
                                   assumptions.LINE_POINT_SPACING)
        if len(centre) < 2:
            return None
        along = start_s + polyline.arclength(centre)
        return polyline.offset_polyline(centre, self.offsets(key, along) + shift)

    def width_at_end(self, at_start: bool) -> float:
        """The kerb to kerb width at one end of the segment."""
        shape = end_shape(self, at_start)
        left = shape.node_offsets.get(LineKey(LineKind.KERB, Side.LEFT), self.lines[LineKey(LineKind.KERB, Side.LEFT)])
        right = shape.node_offsets.get(LineKey(LineKind.KERB, Side.RIGHT),
                                       self.lines[LineKey(LineKind.KERB, Side.RIGHT)])
        return right - left


def _taper_blend(fraction):
    """How much of the node's offset is left at a fraction of the taper from the node: 1 at the node, 0 at the end
    of the taper, with no kink at either end (an S-curve, as RAA's two parabolas)."""
    remaining = 1.0 - np.clip(fraction, 0.0, 1.0)
    return remaining * remaining * (3.0 - 2.0 * remaining)


def build_layouts(network: SegmentNetwork, painted: dict, urban: dict, mouths: dict) -> list:
    """The layout of every segment. painted: segment id -> {LineKey: marking kind}; urban: segment id -> bool;
    mouths: SegmentEnd -> distance from the junction node where its painted lines stop, and (SegmentEnd, LineKey
    facing out of the node) -> the distance for single lines that stop elsewhere."""
    layouts = [SegmentLayout(segment, line_rules.section_lines(segment.section), painted[segment.id])
               for segment in network.segments]
    done = set()
    for node in network.ends_at:
        kind = network.node_kind(node)
        if kind == NodeKind.CONTINUATION and node not in done:
            first, second = network.ends_at[node]
            _shape_continuation(network, layouts, urban, first, second)
            done.add(node)
        if kind != NodeKind.JUNCTION:
            continue
        split = find_split(network, node)
        if split is not None:
            _shape_split(layouts, split)
            continue
        for end in network.ends_at[node]:
            _cut_at_mouth(layouts[end.segment], end, mouths)
    return layouts


def _shape_split(layouts: list, split: Split):
    """Each carriageway of a dual carriageway split starts on its half of the two-way road and moves out to its own
    line over the gore: its lanes and outer edge from the two-way road's lanes and edge on its side, its inner edge
    and kerb from the two-way road's centre line."""
    two_way_lines = arm_lines(layouts[split.two_way.segment].lines, split.two_way)
    # Facing out of the node toward the carriageways, traffic coming in keeps to the left, so the incoming
    # carriageway's inner side is its right and the outgoing one's its left.
    for one_way, inner_side in ((split.incoming, Side.RIGHT), (split.outgoing, Side.LEFT)):
        layout = layouts[one_way.segment]
        shape = end_shape(layout, one_way.at_start)
        shape.taper = min(split.gore_length, layout.segment.length() * assumptions.TAPER_MAX_SEGMENT_SHARE)
        for key, offset in arm_lines(layout.lines, one_way).items():
            meeting = _split_meeting_offset(key, inner_side, two_way_lines)
            segment_key, _ = to_segment_frame(key, 0.0, one_way)
            if meeting is None:
                shape.cuts[segment_key] = shape.taper
                continue
            shape.node_offsets[segment_key] = to_segment_frame(key, meeting, one_way)[1]


def _split_meeting_offset(key: LineKey, inner_side: Side, two_way_lines: dict):
    """Where a carriageway's line (facing out of the node) starts on the two-way road at the node, or None when it
    has no counterpart there: the inner edge and kerb on the centre line, the others on their mirrored line."""
    if key.kind in (LineKind.EDGE, LineKind.KERB) and key.side == inner_side:
        return -two_way_lines.get(LineKey(LineKind.CENTRE), 0.0)
    counterpart = line_rules.mirrored(key)
    if counterpart not in two_way_lines:
        return None
    return -two_way_lines[counterpart]


def end_shape(layout: SegmentLayout, at_start: bool) -> EndShape:
    if at_start:
        return layout.start
    return layout.end


def arm_lines(lines: dict, end: SegmentEnd) -> dict:
    """A segment's lines ({LineKey: offset}) facing out of the node at this end."""
    if end.at_start:
        return dict(lines)
    return line_rules.mirrored_lines(lines)


def arm_keys(values: dict, end: SegmentEnd) -> dict:
    """A segment's {LineKey: value} with the keys facing out of the node at this end (values unchanged)."""
    if end.at_start:
        return dict(values)
    return {line_rules.mirrored(key): value for key, value in values.items()}


def to_segment_frame(key: LineKey, offset: float, end: SegmentEnd):
    """A line given facing out of the node at this end, in the segment's own frame."""
    if end.at_start:
        return key, offset
    return line_rules.mirrored(key), -offset


def _cut_at_mouth(layout: SegmentLayout, end: SegmentEnd, mouths: dict):
    """Painted lines stop at the junction's mouth (mouths: the arm's distance under the end, and some lines' own
    distance under (end, key facing out of the node))."""
    shape = end_shape(layout, end.at_start)
    limit = layout.segment.length() * assumptions.TAPER_MAX_SEGMENT_SHARE
    for key in layout.painted:
        arm_key, _ = to_segment_frame(key, 0.0, end)  # mirroring is its own inverse
        mouth = mouths.get((end, arm_key), mouths.get(end, 0.0))
        shape.cuts[key] = min(mouth, limit)


def _shape_continuation(network, layouts, urban, first: SegmentEnd, second: SegmentEnd):
    """The taper between two segments meeting at a continuation node, on the wider one."""
    owner, other = _taper_owner(layouts, first, second)
    owner_layout, other_layout = layouts[owner.segment], layouts[other.segment]
    owner_lines = arm_lines(owner_layout.lines, owner)
    other_lines = arm_lines(other_layout.lines, other)
    meeting = {}
    for key, offset in owner_lines.items():
        counterpart = line_rules.mirrored(key)
        if counterpart in other_lines:
            meeting[key] = -other_lines[counterpart]
    unmatched_owner = [key for key in owner_lines if key not in meeting]
    unmatched_other = [key for key in other_lines if line_rules.mirrored(key) not in owner_lines]
    shift = max([abs(owner_lines[key] - offset) for key, offset in meeting.items()], default=0.0)
    if shift < 0.05 and not unmatched_owner and not unmatched_other:
        return
    taper = _taper(owner_layout, shift, urban[owner.segment])
    owner_shape = end_shape(owner_layout, owner.at_start)
    owner_shape.taper = taper
    for key, offset in meeting.items():
        segment_key, segment_offset = to_segment_frame(key, offset, owner)
        owner_shape.node_offsets[segment_key] = segment_offset
    for key in unmatched_owner:
        owner_shape.cuts[to_segment_frame(key, 0.0, owner)[0]] = taper
    other_shape = end_shape(other_layout, other.at_start)
    other_limit = other_layout.segment.length() * assumptions.TAPER_MAX_SEGMENT_SHARE
    for key in unmatched_other:
        other_shape.cuts[to_segment_frame(key, 0.0, other)[0]] = min(taper, other_limit)


def _taper_owner(layouts, first: SegmentEnd, second: SegmentEnd):
    """(owner, other): the wider segment moves its lines; on equal widths the one with more lanes, then the first."""
    def size(end):
        section = layouts[end.segment].segment.section
        return round(section.width(), 2), section.lane_count()
    if size(second) > size(first):
        return second, first
    return first, second


def _taper(layout: SegmentLayout, shift: float, urban: bool) -> float:
    """The taper length on a segment for a sideways shift, at most TAPER_MAX_SEGMENT_SHARE of the segment."""
    speed = osm_tags.speed_limit(layout.segment.way.tags)
    if speed is None:
        speed = assumptions.DEFAULT_SPEED_URBAN if urban else assumptions.DEFAULT_SPEED_RURAL
    length = line_rules.taper_length(shift, speed, urban)
    return min(length, layout.segment.length() * assumptions.TAPER_MAX_SEGMENT_SHARE)
