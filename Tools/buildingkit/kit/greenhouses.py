"""Greenhouses (#103): kit pieces for glasshouses and foil tunnels on their footprints.

Two kinds of piece, both in metres with Z up:
- Wall pieces (`<Style>_Side`, `<Style>_Gable`, `<Style>_GableDoor`) follow the building kit's wall convention: the
  glass on the plane Y = 0 seen from -Y, X to the right, origin at the bottom-left, the inside toward +Y. A side bay is
  `bay` wide, a gable span `span` wide; the gable includes the span's triangle up to the ridge.
- Roof-level pieces are in the house frame, X across the spans and Y along the ridges: `<Style>_Roof` and
  `<Style>_RoofVent` cover one span by one bay with their origin at the gutter on the left (z = 0 is the gutter
  height), `<Style>_Gutter` runs one bay along Y, `<Style>_Truss` spans one span along X under the gutter line.
  `<Style>_Post` (perimeter) and `<Style>_Column` (inside, under the gutters) stand centred on their origin.
The foil tunnel has `Tunnel_Section` (one hoop and the foil to the next, X across the tunnel from 0, Y along it)
and `Tunnel_End` (the end wall with its door, facing -Y).

Dimensions are typical Venlo values (typology.md, class 15), not measured on a Vierlande glasshouse.
"""

import math
from collections import OrderedDict
from dataclasses import dataclass

from .geom import Mesh

BAR_WIDTH = 0.05
BAR_DEPTH = 0.06
POST_SIZE = 0.1


@dataclass
class GlasshouseStyle:
    """Everything that tells one glasshouse type from another."""

    name: str
    title: str
    gutter_height: float
    span: float
    bay: float
    pitch_degrees: float
    pane_width: float
    pane_rows: int  # horizontal glazing bars split the wall glass into this many rows
    plinth_height: float
    plinth_material: str

    @property
    def ridge_rise(self):
        return self.span / 2.0 * math.tan(math.radians(self.pitch_degrees))


STYLES = OrderedDict((style.name, style) for style in [
    GlasshouseStyle("Venlo", "Venlo glasshouse", gutter_height=4.5, span=3.2, bay=4.0, pitch_degrees=23.0,
                    pane_width=1.0, pane_rows=2, plinth_height=0.4, plinth_material="Concrete"),
    GlasshouseStyle("VenloOld", "Old Vierlande glasshouse", gutter_height=2.6, span=3.2, bay=3.0, pitch_degrees=26.0,
                    pane_width=0.75, pane_rows=3, plinth_height=0.6, plinth_material="Brick"),
])


# ================================================================================================= helpers
def plane_bar(mesh, start, end, width, y0, y1, material="Aluminium"):
    """A rectangular bar in the wall plane between two (x, z) points, from y0 to y1 deep."""
    dx, dz = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dz)
    across = (-dz / length * width / 2.0, dx / length * width / 2.0)
    outline = [(start[0] - across[0], start[1] - across[1]), (end[0] - across[0], end[1] - across[1]),
               (end[0] + across[0], end[1] + across[1]), (start[0] + across[0], start[1] + across[1])]
    mesh.prism(outline, "xz", y0, y1, material)


def wall_bar(mesh, start, end, width=BAR_WIDTH):
    """A glazing bar centred on the glass plane of a wall piece."""
    plane_bar(mesh, start, end, width, -BAR_DEPTH / 2.0, BAR_DEPTH / 2.0)


def glass(mesh, points, material="GreenhouseGlass"):
    """A pane visible from both sides."""
    mesh.quad_double(points, material)


def wall_glass(mesh, outline_xz):
    """A pane on the wall plane (y = 0) from an (x, z) outline."""
    glass(mesh, [(x, 0.0, z) for x, z in outline_xz])


def add_plinth(mesh, style, x0, x1):
    """The low wall the glass stands on, with the glass plane on its outer face."""
    mesh.box(x0, x1, -0.02, 0.2, 0.0, style.plinth_height, style.plinth_material)


