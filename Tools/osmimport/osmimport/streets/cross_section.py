"""A road's cross-section: the strips of its carriageway from the left kerb to the right one, and so its width.

The strips are, from left to right along the way's node order: a margin (gutter or shoulder strip), the left side's
bus and cycle lanes, the travel lanes, the right side's cycle and bus lanes, and a margin. Travel lanes on the left
half of a two-way road carry traffic against the node order (right-hand traffic).

Sources in order of trust: a width= tag where mappers measure kerb to kerb, then the lane tags (lanes, turn:lanes,
lanes:forward, cycleway, busway, lane_markings), then the assumptions for the road class and its surroundings.
"""
from dataclasses import dataclass
from enum import Enum

from . import assumptions, tags as osm_tags


class StripKind(Enum):
    TRAVEL_LANE = "travel_lane"
    CYCLE_LANE = "cycle_lane"
    BUS_LANE = "bus_lane"
    MARGIN = "margin"


class Travel(Enum):
    """Which way traffic moves on a strip, relative to the way's node order."""
    FORWARD = "forward"
    BACKWARD = "backward"
    BOTH = "both"    # a shared lane on a narrow two-way road
    NONE = "none"    # margins


class WidthSource(Enum):
    WIDTH_TAG = "width tag"
    ASSUMED = "assumed"


@dataclass(frozen=True)
class RoadContext:
    """What the surroundings say about a road, found by the geometry code: urban when buildings line it."""
    urban: bool


@dataclass(frozen=True)
class Strip:
    kind: StripKind
    width: float
    travel: Travel


@dataclass(frozen=True)
class CrossSection:
    strips: tuple          # Strip, from the left kerb to the right one
    marked: bool           # lane lines are painted between the travel lanes
    width_source: WidthSource

    def width(self) -> float:
        """The carriageway width from kerb to kerb."""
        return sum(strip.width for strip in self.strips)

    def travel_lanes(self) -> list:
        """The travel lane strips, from left to right."""
        return [strip for strip in self.strips if strip.kind == StripKind.TRAVEL_LANE]

    def lane_count(self) -> int:
        """The number of travel lanes, both directions together (an unmarked two-way road has two)."""
        return len(self.travel_lanes())

    def strip_edges(self) -> list:
        """(strip, left edge, right edge) of every strip, as offsets from the way's centre line, which runs down the
        middle of the carriageway; positive is to the right."""
        edges = []
        left = -self.width() / 2
        for strip in self.strips:
            edges.append((strip, left, left + strip.width))
            left += strip.width
        return edges

    def lane_dividers(self) -> list:
        """Offsets of the lines between neighbouring travel lanes, the centre line of a two-way road among them."""
        dividers = []
        edges = self.strip_edges()
        for (strip, _, right), (neighbour, _, _) in zip(edges, edges[1:]):
            if strip.kind == StripKind.TRAVEL_LANE and neighbour.kind == StripKind.TRAVEL_LANE:
                dividers.append(right)
        return dividers

    def side_strip_centre(self, kind: StripKind, side: str):
        """Distance from the centre line to the middle of the strip of a kind (cycle or bus lane) on one side
        ("left" or "right"), or None when that side has none."""
        for strip, left, right in self.strip_edges():
            centre = (left + right) / 2
            if strip.kind != kind:
                continue
            if side == "right" and centre > 0:
                return centre
            if side == "left" and centre < 0:
                return -centre
        return None

    def describe(self) -> str:
        """The strips from left to right in words, for the viewer's pop-ups."""
        arrows = {Travel.FORWARD: " →", Travel.BACKWARD: " ←", Travel.BOTH: " ↔", Travel.NONE: ""}
        return " | ".join(f"{strip.kind.value.replace('_', ' ')}{arrows[strip.travel]} {strip.width:.2f}"
                          for strip in self.strips)

    def kerb_lane_centre(self, travel: Travel) -> float:
        """Distance from the centre line to the middle of the travel lane nearest the kerb on the right of traffic
        going one way (FORWARD or BACKWARD); 0 when no lane carries that way on its own (a shared lane)."""
        centres = [(left + right) / 2 for strip, left, right in self.strip_edges()
                   if strip.kind == StripKind.TRAVEL_LANE and strip.travel == travel]
        if not centres:
            return 0.0
        if travel == Travel.FORWARD:
            return max(centres)
        return -min(centres)


