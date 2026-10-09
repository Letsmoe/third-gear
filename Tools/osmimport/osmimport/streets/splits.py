"""Dual carriageway splits: where a two-way road divides into two one-way carriageways, or two join into one.

OSM draws the two carriageways as one-way ways meeting the two-way road at one node. The one-way carriageway whose
traffic drives into the two-way road continues that road's lanes on one side, the one whose traffic comes out of it
the lanes on the other side; the centre line opens into the inner edges of the two carriageways.
"""
from dataclasses import dataclass

from . import assumptions, corrections, tags as osm_tags
from .network import SegmentEnd, SegmentNetwork


@dataclass(frozen=True)
class Split:
    two_way: SegmentEnd
    incoming: SegmentEnd     # the carriageway whose traffic comes to the node and drives on along the two-way road
    outgoing: SegmentEnd     # the carriageway whose traffic comes from the two-way road and leaves the node
    gore_length: float       # how far from the node the two carriageways have separated


def find_split(network: SegmentNetwork, node):
    """The split at a junction node, or None when the node is not one: exactly one two-way road and two one-way
    carriageways (one in, one out) carrying it on nearly straight; driveways and the like may join as well."""
    ends = [end for end in network.ends_at[node] if not corrections.is_minor(network, end)]
    if len(ends) != 3:
        return None
    two_ways = [end for end in ends if not _is_one_way(network, end)]
    one_ways = [end for end in ends if _is_one_way(network, end)]
    if len(two_ways) != 1 or len(one_ways) != 2:
        return None
    two_way = two_ways[0]
    incoming = [end for end in one_ways if not _leaves_node(network, end)]
    outgoing = [end for end in one_ways if _leaves_node(network, end)]
    if len(incoming) != 1 or len(outgoing) != 1:
        return None
    for one_way in one_ways:
        if corrections.deflection(network, two_way, one_way) > assumptions.SPLIT_MAX_DEFLECTION:
            return None
    gore = max(network.clear_distance(incoming[0], outgoing), network.clear_distance(outgoing[0], incoming),
               assumptions.TAPER_MIN_LENGTH)
    return Split(two_way, incoming[0], outgoing[0], gore)


def _is_one_way(network: SegmentNetwork, end: SegmentEnd) -> bool:
    return osm_tags.is_oneway(network.segments[end.segment].way.tags)


def _leaves_node(network: SegmentNetwork, end: SegmentEnd) -> bool:
    """True when the one-way traffic of the arm drives away from the node."""
    forward = not osm_tags.is_reversed_oneway(network.segments[end.segment].way.tags)
    return end.at_start == forward