def add_wall_grid(mesh, style, x0, x1, bottom, top, skip=None):
    """Vertical bars every pane width and horizontal bars between the rows, from bottom to top, and the glass between;
    skip is an (x0, x1, z_top) opening that gets neither glass nor bars."""
    pane_count = max(1, round((x1 - x0) / style.pane_width))
    pane = (x1 - x0) / pane_count
    row_height = (top - bottom) / style.pane_rows
    for column in range(pane_count):
        left, right = x0 + column * pane, x0 + (column + 1) * pane
        for row in range(style.pane_rows):
            low, high = bottom + row * row_height, bottom + (row + 1) * row_height
            if skip is not None and left >= skip[0] - 1e-6 and right <= skip[1] + 1e-6 and high <= skip[2] + 1e-6:
                continue
            wall_glass(mesh, [(left, low), (right, low), (right, high), (left, high)])
    for column in range(1, pane_count):
        x = x0 + column * pane
        wall_bar(mesh, (x, bottom), (x, top))
    for row in range(1, style.pane_rows):
        z = bottom + row * row_height
        wall_bar(mesh, (x0, z), (x1, z))
    wall_bar(mesh, (x0, bottom), (x1, bottom), width=0.08)


# ================================================================================================= walls
def build_side(style):
    """One side bay: plinth, glass in its grid and the eave beam that carries the outer gutter."""
    mesh = Mesh()
    add_plinth(mesh, style, 0.0, style.bay)
    top = style.gutter_height - 0.12
    add_wall_grid(mesh, style, 0.0, style.bay, style.plinth_height, top)
    mesh.box(0.0, style.bay, -0.05, 0.08, top, style.gutter_height, "Metal")
    return mesh


def gable_triangle(mesh, style):
    """The glass triangle above the gutter line with its sloped edge bars and the centre mullion."""
    gutter = style.gutter_height
    ridge = gutter + style.ridge_rise
    middle = style.span / 2.0
    wall_glass(mesh, [(0.0, gutter), (style.span, gutter), (middle, ridge)])
    wall_bar(mesh, (0.0, gutter), (middle, ridge), width=0.07)
    wall_bar(mesh, (middle, ridge), (style.span, gutter), width=0.07)
    for x in (middle - style.span / 4.0, middle + style.span / 4.0):
        z = gutter + style.ridge_rise * (1.0 - abs(x - middle) / middle)
        wall_bar(mesh, (x, gutter), (x, z))
    mesh.box(0.0, style.span, -0.05, 0.08, gutter - 0.12, gutter, "Metal")


def build_gable(style):
    """One gable span: plinth, the wall grid up to the gutter line, the triangle and the mullion to the ridge."""
    mesh = Mesh()
    add_plinth(mesh, style, 0.0, style.span)
    add_wall_grid(mesh, style, 0.0, style.span, style.plinth_height, style.gutter_height - 0.12)
    gable_triangle(mesh, style)
    wall_bar(mesh, (style.span / 2.0, style.plinth_height), (style.span / 2.0, style.gutter_height + style.ridge_rise),
             width=0.08)
    return mesh


DOOR_WIDTH = 2.4
DOOR_HEIGHT = 2.5


def build_gable_door(style):
    """A gable span with a sliding door: the plinth broken for the opening, a glazed leaf on a top track."""
    mesh = Mesh()
    door_left = (style.span - DOOR_WIDTH) / 2.0
    door_right = door_left + DOOR_WIDTH
    door_top = min(DOOR_HEIGHT, style.gutter_height - 0.3)
    add_plinth(mesh, style, 0.0, door_left)
    add_plinth(mesh, style, door_right, style.span)
    add_wall_grid(mesh, style, 0.0, style.span, style.plinth_height, style.gutter_height - 0.12,
                  skip=(door_left, door_right, door_top))
    wall_bar(mesh, (door_left, 0.0), (door_left, door_top), width=0.08)
    wall_bar(mesh, (door_right, 0.0), (door_right, door_top), width=0.08)
    wall_bar(mesh, (door_left, door_top), (door_right, door_top), width=0.08)
    add_door_leaf(mesh, door_left, door_right, door_top)
    gable_triangle(mesh, style)
    return mesh