def cross_section(tags, context: RoadContext) -> CrossSection:
    """The cross-section of a road from its tags and surroundings."""
    marked = _is_marked(tags)
    lanes = _travel_lanes(tags, context, marked)
    margin = Strip(StripKind.MARGIN, _by_context(assumptions.MARGIN_WIDTH, context), Travel.NONE)
    left_side = _side_strips(tags, "left")
    right_side = _side_strips(tags, "right")
    strips = [margin] + list(reversed(left_side)) + lanes + right_side + [margin]
    section = CrossSection(tuple(strips), marked, WidthSource.ASSUMED)
    return _fitted_to_width_tag(section, tags)


def _by_context(urban_and_rural: tuple, context: RoadContext) -> float:
    """The urban or the rural value of an (urban, rural) pair."""
    urban, rural = urban_and_rural
    if context.urban:
        return urban
    return rural


def _is_marked(tags) -> bool:
    """True when lane lines are painted: lane_markings says so, two or more lanes are tagged, or the class is."""
    painted = osm_tags.lane_markings(tags)
    if painted is not None:
        return painted
    tagged = osm_tags.tagged_lane_count(tags)
    if tagged is not None and tagged >= 2:
        return True
    return osm_tags.base_class(tags) in assumptions.MARKED_BY_DEFAULT


def lane_count(tags) -> int:
    """The number of travel lanes for motor traffic from the tags, capped at what the road class plausibly has, or
    the default."""
    oneway = osm_tags.is_oneway(tags)
    tagged = osm_tags.tagged_lane_count(tags)
    if tagged is None:
        if oneway:
            return assumptions.DEFAULT_LANES_ONE_WAY
        return assumptions.DEFAULT_LANES_TWO_WAY
    motor_lanes = max(tagged - osm_tags.non_motor_lanes_counted(tags), 1)
    per_direction = assumptions.MAX_LANES_PER_DIRECTION.get(osm_tags.base_class(tags),
                                                            assumptions.MAX_LANES_PER_DIRECTION_OTHER)
    if oneway:
        return min(motor_lanes, per_direction)
    return min(motor_lanes, 2 * per_direction)


def _lane_directions(tags, count: int) -> list:
    """The travel direction of each of count lanes, from left to right."""
    if osm_tags.is_reversed_oneway(tags):
        return [Travel.BACKWARD] * count
    if osm_tags.is_oneway(tags):
        return [Travel.FORWARD] * count
    if count == 1:
        return [Travel.BOTH]
    backward = count // 2
    by_direction = osm_tags.tagged_lanes_by_direction(tags)
    if by_direction is not None and sum(by_direction) == count:
        backward = by_direction[1]
    return [Travel.BACKWARD] * backward + [Travel.FORWARD] * (count - backward)


def _travel_lanes(tags, context: RoadContext, marked: bool) -> list:
    """The travel lane strips from left to right: marked lanes of the class's lane width, or an unmarked carriageway
    of the class's width split between the directions."""
    count = lane_count(tags)
    if osm_tags.is_link(tags) and osm_tags.is_oneway(tags) and count == 1:
        return [Strip(StripKind.TRAVEL_LANE, assumptions.SINGLE_LANE_LINK_WIDTH, _lane_directions(tags, 1)[0])]
    if marked:
        lane_width = _by_context(assumptions.MARKED_LANE_WIDTH.get(osm_tags.base_class(tags),
                                                                   assumptions.MARKED_LANE_WIDTH_OTHER), context)
        return [Strip(StripKind.TRAVEL_LANE, lane_width, travel) for travel in _lane_directions(tags, count)]
    width = _unmarked_width(tags, context)
    if not osm_tags.is_oneway(tags) and width < assumptions.SHARED_LANE_MAX_WIDTH:
        count = 1
    directions = _lane_directions(tags, count)
    return [Strip(StripKind.TRAVEL_LANE, width / len(directions), travel) for travel in directions]


