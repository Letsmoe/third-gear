"""Power pylons (#101): the 110 kV steel lattice designs of the Bergedorf extract, each as a suspension and an anchor
variant, as one piece per pylon.

Frame: metres, Z up, origin at the centre of the pylon foot on the ground. The line runs along Y, the cross arms
along X. Every conductor and earth wire attachment point is written to the spec as a socket, so the runtime can hang
the catenaries between neighbouring pylons.

Dimensions are typical German 110 kV values (typology.md, "Power pylons and lines"), not a specific pylon.
"""

import math
from collections import OrderedDict
from dataclasses import dataclass, field

from .geom import Mesh, vec_cross, vec_dot, vec_normalize, vec_sub

INSULATOR_LENGTH = 1.6
EARTH_WIRE_CLAMP = 0.25


@dataclass
class CrossArm:
    """One level of cross arms: its height, reach on each side and the conductor positions along it."""

    height: float  # bottom chord level
    reach: float  # tip distance from the pylon axis
    depth: float  # arm height at the body
    conductors: list  # distances from the axis, mirrored to both sides


@dataclass
class PylonDesign:
    """Everything that tells one pylon from another."""

    name: str
    title: str
    base_half_width: float
    waist_half_width: float  # body half width at the lowest arm
    arms: list
    peak_height: float  # earth wire clamp
    anchor: bool
    leg_size: float
    brace_size: float
    first_panel: float = 4.2
    panel_ratio: float = 0.88
    sockets: list = field(default_factory=list)

    @property
    def lowest_arm(self):
        return min(arm.height for arm in self.arms)

    @property
    def top_arm(self):
        return max(self.arms, key=lambda arm: arm.height)


def donau(anchor):
    """The Donau design: two arm levels, the lower with two conductors per side, the upper with one."""
    arms = [CrossArm(21.0, 7.2, 1.6, [3.8, 6.9]), CrossArm(26.5, 4.6, 1.4, [4.3])]
    return PylonDesign(
        name="Donau_" + ("Anchor" if anchor else "Suspension"),
        title="Donau, " + ("anchor" if anchor else "suspension"),
        base_half_width=3.4 if anchor else 2.4, waist_half_width=0.95 if anchor else 0.75, arms=arms,
        peak_height=32.5, anchor=anchor, leg_size=0.14 if anchor else 0.10, brace_size=0.07 if anchor else 0.06,
    )


def one_level(anchor):
    """The one-level design: one long arm carrying all six conductors, the earth wire on a peak above it."""
    arms = [CrossArm(19.0, 10.8, 1.8, [3.7, 7.1, 10.5])]
    return PylonDesign(
        name="OneLevel_" + ("Anchor" if anchor else "Suspension"),
        title="One-level, " + ("anchor" if anchor else "suspension"),
        base_half_width=3.6 if anchor else 2.6, waist_half_width=1.0 if anchor else 0.8, arms=arms,
        peak_height=24.5, anchor=anchor, leg_size=0.14 if anchor else 0.10, brace_size=0.07 if anchor else 0.06,
    )


DESIGNS = OrderedDict((design.name, design) for design in [donau(False), donau(True), one_level(False), one_level(True)])


# ================================================================================================= steel angles
def angle_member(mesh, start, end, flange_a, flange_b, size, material="Lattice"):
    """An L-section steel angle from start to end. The corner of the L runs along the line; its two flanges point
    along flange_a and flange_b (made perpendicular to the member here)."""
    axis = vec_normalize(vec_sub(end, start))
    side_a = vec_normalize(remove_component(flange_a, axis))
    side_b = remove_component(flange_b, axis)
    side_b = vec_normalize(remove_component(side_b, side_a))
    thickness = max(0.008, size * 0.1)
    outline = [(0.0, 0.0), (size, 0.0), (size, thickness), (thickness, thickness), (thickness, size), (0.0, size)]

    def lift(point, base):
        return tuple(base[k] + point[0] * side_a[k] + point[1] * side_b[k] for k in range(3))

    near = [lift(point, start) for point in outline]
    far = [lift(point, end) for point in outline]
    count = len(outline)
    for i in range(count):
        j = (i + 1) % count
        quad = [near[i], near[j], far[j], far[i]]
        edge = (outline[j][0] - outline[i][0], outline[j][1] - outline[i][1])
        # The L outline is counter-clockwise in (a, b), so the outward edge normal is (edge_b, -edge_a).
        normal_2d = (edge[1], -edge[0])
        outward = tuple(normal_2d[0] * side_a[k] + normal_2d[1] * side_b[k] for k in range(3))
        mesh.add_face(quad, material, desired_normal=outward)
    mesh.add_face(near, material, desired_normal=tuple(-c for c in axis))
    mesh.add_face(far, material, desired_normal=axis)


