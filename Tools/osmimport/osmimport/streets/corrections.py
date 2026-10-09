"""Fixes for OSM mistakes the model can recognise from the network around a road.

* A short road piece narrower than the road on both sides of it, which continue it straight on and are the same kind
  of road, is mis-tagged (a lane count missing on a short way, say): it takes the narrower neighbour's cross-section.
"""
import math

import numpy as np

from . import assumptions, tags as osm_tags
from .network import NodeKind, SegmentEnd, SegmentNetwork

# Repeats of the narrowing fix, so a run of several short mis-tagged pieces is filled in from both ends.
NARROWING_PASSES = 3


def fix_short_narrowings(network: SegmentNetwork) -> int:
    """Gives every short piece narrower than both its straight neighbours the narrower neighbour's cross-section,
    in place; returns how many pieces changed."""
    changed = 0
    for _ in range(NARROWING_PASSES):
        fixes = []
        for segment in network.segments:
            replacement = _narrowing_replacement(network, segment)
            if replacement is not None:
                fixes.append((segment, replacement))
        for segment, replacement in fixes:
            segment.section = replacement
        changed += len(fixes)
        if not fixes:
            break
    return changed


def _narrowing_replacement(network: SegmentNetwork, segment):
    """The cross-section a short narrow piece should have (in its own frame), or None when it is fine."""
    if segment.length() > assumptions.SHORT_NARROWING_MAX_LENGTH:
        return None
    width = segment.section.width()
    neighbours = []
    for end in (SegmentEnd(segment.id, True), SegmentEnd(segment.id, False)):
        neighbour = straight_neighbour(network, end)
        if neighbour is None or not _same_kind_of_road(segment.way.tags, network.segments[neighbour.segment].way.tags):
            return None
        section = network.segments[neighbour.segment].section
        if section.width() < width + assumptions.NARROWING_MIN_DIFFERENCE:
            return None
        neighbours.append((section.width(), end, neighbour, section))
    _, end, neighbour, section = min(neighbours, key=lambda item: item[0])
    # Facing out of the node, our end and the neighbour's look in opposite directions when both ways run the same
    # way (one ends where the other starts); otherwise one of them runs against the other.
    if end.at_start == neighbour.at_start:
        return section.mirrored()
    return section


def straight_neighbour(network: SegmentNetwork, end: SegmentEnd):
    """The segment end that continues this one straight on at its node: the other end at a continuation, the arm
    going straight on at a junction (driveways and the like don't count), or None."""
    node = network.node_of(end)
    kind = network.node_kind(node)
    if kind == NodeKind.CONTINUATION:
        return network.other_end(end)
    if kind != NodeKind.JUNCTION:
        return None
    best = None
    for other in network.ends_at[node]:
        if other == end or _is_minor(network, other):
            continue
        turn = deflection(network, end, other)
        if turn <= assumptions.THROUGH_MAX_DEFLECTION_DEGREES and (best is None or turn < best[0]):
            best = (turn, other)
    if best is None:
        return None
    return best[1]


def _same_kind_of_road(tags, other_tags) -> bool:
    """True when both roads are one-way or both two-way, and both slip roads or neither: a one-way branch where a
    dual carriageway splits is not a narrowing of the road before the split."""
    return (osm_tags.is_oneway(tags) == osm_tags.is_oneway(other_tags)
            and osm_tags.is_link(tags) == osm_tags.is_link(other_tags))


def _is_minor(network: SegmentNetwork, end: SegmentEnd) -> bool:
    return osm_tags.base_class(network.segments[end.segment].way.tags) in assumptions.MINOR_ARM_CLASSES


def deflection(network: SegmentNetwork, first: SegmentEnd, second: SegmentEnd) -> float:
    """How many degrees a path from one arm into the other turns (0 for a straight continuation)."""
    through = float(np.dot(network.arm_direction(first), -network.arm_direction(second)))
    return math.degrees(math.acos(max(-1.0, min(1.0, through))))