def add_door_leaf(mesh, left, right, top):
    """The sliding leaf in front of the opening, with its frame, a mid rail, glass and the track above."""
    leaf_y0, leaf_y1 = -0.11, -0.07
    frame = 0.06
    for x0, x1, z0, z1 in ((left, left + frame, 0.02, top), (right - frame, right, 0.02, top),
                           (left, right, 0.02, 0.02 + frame), (left, right, top - frame, top),
                           (left, right, 1.0, 1.0 + frame)):
        mesh.box(x0, x1, leaf_y0, leaf_y1, z0, z1, "Aluminium")
    glass(mesh, [(left + frame, -0.09, 0.08), (right - frame, -0.09, 0.08), (right - frame, -0.09, top - frame),
                 (left + frame, -0.09, top - frame)])
    mesh.box(left - 0.3, right + 0.3, -0.14, -0.04, top + 0.02, top + 0.12, "Metal")
    mesh.box(right - 0.2, right - 0.16, -0.14, -0.11, 0.9, 1.3, "Metal")


# ================================================================================================= roof level
def slope_point(style, x, y, lift=0.0):
    """Point on the roof surface of one span at x across it (0 at the left gutter) and y along it."""
    middle = style.span / 2.0
    z = style.ridge_rise * (1.0 - abs(x - middle) / middle)
    return (x, y, z + lift)


def roof_bar(mesh, style, x0, x1, y):
    """A rafter bar on the roof surface from x0 to x1 at y."""
    start, end = slope_point(style, x0, y, 0.02), slope_point(style, x1, y, 0.02)
    mesh.tube(start, end, 0.022, "Aluminium", segments=6)


def build_roof(style, vents):
    """One span by one bay of roof glass with rafter bars every pane and the ridge profile; vents open a hinged flap
    at the ridge in every other pane on the left slope."""
    mesh = Mesh()
    middle = style.span / 2.0
    pane_count = max(1, round(style.bay / style.pane_width))
    pane = style.bay / pane_count
    flap_run = middle * 0.55
    for index in range(pane_count):
        y0, y1 = index * pane, (index + 1) * pane
        is_vent = vents and index % 2 == 0
        left_top = middle - flap_run if is_vent else middle
        glass(mesh, [slope_point(style, 0.0, y0), slope_point(style, left_top, y0), slope_point(style, left_top, y1),
                     slope_point(style, 0.0, y1)])
        glass(mesh, [slope_point(style, middle, y0), slope_point(style, style.span, y0),
                     slope_point(style, style.span, y1), slope_point(style, middle, y1)])
        if is_vent:
            add_vent_flap(mesh, style, middle - flap_run, y0, y1)
        roof_bar(mesh, style, 0.0, style.span, y0)
    ridge = style.ridge_rise
    mesh.box(middle - 0.05, middle + 0.05, 0.0, style.bay, ridge - 0.02, ridge + 0.06, "Aluminium")
    return mesh


def add_vent_flap(mesh, style, lower_x, y0, y1, open_degrees=22.0):
    """A glazed flap hinged at the ridge, its lower edge lifted open by open_degrees."""
    middle = style.span / 2.0
    hinge = (middle, style.ridge_rise)
    lower = (lower_x, style.ridge_rise * (lower_x / middle))
    angle = math.radians(open_degrees)
    # Rotate the lower edge about the hinge, upward (counter-clockwise seen from +Y, the left slope rises to the right).
    dx, dz = lower[0] - hinge[0], lower[1] - hinge[1]
    lifted = (hinge[0] + dx * math.cos(angle) + dz * math.sin(angle), hinge[1] - dx * math.sin(angle) + dz * math.cos(angle))
    inset = 0.04
    corners = [(lifted[0], y0 + inset, lifted[1]), (hinge[0], y0 + inset, hinge[1]),
               (hinge[0], y1 - inset, hinge[1]), (lifted[0], y1 - inset, lifted[1])]
    glass(mesh, corners)
    for start, end in ((corners[0], corners[1]), (corners[1], corners[2]), (corners[2], corners[3]),
                       (corners[3], corners[0])):
        mesh.tube(start, end, 0.018, "Aluminium", segments=6)
    arm_root = slope_point(style, lower_x + 0.05, (y0 + y1) / 2.0)
    arm_tip = (lifted[0] + 0.03, (y0 + y1) / 2.0, lifted[1])
    mesh.tube(arm_root, arm_tip, 0.012, "Metal", segments=6)