def remove_component(vector, direction):
    """The part of vector perpendicular to the unit direction."""
    along = vec_dot(vector, direction)
    return tuple(vector[k] - along * direction[k] for k in range(3))


def face_member(mesh, start, end, inward, size):
    """A bracing angle lying on a lattice face: one flange in the face, the other pointing into the pylon."""
    axis = vec_sub(end, start)
    in_face = vec_cross(inward, axis)
    angle_member(mesh, start, end, in_face, inward, size)


# ================================================================================================= body
def half_width_at(design, z):
    """Half width of the square body: tapering legs up to the lowest arm, then slimmer up to the top arm."""
    top_arm = design.top_arm
    top_level = top_arm.height + top_arm.depth
    if z <= design.lowest_arm:
        fraction = z / design.lowest_arm
        return design.base_half_width + (design.waist_half_width - design.base_half_width) * fraction
    if z <= top_level:
        fraction = (z - design.lowest_arm) / max(top_level - design.lowest_arm, 1e-6)
        return design.waist_half_width + (design.waist_half_width * 0.8 - design.waist_half_width) * fraction
    fraction = (z - top_level) / (design.peak_height - top_level)
    return design.waist_half_width * 0.8 * (1.0 - fraction) + 0.12 * fraction


def panel_levels(design):
    """Heights of the bracing panels: shrinking geometrically from the ground to the lowest arm, then one panel per
    arm level and short panels up to the peak."""
    heights = []
    panel = design.first_panel
    total = 0.0
    while total + panel < design.lowest_arm - 0.5:
        heights.append(panel)
        total += panel
        panel *= design.panel_ratio
    scale = design.lowest_arm / sum(heights)
    levels = [0.0]
    for height in heights:
        levels.append(levels[-1] + height * scale)
    levels[-1] = design.lowest_arm
    for arm in sorted(design.arms, key=lambda arm: arm.height):
        for z in (arm.height, arm.height + arm.depth):
            if z > levels[-1] + 0.3:
                levels.append(z)
            elif z > levels[-1]:
                levels[-1] = z
    while levels[-1] < design.peak_height - 0.1:
        step = min(1.8, design.peak_height - levels[-1])
        levels.append(levels[-1] + step)
    return levels


def corner(design, z, sign_x, sign_y):
    """Leg position at a height, on the corner given by the two signs."""
    half = half_width_at(design, z)
    return (sign_x * half, sign_y * half, z)


FACES = [  # (corner signs from, corner signs to, inward normal)
    ((-1, -1), (1, -1), (0.0, 1.0, 0.0)),
    ((1, -1), (1, 1), (-1.0, 0.0, 0.0)),
    ((1, 1), (-1, 1), (0.0, -1.0, 0.0)),
    ((-1, 1), (-1, -1), (1.0, 0.0, 0.0)),
]


def add_body(mesh, design):
    """Four legs and X bracing with horizontals on every face, up to the earth wire peak."""
    levels = panel_levels(design)
    for sign_x, sign_y in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        for z0, z1 in zip(levels, levels[1:]):
            angle_member(mesh, corner(design, z0, sign_x, sign_y), corner(design, z1, sign_x, sign_y),
                         (-sign_x, 0.0, 0.0), (0.0, -sign_y, 0.0), leg_size_at(design, z0))
    for index, (z0, z1) in enumerate(zip(levels, levels[1:])):
        for (from_x, from_y), (to_x, to_y), inward in FACES:
            bottom_from = corner(design, z0, from_x, from_y)
            bottom_to = corner(design, z0, to_x, to_y)
            top_from = corner(design, z1, from_x, from_y)
            top_to = corner(design, z1, to_x, to_y)
            size = design.brace_size if z0 < design.lowest_arm else design.brace_size * 0.85
            face_member(mesh, bottom_from, top_to, inward, size)
            face_member(mesh, bottom_to, top_from, inward, size)
            if index % 2 == 1 or z1 >= design.lowest_arm - 1e-6:
                face_member(mesh, top_from, top_to, inward, size)


