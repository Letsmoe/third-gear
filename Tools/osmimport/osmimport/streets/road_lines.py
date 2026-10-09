"""The lines of a road network: every segment's kerbs and painted lines with their tapers and junction cuts, and
the guide lines through the junctions. Ties network.py, layout.py and junctions.py together."""
from dataclasses import dataclass

from . import assumptions, lines as line_rules, tags as osm_tags
from .cross_section import Travel
from .junctions import JunctionLines
from .layout import build_layouts
from .lines import LineKey, LineKind, Side
from .network import SegmentNetwork

# Speeds from which the long rural dashes are painted.
RURAL_DASH_SPEED = 70.0


@dataclass
class RoadLines:
    network: SegmentNetwork
    layouts: list           # SegmentLayout per segment id
    guides: list            # (marking kind, (n, 2) points)

    def painted(self) -> list:
        """(marking kind, (n, 2) points) of every painted line: along the segments, then inside the junctions."""
        result = []
        for layout in self.layouts:
            for key, kind in layout.painted.items():
                result += _painted_pieces(layout, key, kind)
        return [(kind, xy) for kind, xy in result if xy is not None] + self.guides

    def kerbs(self, layout) -> tuple:
        """The left and right kerb of a segment as polylines, tapers included."""
        return layout.line_xy(LineKey(LineKind.KERB, Side.LEFT)), layout.line_xy(LineKey(LineKind.KERB, Side.RIGHT))


def build(ways, sections: dict, urban: dict, signal_points) -> RoadLines:
    """The lines of the given ways. sections and urban: per way id (urban: True inside towns); signal_points: (x, y)
    of the traffic signals, which decide where left turns get guide lines."""
    network = SegmentNetwork(ways, sections)
    painted = {segment.id: line_rules.painted_lines(segment.way.tags, segment.section, _is_rural(segment.way.tags))
               for segment in network.segments}
    urban_by_segment = {segment.id: urban[segment.way.id] for segment in network.segments}
    junctions = JunctionLines(network, signal_points)
    mouths = junctions.mouths()
    layouts = build_layouts(network, painted, urban_by_segment, mouths)
    return RoadLines(network, layouts, junctions.guide_lines(layouts, mouths))


def _painted_pieces(layout, key: LineKey, kind: str) -> list:
    """(marking kind, points) of one painted line. A double centre line is two solid lines; a turn lane's line is
    broken along the road and solid over the queueing length before the junction it leads to."""
    if kind == "centre_double":
        start_s, end_s = layout.line_range(key)
        half_gap = assumptions.DOUBLE_LINE_SPACING / 2
        return [("solid", layout.line_between(key, start_s, end_s, -half_gap)),
                ("solid", layout.line_between(key, start_s, end_s, half_gap))]
    if kind == "turn_lane":
        start_s, end_s = layout.line_range(key)
        if key.travel == Travel.FORWARD:
            queue_start = max(start_s, end_s - assumptions.TURN_LANE_QUEUE_LENGTH)
            return [("turn_lane_dash", layout.line_between(key, start_s, queue_start)),
                    ("turn_lane_solid", layout.line_between(key, queue_start, end_s))]
        queue_end = min(end_s, start_s + assumptions.TURN_LANE_QUEUE_LENGTH)
        return [("turn_lane_solid", layout.line_between(key, start_s, queue_end)),
                ("turn_lane_dash", layout.line_between(key, queue_end, end_s))]
    return [(kind, layout.line_xy(key))]


def _is_rural(tags) -> bool:
    """True where the long rural dashes are painted: from RURAL_DASH_SPEED."""
    speed = osm_tags.speed_limit(tags)
    return speed is not None and speed >= RURAL_DASH_SPEED
