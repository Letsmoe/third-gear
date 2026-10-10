"""Bridges (#106), first part: the common types, generated to fit their span and the road on them.

Frame: metres, Z up. A bridge runs along +Y from y = 0 to its full length, centred on x = 0, with the road surface at
z = 0; everything else hangs below. The full length covers the wing walls at both ends, so it is what the OSM bridge
way should measure: for a road bridge it is the clear span plus 2 * (ABUTMENT_THICKNESS + WING_LENGTH).

The deck is an extrusion of a cross-section (deck_section), so the runtime can sweep it along a curved, sloping way
instead of using the straight samples here. Railing posts, abutments and piers are placed along it.

Types:
- slab bridge: concrete slab on two abutments, for clear spans up to about 25 m (ditches, canals, small roads);
- beam bridge: a concrete box girder on round piers, for long or multi-span crossings (overpasses);
- steel footbridge: two steel girders under a plank deck, on small bank seats;
- timber footbridge: the Vierlande plank bridge on timber piles.
"""

import math
from collections import OrderedDict

from .geom import Mesh

ASPHALT = 0.08  # surfacing on the deck; the street model's road lies on top of the slab at z = -ASPHALT
KERB = 0.15
CAP_OVERHANG = 0.1
CAP_FASCIA = 0.35  # how far the cap's outer face reaches below the slab top
ABUTMENT_THICKNESS = 1.0
WING_LENGTH = 3.0
WING_THICKNESS = 0.5
RAILING_BAY = 2.0


class RoadBridge:
    """The cross-section and span of one road bridge."""

    def __init__(self, carriageway, footway, clear_spans, height, railing="infill", guard_rail=False):
        self.carriageway = carriageway  # kerb to kerb
        self.footway = footway  # walkable width on each cap; 0 for a narrow safety cap
        self.clear_spans = clear_spans  # list of spans between supports, from one abutment to the next
        self.height = height  # from the road surface down to the ground or water level in front of the abutments
        self.railing = railing  # "infill" (vertical bars) or "rail" (posts and two rails)
        self.guard_rail = guard_rail

    def cap_width(self):
        """Width of each edge cap from the kerb to its outer face."""
        return max(self.footway, 0.5) + 0.25

    def half_width(self):
        """Distance from the axis to the cap's outer face."""
        return self.carriageway / 2.0 + self.cap_width()

    def deck_depth(self):
        """Slab or girder depth from the longest span: span / 18 for slabs, span / 20 for girders, at least 0.45 m."""
        longest = max(self.clear_spans)
        if len(self.clear_spans) == 1:
            return max(0.45, longest / 18.0)
        return max(0.8, longest / 20.0)

    def length(self):
        """Full length along the way, wing walls included."""
        pier_widths = 1.2 * (len(self.clear_spans) - 1)
        return sum(self.clear_spans) + pier_widths + 2 * (ABUTMENT_THICKNESS + WING_LENGTH)


# ================================================================================================= deck
def cap_section(bridge, side):
    """The edge cap in (x, z): the kerb face at the carriageway's edge, the walkway, the overhang and the fascia that
    hides the slab edge. side is -1 for the left cap, +1 for the right one."""
    inner = bridge.carriageway / 2.0
    outer = bridge.half_width()
    slab_edge = outer - CAP_OVERHANG
    points = [(inner, -ASPHALT), (slab_edge, -ASPHALT), (slab_edge, -CAP_FASCIA), (outer, -CAP_FASCIA),
              (outer, KERB + 0.02), (inner, KERB)]
    return [(side * x, z) for x, z in points]


def slab_section(bridge):
    """The slab in (x, z), under the surfacing and both caps."""
    edge = bridge.half_width() - CAP_OVERHANG
    bottom = -ASPHALT - bridge.deck_depth()
    return [(-edge, bottom), (edge, bottom), (edge, -ASPHALT), (-edge, -ASPHALT)]