def leg_size_at(design, z):
    """Legs get lighter toward the top."""
    if z < design.lowest_arm:
        return design.leg_size
    return design.leg_size * 0.75


def add_anti_climb(mesh, design):
    """The barbed band about 3 m up that keeps people from climbing: a frame around the legs with spikes outward."""
    z = 3.2
    half = half_width_at(design, z) + 0.25
    ring = [(-half, -half, z), (half, -half, z), (half, half, z), (-half, half, z), (-half, -half, z)]
    mesh.pipe(ring, 0.02, "Lattice", segments=6, caps=False)
    for frame_corner in ring[:4]:
        leg = corner(design, z, math.copysign(1, frame_corner[0]), math.copysign(1, frame_corner[1]))
        mesh.tube(leg, frame_corner, 0.018, "Lattice", segments=6)
    spikes_per_side = int(half * 2 / 0.25)
    for start, end in zip(ring, ring[1:]):
        for spike in range(spikes_per_side):
            fraction = (spike + 0.5) / spikes_per_side
            root = tuple(start[k] + (end[k] - start[k]) * fraction for k in range(3))
            outward = vec_normalize((root[0], root[1], 0.0))
            tip = (root[0] + outward[0] * 0.12, root[1] + outward[1] * 0.12, root[2] + 0.1)
            mesh.tube(root, tip, 0.005, "Lattice", segments=4)


def add_foundations(mesh, design):
    """A concrete stub under every leg, 0.4 m above the ground."""
    half = design.base_half_width
    pad = 0.55 if design.anchor else 0.45
    for sign_x in (-1, 1):
        for sign_y in (-1, 1):
            x, y = sign_x * half, sign_y * half
            mesh.box(x - pad, x + pad, y - pad, y + pad, -0.3, 0.4, "Concrete")


# ================================================================================================= cross arms
def add_cross_arm(mesh, design, arm, side):
    """A tapering truss from the body to the tip: two bottom chords, two top chords and zigzag lacing on every face."""
    base = arm.height
    half = half_width_at(design, base)
    top_half = half_width_at(design, base + arm.depth)
    tip_x = side * arm.reach
    tip_half = 0.15
    bottom = [(side * half, -half, base), (side * half, half, base)]
    bottom_tip = [(tip_x, -tip_half, base), (tip_x, tip_half, base)]
    top = [(side * top_half, -top_half, base + arm.depth), (side * top_half, top_half, base + arm.depth)]
    top_tip = [(tip_x, -tip_half, base + 0.3), (tip_x, tip_half, base + 0.3)]
    chord = design.brace_size * 1.3
    for index in range(2):
        sign_y = -1 if index == 0 else 1
        angle_member(mesh, bottom[index], bottom_tip[index], (0.0, -sign_y, 0.0), (0.0, 0.0, 1.0), chord)
        angle_member(mesh, top[index], top_tip[index], (0.0, -sign_y, 0.0), (0.0, 0.0, -1.0), chord)
    bays = max(3, int(arm.reach / 1.4))
    lace = design.brace_size * 0.8

    def along(start_pair, end_pair, index, fraction):
        return tuple(start_pair[index][k] + (end_pair[index][k] - start_pair[index][k]) * fraction for k in range(3))

    for bay in range(bays):
        f0, f1 = bay / bays, (bay + 1) / bays
        for index, sign_y in ((0, -1), (1, 1)):
            # Side faces: zigzag between bottom and top chord.
            inward = (0.0, -sign_y, 0.0)
            face_member(mesh, along(bottom, bottom_tip, index, f0), along(top, top_tip, index, f1), inward, lace)
            face_member(mesh, along(top, top_tip, index, f1), along(bottom, bottom_tip, index, f1), inward, lace)
        face_member(mesh, along(bottom, bottom_tip, 0, f0), along(bottom, bottom_tip, 1, f1), (0.0, 0.0, 1.0), lace)
        face_member(mesh, along(bottom, bottom_tip, 1, f1), along(bottom, bottom_tip, 0, f1), (0.0, 0.0, 1.0), lace)
        face_member(mesh, along(top, top_tip, 0, f1), along(top, top_tip, 1, f1), (0.0, 0.0, -1.0), lace)
    angle_member(mesh, bottom_tip[0], bottom_tip[1], (side * -1.0, 0.0, 0.0), (0.0, 0.0, 1.0), lace)
    angle_member(mesh, bottom_tip[0], top_tip[0], (side * -1.0, 0.0, 0.0), (0.0, 1.0, 0.0), lace)
    angle_member(mesh, bottom_tip[1], top_tip[1], (side * -1.0, 0.0, 0.0), (0.0, -1.0, 0.0), lace)


