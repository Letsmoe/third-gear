"""The gore of every dual carriageway split: the paved wedge between the two carriageways where they open apart,
marked as a hatched area (Sperrfläche, Zeichen 298) between their inner edge lines."""
from dataclasses import dataclass

import numpy as np

from .layout import end_shape, to_segment_frame
from .lines import LineKey, LineKind, Side
from .network import NodeKind, SegmentEnd, SegmentNetwork
from .splits import find_split


@dataclass
class Gore:
    paved: np.ndarray       # polygon between the two inner kerbs
    hatched: np.ndarray     # polygon between the two inner edge lines
    outline: list           # (n, 2) inner edge lines of the gore that aren't painted already


def split_gores(network: SegmentNetwork, layouts: list) -> list:
    """The gores of all splits in the network."""
    gores = []
    for node in network.ends_at:
        if network.node_kind(node) != NodeKind.JUNCTION:
            continue
        split = find_split(network, node)
        if split is None:
            continue
        gore = _gore(layouts, split.incoming, split.outgoing)
        if gore is not None:
            gores.append(gore)
    return gores


def _gore(layouts: list, incoming: SegmentEnd, outgoing: SegmentEnd):
    """The gore between the incoming carriageway's right side and the outgoing one's left (facing out of the node),
    as long as the shorter of their two openings."""
    length = min(end_shape(layouts[incoming.segment], incoming.at_start).taper,
                 end_shape(layouts[outgoing.segment], outgoing.at_start).taper)
    if length < 1.0:
        return None
    sides = ((incoming, Side.RIGHT), (outgoing, Side.LEFT))
    kerbs = [_from_node(layouts[end.segment], end, LineKey(LineKind.KERB, side), length) for end, side in sides]
    edges = [_from_node(layouts[end.segment], end, LineKey(LineKind.EDGE, side), length) for end, side in sides]
    if any(line is None for line in kerbs + edges):
        return None
    outline = []
    for (end, side), edge in zip(sides, edges):
        key, _ = to_segment_frame(LineKey(LineKind.EDGE, side), 0.0, end)
        if key not in layouts[end.segment].painted:
            outline.append(edge)
    return Gore(np.concatenate([kerbs[0], kerbs[1][::-1]]), np.concatenate([edges[0], edges[1][::-1]]), outline)


def _from_node(layout, end: SegmentEnd, arm_key: LineKey, length: float):
    """A line of the arm (key facing out of the node) over its first metres, as points from the node outward."""
    key, _ = to_segment_frame(arm_key, 0.0, end)
    total = layout.segment.length()
    if end.at_start:
        return layout.line_between(key, 0.0, length)
    line = layout.line_between(key, total - length, total)
    if line is None:
        return None
    return line[::-1]