def build_gutter(style):
    """A U-shaped steel gutter along one bay, its top at the gutter line."""
    mesh = Mesh()
    length = style.bay
    mesh.box(-0.11, 0.11, 0.0, length, -0.17, -0.14, "Metal")
    for x0, x1 in ((-0.11, -0.09), (0.09, 0.11)):
        mesh.box(x0, x1, 0.0, length, -0.14, 0.02, "Metal")
    return mesh


def build_post(style, size):
    """A square steel post from the ground to the gutter, centred on its origin, with a foot plate."""
    mesh = Mesh()
    half = size / 2.0
    mesh.box(-half, half, -half, half, 0.0, style.gutter_height - 0.17, "Metal")
    mesh.box(-half - 0.04, half + 0.04, -half - 0.04, half + 0.04, 0.0, 0.015, "Metal")
    return mesh


def build_truss(style):
    """A light lattice girder across one span under the gutter line: two chords and zigzag diagonals."""
    mesh = Mesh()
    top, bottom = -0.2, -0.6
    mesh.tube((0.0, 0.0, top), (style.span, 0.0, top), 0.025, "Metal", segments=6)
    mesh.tube((0.0, 0.0, bottom), (style.span, 0.0, bottom), 0.025, "Metal", segments=6)
    panels = 6
    for panel in range(panels):
        x0 = style.span * panel / panels
        x1 = style.span * (panel + 1) / panels
        if panel % 2 == 0:
            mesh.tube((x0, 0.0, bottom), (x1, 0.0, top), 0.016, "Metal", segments=5)
        else:
            mesh.tube((x0, 0.0, top), (x1, 0.0, bottom), 0.016, "Metal", segments=5)
    return mesh


# ================================================================================================= foil tunnel
TUNNEL_WIDTH = 8.0
TUNNEL_HEIGHT = 3.5
TUNNEL_SECTION = 2.0
ARCH_POINTS = 24


def arch_point(index):
    """Point on the tunnel's semi-elliptic hoop, index 0 at the left foot to ARCH_POINTS at the right foot, as (x, z)."""
    angle = math.pi * index / ARCH_POINTS
    return (TUNNEL_WIDTH / 2.0 * (1.0 - math.cos(angle)), TUNNEL_HEIGHT * math.sin(angle))