def add_conductor_beam(mesh, design, arm, x):
    """A cross beam between the two bottom chords at the conductor position, where the insulator hangs."""
    fraction = (abs(x) - half_width_at(design, arm.height)) / (arm.reach - half_width_at(design, arm.height))
    half = half_width_at(design, arm.height) + (0.15 - half_width_at(design, arm.height)) * fraction
    angle_member(mesh, (x, -half, arm.height), (x, half, arm.height), (1.0, 0.0, 0.0), (0.0, 0.0, -1.0),
                 design.brace_size)
    return half


# ================================================================================================= insulators
def oriented(mesh, origin, axis):
    """Maps a mesh built along +Y onto a direction from origin."""
    y_axis = vec_normalize(axis)
    helper = (0.0, 0.0, 1.0) if abs(y_axis[2]) < 0.9 else (1.0, 0.0, 0.0)
    x_axis = vec_normalize(vec_cross(y_axis, helper))
    z_axis = vec_cross(x_axis, y_axis)
    return mesh.transformed(lambda p: tuple(
        origin[k] + p[0] * x_axis[k] + p[1] * y_axis[k] + p[2] * z_axis[k] for k in range(3)))


def composite_insulator(length):
    """A silicone long-rod insulator along +Y: metal end fittings, the rod, and alternating large and small sheds."""
    insulator = Mesh()
    fitting = 0.14
    insulator.lathe([(0.0, 0.03), (fitting, 0.035)], "Metal", segments=10)
    insulator.lathe([(length - fitting, 0.035), (length, 0.03)], "Metal", segments=10)
    insulator.lathe([(fitting, 0.02), (length - fitting, 0.02)], "Insulator", segments=10)
    pitch = 0.055
    shed_count = int((length - 2 * fitting) / pitch)
    for shed in range(shed_count):
        y = fitting + (shed + 0.5) * pitch
        radius = 0.065 if shed % 2 == 0 else 0.048
        insulator.lathe([(y - 0.006, 0.02), (y - 0.003, radius), (y + 0.003, radius), (y + 0.006, 0.02)],
                        "Insulator", segments=12)
    return insulator


def add_suspension_string(mesh, top, sockets, name):
    """A vertical insulator hanging from top, with the conductor clamp at its foot; adds the conductor socket."""
    insulator = composite_insulator(INSULATOR_LENGTH)
    mesh.add(oriented(insulator, top, (0.0, 0.0, -1.0)))
    clamp_z = top[2] - INSULATOR_LENGTH
    mesh.box(top[0] - 0.05, top[0] + 0.05, top[1] - 0.18, top[1] + 0.18, clamp_z - 0.06, clamp_z, "Metal")
    sockets.append({"name": name, "position": [round(top[0], 3), round(top[1], 3), round(clamp_z - 0.03, 3)]})


def add_tension_strings(mesh, x, z, beam_half, sockets, name):
    """Anchor pylons hold the conductor on a horizontal insulator to each side along the line, joined below by a
    hanging jumper loop; adds the two conductor sockets (back and ahead along Y)."""
    for sign in (-1, 1):
        root = (x, sign * beam_half, z)
        direction = vec_normalize((0.0, sign * 1.0, -0.12))
        mesh.add(oriented(composite_insulator(INSULATOR_LENGTH), root, direction))
        end = tuple(root[k] + direction[k] * INSULATOR_LENGTH for k in range(3))
        sockets.append({"name": name + ("_Back" if sign < 0 else "_Ahead"),
                        "position": [round(end[0], 3), round(end[1], 3), round(end[2], 3)]})
    loop = []
    reach = beam_half + INSULATOR_LENGTH * 0.993
    end_z = z - 0.12 * INSULATOR_LENGTH * 0.993
    drop = 1.6
    steps = 12
    for step in range(steps + 1):
        fraction = step / steps
        y = -reach + 2 * reach * fraction
        loop.append((x, y, end_z - drop * math.sin(math.pi * fraction)))
    mesh.pipe(loop, 0.011, "Metal", segments=6)