def girder_section(bridge):
    """A box girder in (x, z): cantilevered slab over a narrower box with sloping webs."""
    edge = bridge.half_width() - CAP_OVERHANG
    bottom = -ASPHALT - bridge.deck_depth()
    web_top = edge * 0.55
    web_bottom = edge * 0.42
    cantilever = -ASPHALT - 0.35
    return [(-web_bottom, bottom), (web_bottom, bottom), (web_top, cantilever), (edge, cantilever + 0.1),
            (edge, -ASPHALT), (-edge, -ASPHALT), (-edge, cantilever + 0.1), (-web_top, cantilever)]


def deck_section(bridge):
    """The parts of the cross-section the runtime sweeps along the way: [(polygon, material), ...]."""
    body = slab_section(bridge) if len(bridge.clear_spans) == 1 else girder_section(bridge)
    return [(body, "Concrete"), (cap_section(bridge, -1.0), "Concrete"), (cap_section(bridge, 1.0), "Concrete")]


def sweep_section(mesh, section, start, end):
    """Extrudes each polygon of a section straight along Y between start and end."""
    for polygon, material in section:
        mesh.prism(polygon, "xz", start, end, material)


# ================================================================================================= railings
def add_infill_railing(mesh, x, start, end, height):
    """The standard bridge railing: posts every two metres, a round handrail, a bottom rail and vertical bars."""
    bottom = KERB + 0.02
    bays = max(1, round((end - start) / RAILING_BAY))
    for bay in range(bays + 1):
        y = start + (end - start) * bay / bays
        mesh.box(x - 0.04, x + 0.04, y - 0.008, y + 0.008, bottom, bottom + height, "Metal")
    mesh.tube((x, start, bottom + height + 0.02), (x, end, bottom + height + 0.02), 0.03, "Metal", segments=8)
    mesh.box(x - 0.025, x + 0.025, start, end, bottom + 0.08, bottom + 0.12, "Metal")
    mesh.box(x - 0.025, x + 0.025, start, end, bottom + height - 0.06, bottom + height - 0.02, "Metal")
    bar_count = int((end - start) / 0.13)
    for bar in range(1, bar_count):
        y = start + (end - start) * bar / bar_count
        mesh.box(x - 0.01, x + 0.01, y - 0.01, y + 0.01, bottom + 0.12, bottom + height - 0.06, "Metal")


def add_rail_railing(mesh, x, start, end, height):
    """The plain rural railing: posts every two metres with a handrail and a knee rail."""
    bottom = KERB + 0.02
    bays = max(1, round((end - start) / RAILING_BAY))
    for bay in range(bays + 1):
        y = start + (end - start) * bay / bays
        mesh.cylinder(x, y, bottom, bottom + height, 0.03, "Metal", segments=8)
    for rail_height in (height, height * 0.5):
        mesh.tube((x, start, bottom + rail_height), (x, end, bottom + rail_height), 0.03, "Metal", segments=8)


def add_guard_rail(mesh, x, side, start, end):
    """A steel guard rail on the cap facing the traffic: posts every two metres and the corrugated beam."""
    bays = max(1, round((end - start) / RAILING_BAY))
    for bay in range(bays + 1):
        y = start + (end - start) * bay / bays
        mesh.box(x - 0.05, x + 0.05, y - 0.05, y + 0.05, KERB, KERB + 0.75, "Metal")
    face = x - side * 0.08
    for z, depth in ((KERB + 0.5, 0.06), (KERB + 0.6, 0.02), (KERB + 0.7, 0.06)):
        mesh.box(min(face, face - side * depth), max(face, face - side * depth), start, end, z - 0.05, z + 0.05,
                 "Metal")


def add_railings(mesh, bridge, start, end):
    """Railings along both caps' outer edges, and guard rails at their kerbs where the bridge has them."""
    for side in (-1.0, 1.0):
        x = side * (bridge.half_width() - 0.12)
        if bridge.railing == "infill":
            add_infill_railing(mesh, x, start, end, 1.0 if bridge.footway < 1.5 else 1.3)
        else:
            add_rail_railing(mesh, x, start, end, 1.0)
        if bridge.guard_rail:
            add_guard_rail(mesh, side * (bridge.carriageway / 2.0 + 0.4), side, start, end)


