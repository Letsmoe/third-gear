"""Reading the OSM tags the street model uses: plain functions over a way's tag dictionary.

Sides are relative to the way's node order: left and right as seen when walking from its first node to its last.
"""

ONEWAY_VALUES = {"yes", "1", "true", "-1"}
ROUNDABOUT_VALUES = {"roundabout", "circular"}
ALWAYS_ONEWAY_CLASSES = {"motorway", "motorway_link"}
CYCLE_LANE_VALUES = {"lane", "opposite_lane"}
BUS_LANE_VALUES = {"lane", "opposite_lane"}
# Speed limits written as a zone instead of a number (maxspeed=DE:urban and the like), in km/h.
ZONE_SPEED_LIMITS = {"urban": 50.0, "rural": 100.0, "zone30": 30.0, "zone:30": 30.0, "living_street": 7.0,
                     "motorway": 130.0, "bicycle_road": 30.0}
MILES_TO_KILOMETRES = 1.609344


def number(value, default=None):
    """The number at the start of a tag value ("5.5", "5,5 m", "4;6"), or default when there is none."""
    if value is None:
        return default
    text = str(value).split(";")[0].replace(",", ".").replace("m", "").strip()
    try:
        return float(text)
    except ValueError:
        return default


def highway(tags) -> str:
    """The road class, such as "secondary" or "secondary_link"."""
    return str(tags.get("highway", ""))


def base_class(tags) -> str:
    """The road class without the _link of slip roads: "secondary" for both secondary and secondary_link."""
    return highway(tags).removesuffix("_link")


def is_link(tags) -> bool:
    """True for slip roads and junction connectors (highway=*_link)."""
    return highway(tags).endswith("_link")


def is_oneway(tags) -> bool:
    """True when traffic only goes one way: tagged one-way, roundabouts and motorways."""
    if tags.get("oneway") in ONEWAY_VALUES:
        return True
    if tags.get("junction") in ROUNDABOUT_VALUES:
        return True
    return highway(tags) in ALWAYS_ONEWAY_CLASSES


def is_reversed_oneway(tags) -> bool:
    """True for oneway=-1: traffic goes against the node order."""
    return tags.get("oneway") == "-1"


def _lane_list_count(tags, key):
    """How many lanes a lane list such as turn:lanes="left|through|right" lists, or None when it is not tagged."""
    value = tags.get(key)
    if not value:
        return None
    return len(str(value).split("|"))


def turn_lane_count(tags):
    """How many lanes the turn:lanes tags list (both directions on a two-way road), or None when they are incomplete.

    turn:lanes lists every lane, so it is rarely wrong where lanes= is: it wins when both are there."""
    if is_oneway(tags):
        return _lane_list_count(tags, "turn:lanes")
    forward = _lane_list_count(tags, "turn:lanes:forward")
    backward = _lane_list_count(tags, "turn:lanes:backward")
    if forward is None or backward is None:
        return None
    return forward + backward


def tagged_lane_count(tags):
    """The number of travel lanes from the tags (turn:lanes, then lanes), or None when neither says."""
    from_turn_lanes = turn_lane_count(tags)
    if from_turn_lanes:
        return from_turn_lanes
    lanes = number(tags.get("lanes"))
    if not lanes or lanes < 1:
        return None
    return int(lanes)


def _lane_values(tags, key: str, suffix: str) -> list:
    """The per-lane values of a lane list such as bicycle:lanes=no|no|designated, or [] when it is not tagged."""
    value = tags.get(f"{key}:lanes{suffix}")
    if not value:
        return []
    return str(value).split("|")


def _non_motor_lanes(tags, suffix: str) -> int:
    """How many lanes of one lane list (suffix "", ":forward" or ":backward") are for bicycles or buses only."""
    bicycle = _lane_values(tags, "bicycle", suffix)
    vehicle = _lane_values(tags, "vehicle", suffix) or _lane_values(tags, "motor_vehicle", suffix)
    bus = _lane_values(tags, "bus", suffix) or _lane_values(tags, "psv", suffix)
    count = 0
    for index in range(max(len(bicycle), len(vehicle), len(bus))):
        cycle_only = _at(bicycle, index) == "designated" and _at(vehicle, index) == "no"
        bus_only = _at(bus, index) == "designated"
        if cycle_only or bus_only:
            count += 1
    return count


def _at(values: list, index: int) -> str:
    """The value at an index of a lane list, or "" past its end."""
    if index < len(values):
        return values[index]
    return ""


