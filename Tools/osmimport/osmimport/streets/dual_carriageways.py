"""Short dual carriageway sections redrawn parallel: a correction of the OSM geometry.

Where a two-way road splits into two one-way carriageways that meet again a little further on (around a median
island before a junction, say), OSM draws both carriageways from single nodes at either end, and the measured lines
bend and wobble between. Real carriageways there run parallel: this redraws both around their common axis at their
measured distance, opening over the gore at the split and over a few metres at the far end.

The section's end nodes stay where they are; the nodes in between move, in every way that uses them, and the
carriageways get extra shape points so their curves are smooth.
"""
import dataclasses
import itertools

import numpy as np

from . import assumptions, corrections, lines as line_rules, polyline, tags as osm_tags
from .network import NodeKind, SegmentEnd, SegmentNetwork
from .splits import find_split

# Shape points added along a redrawn carriageway, and the search length for the far end of a section.
POINT_SPACING = 2.0
MAX_SECTION_LENGTH = 300.0
# Sections whose carriageways are this far apart (median, centre line to centre line) are redrawn.
SEPARATION_RANGE = (4.0, 30.0)
# The two carriageways' lengths may differ by this share at most.
MAX_LENGTH_DIFFERENCE = 0.3
# The common axis is taken straight when it bows less than this from the line between the end nodes.
MAX_STRAIGHT_BOW = 3.0
AXIS_SMOOTHING_PASSES = 20

_synthetic_ids = itertools.count(-(10 ** 15), -1)


def straighten(ways, sections: dict) -> list:
    """The ways with every short dual carriageway section redrawn parallel (new Way objects for changed ways)."""
    network = SegmentNetwork(ways, sections)
    moved_nodes = {}       # node id -> new (x, y)
    added_points = {}      # (node id, next node id) -> [(synthetic id, (x, y))] between them, in that order
    for node in list(network.ends_at):
        if network.node_kind(node) != NodeKind.JUNCTION:
            continue
        split = find_split(network, node)
        if split is None:
            continue
        section = _section(network, split.incoming, split.outgoing)
        if section is not None:
            _redraw(network, section, moved_nodes, added_points)
    return [_rebuilt(way, moved_nodes, added_points) for way in ways]


def _section(network: SegmentNetwork, first: SegmentEnd, second: SegmentEnd):
    """(chain, chain) of the two carriageways from the split to the node where they meet again, each a list of
    (segment, leaves along its node order), or None when they don't meet within MAX_SECTION_LENGTH."""
    first_chain, first_nodes = _walk(network, first)
    second_chain, second_nodes = _walk(network, second)
    common = [node for node in first_nodes if node in second_nodes]
    if not common:
        return None
    meeting = common[0]
    first_chain = first_chain[:first_nodes.index(meeting) + 1]
    second_chain = second_chain[:second_nodes.index(meeting) + 1]
    return first_chain, second_chain


def _walk(network: SegmentNetwork, start: SegmentEnd):
    """The one-way segments straight on from a segment end, and the node each of them ends at."""
    chain, nodes = [], []
    end = start
    travelled = 0.0
    while end is not None and travelled < MAX_SECTION_LENGTH:
        segment = network.segments[end.segment]
        if not osm_tags.is_oneway(segment.way.tags) or any(item[0] is segment for item in chain):
            break
        chain.append((segment, end.at_start))
        far_end = SegmentEnd(segment.id, not end.at_start)
        nodes.append(network.node_of(far_end))
        travelled += segment.length()
        if network.node_kind(nodes[-1]) == NodeKind.CONTINUATION:
            end = network.other_end(far_end)
        else:
            end = corrections.straight_neighbour(network, far_end)
    return chain, nodes


def _chain_line(chain: list) -> np.ndarray:
    """The chain's centre line from the split onward, and the node ids along it."""
    points = []
    for segment, along in chain:
        xy = segment.xy if along else segment.xy[::-1]
        points.extend(xy if not points else xy[1:])
    return np.array(points)


def _chain_nodes(chain: list) -> list:
    """The node ids along the chain from the split, matching _chain_line's points."""
    nodes = []
    for segment, along in chain:
        ids = _segment_node_ids(segment)
        if not along:
            ids = ids[::-1]
        nodes.extend(ids if not nodes else ids[1:])
    return nodes


def _segment_node_ids(segment) -> list:
    """The way's node ids of a segment's points."""
    ids = segment.way.node_ids
    start = ids.index(segment.start_node)
    return list(ids[start:start + len(segment.xy)])