# ================================================================================================= supports
def add_abutment(mesh, bridge, front_y, facing):
    """An abutment: the front wall from the ground up to the bearing shelf, bearings under the deck, and parallel wing
    walls running back into the embankment. facing is +1 when the span lies toward +Y."""
    edge = bridge.half_width() - CAP_OVERHANG
    deck_bottom = -ASPHALT - bridge.deck_depth()
    back_y = front_y - facing * ABUTMENT_THICKNESS
    low, high = min(front_y, back_y), max(front_y, back_y)
    mesh.box(-edge, edge, low, high, -bridge.height - 0.5, deck_bottom - 0.12, "Concrete")
    mesh.box(-edge, edge, low, high, deck_bottom - 0.12, -ASPHALT, "Concrete", skip="T")
    for x in (-edge * 0.6, 0.0, edge * 0.6):
        bearing_y = front_y - facing * 0.4
        mesh.box(x - 0.25, x + 0.25, bearing_y - 0.2, bearing_y + 0.2, deck_bottom - 0.12, deck_bottom, "Metal")
    wing_end = back_y - facing * WING_LENGTH
    for side in (-1.0, 1.0):
        x0, x1 = sorted((side * (edge - WING_THICKNESS), side * edge))
        polygon = [(back_y, -bridge.height - 0.5), (back_y, -ASPHALT), (wing_end, -ASPHALT),
                   (wing_end, -1.2)]
        mesh.prism(polygon, "yz", x0, x1, "Concrete")


def add_column_pier(mesh, bridge, y):
    """A pier of two round columns under the box girder's webs, with a crosshead."""
    deck_bottom = -ASPHALT - bridge.deck_depth()
    edge = bridge.half_width() - CAP_OVERHANG
    for side in (-1.0, 1.0):
        mesh.cylinder(side * edge * 0.3, y, -bridge.height - 0.5, deck_bottom - 0.6, 0.6, "Concrete", segments=20)
    mesh.box(-edge * 0.48, edge * 0.48, y - 0.6, y + 0.6, deck_bottom - 0.6, deck_bottom, "Concrete")


def build_road_bridge(bridge):
    """A road bridge: deck swept over the abutments and spans, caps and railings over the full length, abutments
    with wing walls at both ends and a pier between each pair of spans."""
    mesh = Mesh()
    length = bridge.length()
    first_front = WING_LENGTH + ABUTMENT_THICKNESS
    last_front = length - first_front
    sweep_section(mesh, deck_section(bridge)[:1], WING_LENGTH, length - WING_LENGTH)
    sweep_section(mesh, deck_section(bridge)[1:], 0.0, length)
    add_railings(mesh, bridge, 0.15, length - 0.15)
    add_abutment(mesh, bridge, first_front, 1.0)
    add_abutment(mesh, bridge, last_front, -1.0)
    y = first_front
    for span in bridge.clear_spans[:-1]:
        y += span + 0.6
        add_column_pier(mesh, bridge, y)
        y += 0.6
    return mesh


# ================================================================================================= footbridges
def add_plank_deck(mesh, half_width, start, end, top):
    """Planks across the deck with small gaps, each 0.2 m wide and 5 cm thick."""
    count = int((end - start) / 0.22)
    for plank in range(count):
        y = start + (end - start) * plank / count
        mesh.box(-half_width, half_width, y + 0.01, y + 0.21, top - 0.05, top, "Timber")


def i_girder(mesh, x, start, end, top, depth, flange):
    """A steel I-girder along Y with its top flange at top."""
    web = 0.012
    flange_thickness = 0.02
    mesh.box(x - flange / 2.0, x + flange / 2.0, start, end, top - flange_thickness, top, "Metal")
    mesh.box(x - flange / 2.0, x + flange / 2.0, start, end, top - depth, top - depth + flange_thickness, "Metal")
    mesh.box(x - web / 2.0, x + web / 2.0, start, end, top - depth + flange_thickness, top - flange_thickness, "Metal")


