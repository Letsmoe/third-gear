"""The road network as segments: ways cut at junctions, and what lies at each end of a segment.

A node where three or more segment ends meet is a junction. A node where exactly two meet is a continuation (one
road becomes the next, or a way's end meets the next way's start). A node with one end is a dead end.
"""
from collections import defaultdict
from dataclasses import dataclass
from enum import Enum

import numpy as np

from . import assumptions, polyline
from .cross_section import CrossSection


# How far out along an arm the clearance search looks, and its step.
MOUTH_SEARCH_LIMIT = 60.0
MOUTH_SEARCH_STEP = 0.5
# Points across an arm's carriageway tested against the other arms.
MOUTH_PROBE_POINTS = 9


class NodeKind(Enum):
    JUNCTION = "junction"
    CONTINUATION = "continuation"
    DEAD_END = "dead end"


@dataclass
class Segment:
    """A piece of a way between two nodes that are junctions or way ends."""
    id: int
    way: object             # osm.Way
    xy: np.ndarray          # (n, 2) along the way's node order
    start_node: int
    end_node: int
    section: CrossSection

    def length(self) -> float:
        return polyline.length(self.xy)


@dataclass(frozen=True)
class SegmentEnd:
    """One end of a segment: at its start (the way's first node of the piece) or at its end."""
    segment: int
    at_start: bool


class SegmentNetwork:
    """Segments of the given ways and the ends meeting at every node."""

    def __init__(self, ways, sections: dict):
        self.segments = []
        self.node_xy = {}
        self.ends_at = defaultdict(list)
        degree = _node_degrees(ways)
        for way in ways:
            self._split_way(way, sections[way.id], degree)
        for segment in self.segments:
            self.ends_at[segment.start_node].append(SegmentEnd(segment.id, True))
            self.ends_at[segment.end_node].append(SegmentEnd(segment.id, False))

    def _split_way(self, way, section, degree):
        """Cuts a way at its inner junction nodes into segments."""
        start = 0
        for index, node in enumerate(way.node_ids):
            self.node_xy[node] = np.asarray(way.xy[index], dtype=np.float64)
            is_last = index == len(way.node_ids) - 1
            if index == 0 or not (is_last or degree[node] >= 3):
                continue
            xy = np.asarray(way.xy[start:index + 1], dtype=np.float64)
            if polyline.length(xy) > 0.05:
                self.segments.append(Segment(len(self.segments), way, xy, way.node_ids[start], node, section))
            start = index

    def node_kind(self, node) -> NodeKind:
        count = len(self.ends_at[node])
        if count >= 3:
            return NodeKind.JUNCTION
        if count == 2:
            return NodeKind.CONTINUATION
        return NodeKind.DEAD_END

    def node_of(self, end: SegmentEnd):
        segment = self.segments[end.segment]
        if end.at_start:
            return segment.start_node
        return segment.end_node

    def other_end(self, end: SegmentEnd):
        """The other segment end at a continuation node."""
        for candidate in self.ends_at[self.node_of(end)]:
            if candidate != end:
                return candidate
        return None

    def arm_line(self, end: SegmentEnd) -> np.ndarray:
        """The segment's centre line leaving the node at this end (reversed when the end is the segment's end)."""
        xy = self.segments[end.segment].xy
        if end.at_start:
            return xy
        return xy[::-1].copy()

    def arm_direction(self, end: SegmentEnd, reach: float = 6.0) -> np.ndarray:
        """The unit direction in which the segment leaves the node, measured over its first metres."""
        line = self.arm_line(end)
        return polyline.unit(polyline.point_at(line, min(reach, polyline.length(line))) - line[0])

    def clear_distance(self, end: SegmentEnd, others: list, gap: float = 0.0) -> float:
        """How far from the node the arm's carriageway, across its whole width, is clear of the other arms' (and
        at least gap away from them)."""
        line = self.arm_line(end)
        half_width = self.segments[end.segment].section.width() / 2
        other_lines = [(polyline.cut_polyline(self.arm_line(other), 0.0, MOUTH_SEARCH_LIMIT),
                        self.segments[other.segment].section.width() / 2 + gap) for other in others]
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


def _node_degrees(ways) -> dict:
    """How many way ends and passes touch each node: a pass counts twice, so three or more means a junction."""
    degree = defaultdict(int)
    for way in ways:
        last = len(way.node_ids) - 1
        for index, node in enumerate(way.node_ids):
            if index in (0, last):
                degree[node] += 1
            else:
                degree[node] += 2
    return degree