def _redraw(network: SegmentNetwork, section: tuple, moved_nodes: dict, added_points: dict):
    """Moves the nodes of both carriageways of a section onto lines parallel to their common axis."""
    first_chain, second_chain = section
    first, second = _chain_line(first_chain), _chain_line(second_chain)
    first_length, second_length = polyline.length(first), polyline.length(second)
    if abs(first_length - second_length) > MAX_LENGTH_DIFFERENCE * max(first_length, second_length):
        return
    samples = max(int(max(first_length, second_length) / POINT_SPACING), 4) + 1
    fractions = np.linspace(0.0, 1.0, samples)
    first_points = _at_fractions(first, fractions)
    second_points = _at_fractions(second, fractions)
    middle = slice(samples // 4, max(3 * samples // 4, samples // 4 + 1))
    separation = float(np.median(np.hypot(*(first_points[middle] - second_points[middle]).T)))
    low, high = SEPARATION_RANGE
    if not low <= separation <= high:
        return
    axis = _axis((first_points + second_points) / 2)
    length = polyline.length(axis)
    start_ramp = _gore_length(first_chain[0][0], separation, length)
    end_ramp = start_ramp
    if find_split(network, network.node_of(_last_end(first_chain))) is None:
        end_ramp = min(assumptions.DUAL_SECTION_JUNCTION_RAMP, length / 2)
    along = fractions * length
    opening = _opening(along, length, start_ramp, end_ramp) * separation / 2
    normals = np.array([polyline.right_of(polyline.direction_at(axis, s)) for s in along])
    side = float(np.sign(np.dot(first_points[samples // 2] - axis[samples // 2], normals[samples // 2]))) or 1.0
    for chain, sign in ((first_chain, side), (second_chain, -side)):
        new_line = axis + normals * (sign * opening)[:, None]
        _place_nodes(chain, new_line, moved_nodes, added_points)


def _last_end(chain: list) -> SegmentEnd:
    segment, along = chain[-1]
    return SegmentEnd(segment.id, not along)


def _at_fractions(xy, fractions) -> np.ndarray:
    """Points at fractions of a polyline's length."""
    return np.array([polyline.point_at(xy, fraction * polyline.length(xy)) for fraction in fractions])


def _axis(middle_points) -> np.ndarray:
    """The common axis: straight between the end nodes when the middle line hardly bows, else the middle line
    smoothed with its ends kept."""
    start, end = middle_points[0], middle_points[-1]
    chord = np.linspace(0.0, 1.0, len(middle_points))[:, None] * (end - start) + start
    if polyline.distances_to(np.array([start, end]), middle_points).max() <= MAX_STRAIGHT_BOW:
        return chord
    smoothed = middle_points.copy()
    for _ in range(AXIS_SMOOTHING_PASSES):
        smoothed[1:-1] = 0.25 * smoothed[:-2] + 0.5 * smoothed[1:-1] + 0.25 * smoothed[2:]
    return smoothed


def _gore_length(segment, separation: float, length: float) -> float:
    """How long the carriageways take to open from the split to their full distance (a taper of the road's speed)."""
    speed = osm_tags.speed_limit(segment.way.tags) or assumptions.DEFAULT_SPEED_URBAN
    taper = line_rules.taper_length(separation / 2, speed, urban=True)
    return min(taper, length * assumptions.TAPER_MAX_SEGMENT_SHARE)


def _opening(along, length: float, start_ramp: float, end_ramp: float) -> np.ndarray:
    """0 at both end nodes, 1 in between, with smooth ramps of the given lengths."""
    def ramp(distance, ramp_length):
        fraction = np.clip(distance / max(ramp_length, 1e-6), 0.0, 1.0)
        return fraction * fraction * (3.0 - 2.0 * fraction)
    return np.minimum(ramp(along, start_ramp), ramp(length - along, end_ramp))


def _place_nodes(chain: list, new_line, moved_nodes: dict, added_points: dict):
    """Moves the chain's nodes to the same share of the redrawn line and adds shape points between them."""
    old_line = _chain_line(chain)
    nodes = _chain_nodes(chain)
    old_along = polyline.arclength(old_line)
    new_along = polyline.arclength(new_line)
    targets = old_along / max(old_along[-1], 1e-6) * new_along[-1]
    for index, node in enumerate(nodes):
        if 0 < index < len(nodes) - 1:
            moved_nodes[node] = polyline.point_at(new_line, targets[index])
    for index in range(len(nodes) - 1):
        start, end = targets[index], targets[index + 1]
        count = int((end - start) / POINT_SPACING)
        points = [(next(_synthetic_ids), polyline.point_at(new_line, start + (end - start) * step / (count + 1)))
                  for step in range(1, count + 1)]
        added_points[(nodes[index], nodes[index + 1])] = points
        added_points[(nodes[index + 1], nodes[index])] = points[::-1]


def _rebuilt(way, moved_nodes: dict, added_points: dict):
    """The way with moved nodes and added shape points, or the way itself when nothing changed."""
    touched = any(node in moved_nodes for node in way.node_ids) or any(
        (first, second) in added_points for first, second in zip(way.node_ids, way.node_ids[1:]))
    if not touched:
        return way
    node_ids, xy = [], []
    for index, node in enumerate(way.node_ids):
        if index > 0:
            for synthetic_id, point in added_points.get((way.node_ids[index - 1], node), []):
                node_ids.append(synthetic_id)
                xy.append(point)
        node_ids.append(node)
        xy.append(moved_nodes.get(node, way.xy[index]))
    return dataclasses.replace(way, node_ids=node_ids, xy=np.array(xy, dtype=np.float64))