def build_steel_footbridge(span, width, height):
    """A park or canal footbridge: two steel girders on concrete bank seats, a plank deck and infill railings.
    Origin at the start of the deck on its axis, the walking surface at z = 0."""
    mesh = Mesh()
    seat = 1.2
    length = span + 2 * seat
    depth = max(0.3, span / 22.0)
    half = width / 2.0
    for x in (-half * 0.6, half * 0.6):
        i_girder(mesh, x, 0.1, length - 0.1, -0.05, depth, 0.2)
    for x in (-half - 0.05, half + 0.05):
        mesh.box(min(x, x * 0.98), max(x, x * 0.98) + 0.0001, 0.1, length - 0.1, -0.2, -0.05, "Metal")
    mesh.box(-half - 0.06, half + 0.06, 0.1, length - 0.1, -0.08, -0.05, "Metal", skip="T")
    add_plank_deck(mesh, half, 0.0, length, 0.0)
    for y0 in (0.0, length - seat):
        mesh.box(-half - 0.3, half + 0.3, y0, y0 + seat, -height - 0.3, -0.05 - depth, "Concrete")
    for side in (-1.0, 1.0):
        x = side * (half + 0.03)
        add_footbridge_railing(mesh, x, 0.0, length)
    return mesh


def add_footbridge_railing(mesh, x, start, end):
    """The footbridge railing, 1.1 m high: flat posts, a round handrail and vertical bars."""
    bays = max(1, round((end - start) / 1.5))
    for bay in range(bays + 1):
        y = start + (end - start) * bay / bays
        mesh.box(x - 0.03, x + 0.03, y - 0.006, y + 0.006, -0.2, 1.1, "Metal")
    mesh.tube((x, start, 1.12), (x, end, 1.12), 0.025, "Metal", segments=8)
    mesh.box(x - 0.015, x + 0.015, start, end, 0.08, 0.11, "Metal")
    bar_count = int((end - start) / 0.12)
    for bar in range(1, bar_count):
        y = start + (end - start) * bar / bar_count
        mesh.box(x - 0.008, x + 0.008, y - 0.008, y + 0.008, 0.11, 1.08, "Metal")


def build_timber_footbridge(span, width, height):
    """The Vierlande plank bridge over a ditch: timber piles in pairs, cross beams, stringers, planks, and a post and
    rail handrail with knee braces. Origin at the start of the deck on its axis, the walking surface at z = 0."""
    mesh = Mesh()
    half = width / 2.0
    bay_count = max(1, math.ceil(span / 4.0))
    pile_ys = [0.3 + (span - 0.6) * bay / bay_count for bay in range(bay_count + 1)]
    for y in pile_ys:
        for x in (-half + 0.15, half - 0.15):
            mesh.cylinder(x, y, -height - 0.8, -0.35, 0.12, "Timber", segments=10)
        mesh.box(-half - 0.15, half + 0.15, y - 0.1, y + 0.1, -0.35, -0.17, "Timber")
    for x in (-half * 0.7, 0.0, half * 0.7):
        mesh.box(x - 0.07, x + 0.07, 0.0, span, -0.17, -0.05, "Timber")
    add_plank_deck(mesh, half, 0.0, span, 0.0)
    for side in (-1.0, 1.0):
        x = side * (half + 0.08)
        for y in pile_ys:
            mesh.box(x - 0.05, x + 0.05, y - 0.05, y + 0.05, -0.35, 1.0, "Timber")
            brace_inner = side * (half - 0.3)
            mesh.tube((x, y, 0.55), (brace_inner, y, -0.1), 0.035, "Timber", segments=6)
        mesh.box(x - 0.06, x + 0.06, pile_ys[0] - 0.05, pile_ys[-1] + 0.05, 1.0, 1.08, "Timber")
        mesh.box(x - 0.025, x + 0.025, pile_ys[0], pile_ys[-1], 0.5, 0.6, "Timber")
    return mesh