def build_tunnel_section():
    """One hoop at y = 0, purlins to the next hoop, and the foil over the section."""
    mesh = Mesh()
    hoop = [(x, 0.0, z) for x, z in (arch_point(index) for index in range(ARCH_POINTS + 1))]
    mesh.pipe(hoop, 0.03, "Metal", segments=6)
    for index in (ARCH_POINTS // 4, ARCH_POINTS // 2, 3 * ARCH_POINTS // 4):
        x, z = arch_point(index)
        mesh.tube((x, 0.0, z - 0.05), (x, TUNNEL_SECTION, z - 0.05), 0.02, "Metal", segments=6)
    for index in range(ARCH_POINTS):
        (x0, z0), (x1, z1) = arch_point(index), arch_point(index + 1)
        quad = [(x0, 0.0, z0 + 0.035), (x1, 0.0, z1 + 0.035), (x1, TUNNEL_SECTION, z1 + 0.035),
                (x0, TUNNEL_SECTION, z0 + 0.035)]
        mesh.add_face(quad, "Foil", smooth=True, desired_normal=(x0 - TUNNEL_WIDTH / 2.0, 0.0, z0 + 0.1))
        mesh.add_face(list(reversed(quad)), "Foil", smooth=True, desired_normal=(TUNNEL_WIDTH / 2.0 - x0, 0.0, -z0))
    return mesh


def build_tunnel_end():
    """The end wall facing -Y: the hoop, two door posts up to the arch, a timber door frame, foil over the rest."""
    mesh = Mesh()
    hoop = [(x, 0.0, z) for x, z in (arch_point(index) for index in range(ARCH_POINTS + 1))]
    mesh.pipe(hoop, 0.03, "Metal", segments=6)
    door_left, door_right, door_top = 3.0, 5.0, 2.4
    for x in (door_left - 0.08, door_right):
        height = TUNNEL_HEIGHT * math.sin(math.acos(1.0 - x / (TUNNEL_WIDTH / 2.0)))
        mesh.box(x, x + 0.08, -0.04, 0.04, 0.0, height - 0.02, "Timber")
    mesh.box(door_left, door_right, -0.04, 0.04, door_top, door_top + 0.08, "Timber")
    outline = [(x, -0.045, z) for x, z in (arch_point(index) for index in range(ARCH_POINTS + 1))]
    glass(mesh, outline, material="Foil")
    leaf = [(door_left + 0.05, -0.08, 0.03), (door_right - 0.05, -0.08, 0.03), (door_right - 0.05, -0.08, door_top - 0.03),
            (door_left + 0.05, -0.08, door_top - 0.03)]
    glass(mesh, leaf, material="Foil")
    for x0, x1, z0, z1 in ((door_left, door_left + 0.06, 0.0, door_top), (door_right - 0.06, door_right, 0.0, door_top),
                           (door_left, door_right, 0.0, 0.06), (door_left, door_right, door_top - 0.06, door_top),
                           (door_left, door_right, 1.1, 1.16)):
        mesh.box(x0, x1, -0.1, -0.06, z0, z1, "Timber")
    return mesh


# ================================================================================================= kit entry
def build():
    """Every piece of both glasshouse styles and of the tunnel, and a spec with the module sizes."""
    pieces = OrderedDict()
    spec = {}
    for style in STYLES.values():
        prefix = style.name + "_"
        pieces[prefix + "Side"] = build_side(style)
        pieces[prefix + "Gable"] = build_gable(style)
        pieces[prefix + "GableDoor"] = build_gable_door(style)
        pieces[prefix + "Roof"] = build_roof(style, vents=False)
        pieces[prefix + "RoofVent"] = build_roof(style, vents=True)
        pieces[prefix + "Gutter"] = build_gutter(style)
        pieces[prefix + "Post"] = build_post(style, POST_SIZE)
        pieces[prefix + "Column"] = build_post(style, 0.08)
        pieces[prefix + "Truss"] = build_truss(style)
        spec[style.name] = {"span": style.span, "bay": style.bay, "gutter_height": style.gutter_height,
                            "ridge_rise": round(style.ridge_rise, 3), "pitch_degrees": style.pitch_degrees}
    pieces["Tunnel_Section"] = build_tunnel_section()
    pieces["Tunnel_End"] = build_tunnel_end()
    spec["Tunnel"] = {"width": TUNNEL_WIDTH, "height": TUNNEL_HEIGHT, "section": TUNNEL_SECTION}
    return pieces, spec


def glasshouse_parts(style, spans, bays):
    """The placement of every piece for a glasshouse of spans by bays, in the house frame with the front gable on
    y = 0 and the left side on x = 0, as the runtime would place them on a footprint."""
    width, length = spans * style.span, bays * style.bay
    gutter = style.gutter_height
    prefix = style.name + "_"
    parts = []

    def place(piece, x, y, z=0.0, yaw=0.0):
        parts.append((prefix + piece, (x, y, z), (0.0, 0.0, yaw), "XYZ"))

    for span in range(spans):
        front = "GableDoor" if span == spans // 2 else "Gable"
        place(front, span * style.span, 0.0)
        place("Gable", width - span * style.span, length, yaw=180.0)
    for bay in range(bays):
        place("Side", 0.0, length - bay * style.bay, yaw=-90.0)
        place("Side", width, bay * style.bay, yaw=90.0)
    for span in range(spans + 1):
        for y in (0.0, length):
            place("Post", span * style.span, y)
    for bay in range(1, bays):
        for x in (0.0, width):
            place("Post", x, bay * style.bay)
    for span in range(spans + 1):
        for bay in range(bays):
            place("Gutter", span * style.span, bay * style.bay, gutter)
    for span in range(spans):
        for bay in range(bays):
            roof = "RoofVent" if (span + bay) % 2 == 0 else "Roof"
            place(roof, span * style.span, bay * style.bay, gutter)
    for bay in range(1, bays):
        for span in range(1, spans):
            place("Column", span * style.span, bay * style.bay)
        for span in range(spans):
            place("Truss", span * style.span, bay * style.bay, gutter)
    return parts, width, length


def tunnel_parts(sections):
    """A foil tunnel of sections with an end wall at both ends."""
    length = sections * TUNNEL_SECTION
    parts = [("Tunnel_End", (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "XYZ"),
             ("Tunnel_End", (TUNNEL_WIDTH, length, 0.0), (0.0, 0.0, 180.0), "XYZ")]
    for section in range(sections):
        parts.append(("Tunnel_Section", (0.0, section * TUNNEL_SECTION, 0.0), (0.0, 0.0, 0.0), "XYZ"))
    return parts, TUNNEL_WIDTH, length


def centred(parts, width):
    """Shifts parts so the model's width is centred on x = 0, as the line-up expects."""
    return [(piece, (location[0] - width / 2.0, location[1], location[2]), rotation, mode)
            for piece, location, rotation, mode in parts]


def assemblies():
    """A modern Venlo house of three spans, an old one of two, and a tunnel, with close-ups of the door corner, the
    roof vents, the inside and the tunnel end."""
    venlo, old = STYLES["Venlo"], STYLES["VenloOld"]
    venlo_parts, venlo_width, venlo_length = glasshouse_parts(venlo, 3, 4)
    old_parts, old_width, old_length = glasshouse_parts(old, 2, 4)
    tunnel, tunnel_width, tunnel_length = tunnel_parts(6)
    venlo_top = venlo.gutter_height + venlo.ridge_rise
    return [
        {
            "name": "Venlo", "title": venlo.title, "parts": centred(venlo_parts, venlo_width),
            "half_width": venlo_width / 2.0, "top": venlo_top, "depth": venlo_length, "extras": None,
            "closeups": [
                {"caption": "Venlo, door and corner", "target": (-venlo_width * 0.15, 0.0, 2.2),
                 "direction": (-0.6, -1.0, 0.3), "distance": 12.0, "lens": 35.0},
                {"caption": "Venlo, roof vents", "target": (0.0, venlo_length * 0.4, venlo.gutter_height + 0.4),
                 "direction": (-0.35, -0.7, 1.0), "distance": 11.0, "lens": 35.0},
                {"caption": "Venlo, inside", "target": (0.0, venlo_length * 0.7, venlo.gutter_height - 0.4),
                 "direction": (0.15, -1.0, -0.35), "distance": 6.5, "lens": 22.0},
            ],
        },
        {
            "name": "VenloOld", "title": old.title, "parts": centred(old_parts, old_width),
            "half_width": old_width / 2.0, "top": old.gutter_height + old.ridge_rise, "depth": old_length,
            "extras": None,
            "closeups": [
                {"caption": old.title, "target": (-old_width * 0.1, 0.0, 1.6),
                 "direction": (-0.6, -1.0, 0.3), "distance": 9.5, "lens": 35.0},
            ],
        },
        {
            "name": "Tunnel", "title": "Foil tunnel", "parts": centred(tunnel, tunnel_width),
            "half_width": tunnel_width / 2.0, "top": TUNNEL_HEIGHT, "depth": tunnel_length, "extras": None,
            "closeups": [
                {"caption": "Foil tunnel, end wall", "target": (0.0, 0.0, 1.7),
                 "direction": (-0.6, -1.0, 0.3), "distance": 12.0, "lens": 35.0},
            ],
        },
    ]