def non_motor_lanes_counted(tags) -> int:
    """How many of the lanes= lanes are cycle or bus lanes. Some mappers count a painted cycle or bus lane in
    lanes= and say which one it is in the per-lane lists (vehicle:lanes=yes|yes|no, bicycle:lanes=no|no|designated);
    those lanes are already in the cycleway= and busway= tags and must not count as travel lanes."""
    if is_oneway(tags):
        return _non_motor_lanes(tags, "")
    return _non_motor_lanes(tags, ":forward") + _non_motor_lanes(tags, ":backward")


def tagged_lanes_by_direction(tags):
    """(forward, backward) lane counts from lanes:forward and lanes:backward, or None when not both are tagged."""
    forward = number(tags.get("lanes:forward"))
    backward = number(tags.get("lanes:backward"))
    if forward is None or backward is None:
        return None
    return int(forward), int(backward)


def is_bus_road(tags) -> bool:
    """True for a road only buses may use: bus=yes or psv=yes, with general traffic (vehicle, motor_vehicle) banned."""
    buses_allowed = tags.get("bus") in {"yes", "designated"} or tags.get("psv") in {"yes", "designated"}
    others_banned = tags.get("vehicle") == "no" or tags.get("motor_vehicle") == "no" or tags.get("access") == "no"
    return buses_allowed and others_banned


def is_zone30(tags) -> bool:
    """True inside a Tempo 30 zone (Zeichen 274.1), however the zone is tagged."""
    for key in ("maxspeed:type", "source:maxspeed", "zone:maxspeed", "zone:traffic"):
        value = str(tags.get(key, "")).lower()
        if "zone30" in value or value.endswith(":30") or "zone:30" in value:
            return True
    return False


def lane_markings(tags):
    """True or False when lane_markings says whether lanes are painted, None when it is not tagged."""
    value = tags.get("lane_markings")
    if value == "yes":
        return True
    if value == "no":
        return False
    return None


def speed_limit(tags):
    """The speed limit in km/h, or None when maxspeed is missing or unreadable ("none" on motorways is None too)."""
    value = str(tags.get("maxspeed", "")).strip()
    if not value:
        return None
    zone = value.split(":")[-1]  # DE:urban, DE:zone30
    if zone in ZONE_SPEED_LIMITS:
        return ZONE_SPEED_LIMITS[zone]
    if value.endswith("mph"):
        miles = number(value.removesuffix("mph"))
        if miles is None:
            return None
        return miles * MILES_TO_KILOMETRES
    return number(value)


def side_values(tags, key: str) -> dict:
    """What a sided tag such as cycleway or busway says per side, as {"left": value, "right": value}; a side
    without a value is missing. key= alone means both sides, except on a one-way street, where it means the right
    side (the side traffic keeps to)."""
    sides = {}
    both = tags.get(f"{key}:both") or tags.get(key)
    if both:
        sides = {"left": both, "right": both}
        if tags.get(key) and is_oneway(tags) and f"{key}:both" not in tags:
            sides = {"right": both}
    for side in ("left", "right"):
        value = tags.get(f"{key}:{side}")
        if value:
            sides[side] = value
    return sides


def cycleway_sides(tags) -> dict:
    """The cycleway tagged on a road per side (lane, track, separate, no...)."""
    return side_values(tags, "cycleway")


def cycle_lane_is_advisory(tags, side: str) -> bool:
    """True for an advisory cycle lane (Schutzstreifen, dashed line) rather than an exclusive one (solid line)."""
    for key in (f"cycleway:{side}:lane", "cycleway:both:lane", "cycleway:lane"):
        if key in tags:
            return tags[key] == "advisory"
    return False


def bus_lane_sides(tags) -> list:
    """The sides ("left", "right") with a bus lane on the carriageway."""
    return [side for side, value in side_values(tags, "busway").items() if value in BUS_LANE_VALUES]


def turn_lanes(tags, travel) -> list:
    """The turn:lanes values of the lanes going one way along the way (travel: cross_section.Travel FORWARD or
    BACKWARD), from the driver's left; [] when not tagged."""
    if is_oneway(tags):
        forward_is_travel = not is_reversed_oneway(tags)
        if (travel.value == "forward") != forward_is_travel:
            return []
        value = tags.get("turn:lanes", "")
    elif travel.value == "forward":
        value = tags.get("turn:lanes:forward", "")
    else:
        value = tags.get("turn:lanes:backward", "")
    if not value:
        return []
    return str(value).split("|")