# ================================================================================================= samples
SAMPLES = OrderedDict([
    ("Ditch_Slab", {"title": "Ditch crossing, 6 m span\n(Vierlande lane)",
                    "bridge": RoadBridge(5.5, 0.0, [6.0], 2.4, railing="rail"), "crosses": "water"}),
    ("Canal_Slab", {"title": "Town bridge, 14 m span\n(two lanes, footways)",
                    "bridge": RoadBridge(7.0, 2.5, [14.0], 4.2), "crosses": "water"}),
    ("Overpass_Beam", {"title": "Overpass, 3 spans\n(over a main road)",
                       "bridge": RoadBridge(10.5, 1.0, [20.0, 28.0, 20.0], 6.8, guard_rail=True),
                       "crosses": "road"}),
])
FOOTBRIDGES = OrderedDict([
    ("Foot_Steel", {"title": "Steel footbridge, 14 m", "span": 14.0, "width": 2.5, "height": 2.8}),
    ("Foot_Timber", {"title": "Timber footbridge, 7 m\n(Vierlande)", "span": 7.0, "width": 1.6, "height": 1.4}),
])


def build():
    """The sample bridges as pieces, and a spec with their cross-sections and lengths."""
    pieces = OrderedDict()
    spec = {"asphalt": ASPHALT, "kerb": KERB, "wing_length": WING_LENGTH, "abutment_thickness": ABUTMENT_THICKNESS}
    for name, sample in SAMPLES.items():
        bridge = sample["bridge"]
        pieces[name] = build_road_bridge(bridge)
        spec[name] = {
            "length": round(bridge.length(), 3),
            "clear_spans": bridge.clear_spans,
            "deck_depth": round(bridge.deck_depth(), 3),
            "section": [{"material": material, "polygon": [[round(x, 3), round(z, 3)] for x, z in polygon]}
                        for polygon, material in deck_section(bridge)],
        }
    for name, sample in FOOTBRIDGES.items():
        builder = build_steel_footbridge if name == "Foot_Steel" else build_timber_footbridge
        pieces[name] = builder(sample["span"], sample["width"], sample["height"])
        spec[name] = {"span": sample["span"], "width": sample["width"]}
    return pieces, spec


# ================================================================================================= sheet
def ground_under(length, half_width, height, crosses):
    """Sheet-only surroundings in the bridge's own frame: the embankments behind the abutments with the approach road
    on top, and the water or road below."""
    mesh = Mesh()
    ground = -height
    approach = 14.0
    slope = 1.5  # horizontal per vertical
    top_half = half_width + 0.5
    foot_half = top_half + height * slope
    for start, end in ((-approach, WING_LENGTH), (length - WING_LENGTH, length + approach)):
        section = [(-foot_half, ground), (foot_half, ground), (top_half, -ASPHALT), (-top_half, -ASPHALT)]
        mesh.prism(section, "xz", start, end, "Ground", caps="")
        mesh.add_face([(-half_width + 0.6, start, 0.0), (half_width - 0.6, start, 0.0), (half_width - 0.6, end, 0.0),
                       (-half_width + 0.6, end, 0.0)], "Asphalt", desired_normal=(0.0, 0.0, 1.0))
    span_start, span_end = WING_LENGTH, length - WING_LENGTH
    outer = foot_half + 12.0
    mesh.add_face([(-outer, -approach, ground - 0.01), (outer, -approach, ground - 0.01),
                   (outer, length + approach, ground - 0.01), (-outer, length + approach, ground - 0.01)],
                  "Ground", desired_normal=(0.0, 0.0, 1.0))
    surface = "Water" if crosses == "water" else "Asphalt"
    lift = 0.3 if crosses == "water" else 0.0
    mesh.add_face([(-outer, span_start + 1.0, ground + lift), (outer, span_start + 1.0, ground + lift),
                   (outer, span_end - 1.0, ground + lift), (-outer, span_end - 1.0, ground + lift)], surface,
                  desired_normal=(0.0, 0.0, 1.0))
    add_deck_road(mesh, half_width, length)
    return mesh