def _unmarked_width(tags, context: RoadContext) -> float:
    """The width of the travel lanes together on a carriageway without lane lines."""
    road_class = osm_tags.base_class(tags)
    oneway = osm_tags.is_oneway(tags)
    if osm_tags.is_bus_road(tags):
        return assumptions.BUS_ROAD_LANE_WIDTH * lane_count(tags)
    if road_class == "service" and tags.get("service") in assumptions.SERVICE_WIDTH:
        two_way, one_way = assumptions.SERVICE_WIDTH[tags["service"]]
        if oneway:
            return one_way
        return two_way
    if oneway:
        return assumptions.ONE_WAY_WIDTH.get(road_class, assumptions.ONE_WAY_WIDTH_OTHER)
    speed = osm_tags.speed_limit(tags)
    if road_class == "residential" and speed is not None and speed >= assumptions.COLLECTOR_STREET_SPEED:
        return assumptions.COLLECTOR_STREET_WIDTH
    return _by_context(assumptions.UNMARKED_TWO_WAY_WIDTH.get(road_class, assumptions.UNMARKED_TWO_WAY_WIDTH_OTHER),
                       context)


def _side_travel(side: str) -> Travel:
    """The direction of a lane at one side of the carriageway, in right-hand traffic: against the node order on the
    left, along it on the right. On a one-way street one of them is a contraflow lane (cycleway=opposite_lane)."""
    if side == "left":
        return Travel.BACKWARD
    return Travel.FORWARD


def _side_strips(tags, side: str) -> list:
    """The bus and cycle lanes at one side, from the travel lanes outward: the cycle lane runs at the kerb."""
    strips = []
    travel = _side_travel(side)
    if side in osm_tags.bus_lane_sides(tags):
        strips.append(Strip(StripKind.BUS_LANE, assumptions.BUS_LANE_WIDTH, travel))
    cycleway = osm_tags.cycleway_sides(tags).get(side)
    if cycleway in osm_tags.CYCLE_LANE_VALUES:
        width = assumptions.CYCLE_LANE_WIDTH
        if osm_tags.cycle_lane_is_advisory(tags, side):
            width = assumptions.ADVISORY_CYCLE_LANE_WIDTH
        strips.append(Strip(StripKind.CYCLE_LANE, width, travel))
    return strips


def _tagged_width(tags):
    """The width= tag when it is a plausible carriageway width, else None."""
    width = osm_tags.number(tags.get("width"))
    low, high = assumptions.WIDTH_TAG_RANGE
    if width is None or not low <= width <= high:
        return None
    return width


def _fitted_to_width_tag(section: CrossSection, tags) -> CrossSection:
    """The cross-section with its travel lanes widened or narrowed to match a trusted width= tag.

    On classes whose width tags measure less than the kerbs (assumptions.WIDTH_TAG_TRUSTED_CLASSES) the tag can only
    widen the road, and a tag far below the assumption is taken to measure one lane and ignored."""
    target = _tagged_width(tags)
    if target is None:
        return section
    difference = target - section.width()
    trusted = osm_tags.highway(tags) in assumptions.WIDTH_TAG_TRUSTED_CLASSES
    if difference < 0 and not trusted:
        return section
    if difference < -assumptions.WIDTH_TAG_LANE_ONLY_GAP:
        return section
    change_per_lane = difference / section.lane_count()
    strips = []
    for strip in section.strips:
        if strip.kind == StripKind.TRAVEL_LANE:
            width = max(strip.width + change_per_lane, assumptions.MIN_TRAVEL_LANE_WIDTH)
            strip = Strip(strip.kind, width, strip.travel)
        strips.append(strip)
    return CrossSection(tuple(strips), section.marked, WidthSource.WIDTH_TAG)