def add_conductor_hardware(mesh, design, arm, sockets, level_name):
    """Insulators for every conductor on both sides of one arm level."""
    for side in (-1, 1):
        for index, distance in enumerate(arm.conductors):
            x = side * distance
            beam_half = add_conductor_beam(mesh, design, arm, x)
            name = "%s_%s%d" % (level_name, "L" if side < 0 else "R", index + 1)
            if design.anchor:
                add_tension_strings(mesh, x, arm.height, beam_half, sockets, name)
            else:
                add_suspension_string(mesh, (x, 0.0, arm.height), sockets, name)


def add_earth_wire_peak(mesh, design, sockets):
    """Clamp for the earth wire at the top of the peak."""
    z = design.peak_height
    mesh.box(-0.15, 0.15, -0.2, 0.2, z, z + 0.08, "Metal")
    mesh.cylinder(0.0, 0.0, z + 0.08, z + EARTH_WIRE_CLAMP, 0.04, "Metal", segments=8)
    sockets.append({"name": "EarthWire", "position": [0.0, 0.0, round(z + EARTH_WIRE_CLAMP, 3)]})


# ================================================================================================= kit entry
def build_pylon(design):
    """One complete pylon and its sockets."""
    mesh = Mesh()
    sockets = []
    add_foundations(mesh, design)
    add_body(mesh, design)
    add_anti_climb(mesh, design)
    for level, arm in enumerate(sorted(design.arms, key=lambda arm: arm.height)):
        for side in (-1, 1):
            add_cross_arm(mesh, design, arm, side)
        add_conductor_hardware(mesh, design, arm, sockets, "Arm%d" % (level + 1))
    add_earth_wire_peak(mesh, design, sockets)
    return mesh, sockets


def build():
    """Every design as one piece named after it, and the spec with its sockets."""
    pieces = OrderedDict()
    spec = {}
    for design in DESIGNS.values():
        mesh, sockets = build_pylon(design)
        pieces[design.name] = mesh
        spec[design.name] = {"height": design.peak_height + EARTH_WIRE_CLAMP, "anchor": design.anchor,
                             "sockets": sockets}
    return pieces, spec


def preview_conductors(sockets, length=45.0, span=300.0, sag=8.0):
    """The first metres of every conductor leaving its socket along the line, on the parabola of a typical span,
    only for the contact sheet."""
    wires = Mesh()
    for socket in sockets:
        x, y, z = socket["position"]
        directions = [-1, 1]
        if socket["name"].endswith("_Back"):
            directions = [-1]
        elif socket["name"].endswith("_Ahead"):
            directions = [1]
        for direction in directions:
            points = []
            for step in range(9):
                distance = length * step / 8
                drop = 4.0 * sag * (distance / span) * (1.0 - distance / span)
                points.append((x, y + direction * distance, z - drop))
            wires.pipe(points, 0.03, "Metal", segments=5)
    return wires


def assemblies():
    """How the contact sheet shows each pylon: one piece at the origin, conductor stubs, and close-ups of the arms
    and of the foot."""
    pieces, spec = build()
    result = []
    for design in DESIGNS.values():
        top_arm = design.top_arm
        widest = max(arm.reach for arm in design.arms)
        closeups = [{
            "caption": design.title + ", arms",
            "target": (widest * 0.45, 0.0, top_arm.height - 0.6),
            "direction": (-0.35, -1.0, 0.25),
            "distance": widest * 1.9,
            "lens": 40.0,
        }]
        if design.name.startswith("Donau"):
            closeups.append({
                "caption": design.title + ", foot",
                "target": (0.0, 0.0, 2.2),
                "direction": (-0.6, -1.0, 0.3),
                "distance": design.base_half_width * 4.2,
                "lens": 35.0,
            })
        result.append({
            "name": design.name,
            "title": design.title,
            "parts": [(design.name, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "XYZ")],
            "half_width": widest,
            "top": design.peak_height,
            "extras": preview_conductors(spec[design.name]["sockets"]),
            "closeups": closeups,
        })
    return result