def add_deck_road(mesh, half_width, length):
    """Sheet-only road on the deck: the surfacing and a dashed centre line."""
    road_half = half_width
    mesh.add_face([(-road_half, 0.0, 0.0), (road_half, 0.0, 0.0), (road_half, length, 0.0), (-road_half, length, 0.0)],
                  "Asphalt", desired_normal=(0.0, 0.0, 1.0))
    dash = 0
    while dash * 6.0 + 3.0 < length:
        y = dash * 6.0
        mesh.add_face([(-0.06, y, 0.005), (0.06, y, 0.005), (0.06, y + 3.0, 0.005), (-0.06, y + 3.0, 0.005)],
                      "Paint", desired_normal=(0.0, 0.0, 1.0))
        dash += 1


def side_on(piece, length, height, extras):
    """A model that turns a bridge built along +Y to run along X, centred, with the ground at z = 0."""
    return [(piece, (length / 2.0, 0.0, height), (0.0, 0.0, 90.0), "XYZ")], extras.rotated_z(90.0).translated(
        length / 2.0, 0.0, height)


def road_bridge_model(name, sample):
    """The sheet entry for a road bridge: side view and a driver's view from the approach."""
    bridge = sample["bridge"]
    length = bridge.length()
    extras = ground_under(length, bridge.carriageway / 2.0, bridge.height, sample["crosses"])
    parts, extras = side_on(name, length, bridge.height, extras)
    top = bridge.height + 1.5
    caption = sample["title"].replace("\n", " ")
    return {
        "name": name, "title": sample["title"], "parts": parts, "extras": extras,
        "half_width": length / 2.0 + 10.0, "top": top, "depth": bridge.half_width() * 2.0 + 20.0,
        "closeups": [
            {"caption": caption, "target": (0.0, 0.0, bridge.height * 0.5), "direction": (-0.35, -1.0, 0.22),
             "distance": max(length * 1.0, 18.0), "lens": 35.0},
            {"caption": "Driving onto it", "target": (0.0, 0.0, bridge.height + 1.0),
             "direction": (-1.0, -0.12 * bridge.carriageway / 7.0, 0.05), "distance": length / 2.0 + 14.0,
             "lens": 35.0},
        ],
    }


def footbridge_model(name, sample, mesh):
    """The sheet entry for a footbridge: side view over water and a view along the deck."""
    span, height = sample["span"], sample["height"]
    lower, upper = mesh.bounds()
    length = upper[1] - lower[1]
    extras = Mesh()
    outer = 14.0
    extras.add_face([(-outer, -6.0, -height), (outer, -6.0, -height), (outer, length + 6.0, -height),
                     (-outer, length + 6.0, -height)], "Ground", desired_normal=(0.0, 0.0, 1.0))
    extras.add_face([(-outer, 0.6, -height + 0.3), (outer, 0.6, -height + 0.3), (outer, length - 0.6, -height + 0.3),
                     (-outer, length - 0.6, -height + 0.3)], "Water", desired_normal=(0.0, 0.0, 1.0))
    for start, end in ((-6.0, 0.6), (length - 0.6, length + 6.0)):
        bank = [(-outer, -height), (outer, -height), (outer, -0.06), (-outer, -0.06)]
        extras.prism(bank, "xz", start, end, "Ground", caps="")
    parts, extras = side_on(name, length, height, extras)
    caption = sample["title"].replace("\n", " ")
    return {
        "name": name, "title": sample["title"], "parts": parts, "extras": extras,
        "half_width": length / 2.0 + 6.0, "top": height + 1.5, "depth": 28.0,
        "closeups": [
            {"caption": caption, "target": (0.0, 0.0, height * 0.6), "direction": (-0.4, -1.0, 0.3),
             "distance": max(length * 1.1, 10.0), "lens": 35.0},
        ],
    }


def assemblies():
    """The three road bridges and the two footbridges, side on, with ground, water and the approach roads."""
    pieces, _spec = build()
    models = [road_bridge_model(name, sample) for name, sample in SAMPLES.items()]
    models += [footbridge_model(name, sample, pieces[name]) for name, sample in FOOTBRIDGES.items()]
    return models
