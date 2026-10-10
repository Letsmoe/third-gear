"""Wind turbines (#100): the four models found in the Bergedorf OSM extract, each as a tower, a nacelle and a rotor.

Frames, all in metres with Z up and the rotor facing -Y (upwind):
- Tower: origin at the centre of the tower foot on the ground; the top flange is at z = tower height.
- Nacelle: origin on the yaw axis at the tower's top flange, so yawing the nacelle is a rotation about its own Z.
  The rotor attaches at the socket (0, -overhang, axis height) and is tilted by the model's tilt, front end up.
- Rotor: origin at the hub centre, the rotor axis along Y. Seen from the front (from -Y) it turns clockwise, which is
  a right-handed rotation about +Y. The blades are coned forward and pre-bent upwind; the tilt is not built in.

Dimensions are the manufacturers' figures where known (typology.md, "Wind turbines"), the rest is measured on photos.
"""

import math
from collections import OrderedDict
from dataclasses import dataclass

from .geom import Mesh

# German day marking for structures above 100 m: red, white and red bands of 6 m each from the blade tip inward.
MARKING_BAND_LENGTH = 6.0
MARKING_MIN_TOTAL_HEIGHT = 100.0
# Enercon paints the foot of the tower in five green bands, dark at the ground.
GREEN_BAND_COUNT = 5
GREEN_BAND_HEIGHT = 3.0


@dataclass
class TurbineModel:
    """Everything that tells one turbine model from another."""

    name: str
    rotor_diameter: float
    hub_height: float
    tower_base_diameter: float
    tower_top_diameter: float
    tower_sections: int
    nacelle_shape: str  # "box" (geared, angular), "rounded" (geared, round edges) or "egg" (Enercon direct drive)
    nacelle_length: float
    nacelle_width: float
    nacelle_height: float
    overhang: float  # hub centre ahead of the tower axis
    axis_height: float  # hub centre above the top flange
    spinner_diameter: float
    spinner_length: float
    blade_max_chord: float
    blade_root_diameter: float
    root_style: str  # "round" (cylindrical root) or "faired" (Enercon, chord runs into the spinner)
    tilt_degrees: float = 5.0
    cone_degrees: float = 3.0
    prebend: float = 0.0  # tip displacement upwind
    green_bands: bool = False
    transformer_box: bool = True
    rated_rpm: float = 12.0

    @property
    def rotor_radius(self):
        return self.rotor_diameter / 2.0

    @property
    def tower_height(self):
        return self.hub_height - self.axis_height

    @property
    def marked(self):
        """True when the blade tips carry the red and white day marking."""
        return self.hub_height + self.rotor_radius > MARKING_MIN_TOTAL_HEIGHT


MODELS = OrderedDict((model.name, model) for model in [
    TurbineModel(
        name="MM100", rotor_diameter=100.0, hub_height=100.0, tower_base_diameter=4.3, tower_top_diameter=3.0,
        tower_sections=4, nacelle_shape="box", nacelle_length=13.0, nacelle_width=4.0, nacelle_height=4.0,
        overhang=5.2, axis_height=2.3, spinner_diameter=3.6, spinner_length=4.2, blade_max_chord=3.5,
        blade_root_diameter=2.2, root_style="round", prebend=2.0, rated_rpm=13.9,
    ),
    TurbineModel(
        name="N117", rotor_diameter=117.0, hub_height=91.0, tower_base_diameter=4.3, tower_top_diameter=3.0,
        tower_sections=4, nacelle_shape="rounded", nacelle_length=13.5, nacelle_width=4.1, nacelle_height=4.2,
        overhang=5.6, axis_height=2.4, spinner_diameter=3.8, spinner_length=4.6, blade_max_chord=3.9,
        blade_root_diameter=2.3, root_style="round", prebend=3.0, rated_rpm=14.1,
    ),
    TurbineModel(
        name="E92", rotor_diameter=92.0, hub_height=104.0, tower_base_diameter=4.3, tower_top_diameter=2.8,
        tower_sections=4, nacelle_shape="egg", nacelle_length=10.5, nacelle_width=5.4, nacelle_height=5.4,
        overhang=5.0, axis_height=3.0, spinner_diameter=4.6, spinner_length=3.6, blade_max_chord=3.6,
        blade_root_diameter=2.4, root_style="faired", prebend=1.0, green_bands=True, transformer_box=False,
        rated_rpm=16.0,
    ),
    TurbineModel(
        name="NM48", rotor_diameter=48.0, hub_height=46.0, tower_base_diameter=3.0, tower_top_diameter=1.9,
        tower_sections=2, nacelle_shape="box", nacelle_length=7.5, nacelle_width=2.6, nacelle_height=2.8,
        overhang=2.6, axis_height=1.5, spinner_diameter=1.9, spinner_length=2.4, blade_max_chord=2.0,
        blade_root_diameter=1.2, root_style="round", cone_degrees=0.0, rated_rpm=30.0,
    ),
])


# ================================================================================================= helpers
def interpolate(table, position):
    """Piecewise linear lookup in a list of (position, value) pairs sorted by position."""
    if position <= table[0][0]:
        return table[0][1]
    for (start, start_value), (end, end_value) in zip(table, table[1:]):
        if position <= end:
            fraction = (position - start) / (end - start)
            return start_value + fraction * (end_value - start_value)
    return table[-1][1]


def rotated_y(mesh, degrees):
    """Rotates a mesh about the Y axis (right-handed: +Z turns toward +X)."""
    cosine, sine = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return mesh.transformed(lambda p: (p[0] * cosine + p[2] * sine, p[1], -p[0] * sine + p[2] * cosine))


def rounded_rectangle(width, height, radius, bottom, corner_segments=4):
    """Counter-clockwise (x, z) outline of a rectangle centred on x = 0, standing on z = bottom, with rounded corners."""
    half = width / 2.0
    corners = [
        (half - radius, bottom + radius, -90.0),
        (half - radius, bottom + height - radius, 0.0),
        (-half + radius, bottom + height - radius, 90.0),
        (-half + radius, bottom + radius, 180.0),
    ]
    outline = []
    for centre_x, centre_z, start_angle in corners:
        for step in range(corner_segments + 1):
            angle = math.radians(start_angle + 90.0 * step / corner_segments)
            outline.append((centre_x + radius * math.cos(angle), centre_z + radius * math.sin(angle)))
    return outline


# ================================================================================================= blade
# Planform and section tables over the blade span, 0 at the root flange and 1 at the tip.
ROUND_ROOT = {
    "chord": [(0.0, None), (0.05, None), (0.21, 1.0), (0.5, 0.62), (0.8, 0.36), (0.95, 0.2), (1.0, 0.1)],
    "circle_blend": [(0.0, 1.0), (0.05, 1.0), (0.21, 0.0)],
}
FAIRED_ROOT = {
    "chord": [(0.0, 0.8), (0.08, 1.0), (0.5, 0.6), (0.8, 0.34), (0.95, 0.19), (1.0, 0.1)],
    "circle_blend": [(0.0, 0.45), (0.07, 0.0)],
}
THICKNESS_RATIO = [(0.0, 0.45), (0.2, 0.40), (0.4, 0.27), (0.7, 0.20), (1.0, 0.16)]
TWIST_DEGREES = [(0.0, 14.0), (0.2, 13.0), (0.4, 7.0), (0.7, 2.5), (1.0, 0.0)]
PITCH_AXIS_CHORD = 0.3  # the blade turns about this fraction of the chord behind the leading edge
SECTION_POINTS = 14  # per surface, leading and trailing edge included


def airfoil_half(thickness_ratio, upper):
    """One surface of a NACA 4-digit section (2 % camber at 40 %) from trailing to leading edge (upper) or leading to
    trailing edge (lower), as (chord fraction, thickness fraction) with cosine spacing."""
    camber, camber_position = 0.02, 0.4
    points = []
    for index in range(SECTION_POINTS):
        angle = math.pi * index / (SECTION_POINTS - 1)
        if not upper:
            angle = math.pi - angle
        x = (1.0 + math.cos(angle)) / 2.0
        half_thickness = 5.0 * thickness_ratio * (
            0.2969 * math.sqrt(x) - 0.1260 * x - 0.3516 * x ** 2 + 0.2843 * x ** 3 - 0.1036 * x ** 4)
        if x < camber_position:
            mean_line = camber / camber_position ** 2 * (2 * camber_position * x - x ** 2)
        else:
            mean_line = camber / (1 - camber_position) ** 2 * (1 - 2 * camber_position + 2 * camber_position * x - x ** 2)
        if upper:
            points.append((x, mean_line + half_thickness))
        else:
            points.append((x, mean_line - half_thickness))
    return points


def blade_section(model, span_fraction):
    """The closed outline of one blade section in the section plane, as (chord-wise, thickness-wise) metres around the
    pitch axis. Chord-wise points toward the trailing edge, thickness-wise toward the suction (downwind) side."""
    tables = ROUND_ROOT if model.root_style == "round" else FAIRED_ROOT
    root_chord = model.blade_root_diameter / model.blade_max_chord
    chord_table = [(position, root_chord if value is None else value) for position, value in tables["chord"]]
    chord = model.blade_max_chord * interpolate(chord_table, span_fraction)
    circle_weight = interpolate(tables["circle_blend"], span_fraction)
    thickness_ratio = interpolate(THICKNESS_RATIO, span_fraction)
    root_radius = model.blade_root_diameter / 2.0
    surface = airfoil_half(thickness_ratio, upper=True) + airfoil_half(thickness_ratio, upper=False)[1:-1]
    outline = []
    for index, (x, t) in enumerate(surface):
        airfoil_point = ((x - PITCH_AXIS_CHORD) * chord, t * chord)
        angle = math.pi * index / (SECTION_POINTS - 1)  # 0 at the trailing edge, pi at the leading edge, 2 pi back
        circle_point = (root_radius * math.cos(angle), root_radius * math.sin(angle))
        outline.append((
            airfoil_point[0] * (1 - circle_weight) + circle_point[0] * circle_weight,
            airfoil_point[1] * (1 - circle_weight) + circle_point[1] * circle_weight,
        ))
    return outline


def blade_stations(model, blade_length):
    """Span fractions where sections are built: dense at the root and tip, plus the marking band edges."""
    stations = [0.0, 0.02, 0.04, 0.06, 0.09, 0.12, 0.16, 0.21, 0.27, 0.34, 0.42, 0.5, 0.58, 0.66, 0.74, 0.81, 0.87,
                0.92, 0.96, 0.985, 1.0]
    if model.marked:
        for band in (1, 2, 3):
            stations.append(1.0 - band * MARKING_BAND_LENGTH / blade_length)
    return sorted(set(round(station, 5) for station in stations))


def band_material(model, distance_from_tip):
    """Paint slot of the blade surface at a distance from the tip: the outer and the third band are red when marked."""
    if not model.marked:
        return "Paint"
    band = int(distance_from_tip // MARKING_BAND_LENGTH)
    if band in (0, 2):
        return "PaintRed"
    return "Paint"


def place_section(model, outline, span_fraction, root_radius, blade_length):
    """Moves a section outline into the rotor frame for a blade pointing up (+Z), leading edge toward +X."""
    twist = math.radians(interpolate(TWIST_DEGREES, span_fraction))
    # Chord-wise runs from the leading edge (+X, turned upwind by the twist) to the trailing edge.
    chord_direction = (-math.cos(twist), math.sin(twist))
    thickness_direction = (math.sin(twist), math.cos(twist))
    z = root_radius + span_fraction * blade_length
    upwind_bend = -model.prebend * span_fraction ** 2
    return [
        (c * chord_direction[0] + t * thickness_direction[0], c * chord_direction[1] + t * thickness_direction[1] + upwind_bend, z)
        for c, t in outline
    ]


def build_blade(model, root_radius):
    """One blade pointing up from the hub, root flange at root_radius from the hub centre, cone not applied."""
    blade_length = model.rotor_radius - root_radius
    stations = blade_stations(model, blade_length)
    rings = [place_section(model, blade_section(model, station), station, root_radius, blade_length) for station in stations]
    mesh = Mesh()
    point_count = len(rings[0])
    for index in range(len(stations) - 1):
        middle = (stations[index] + stations[index + 1]) / 2.0
        material = band_material(model, (1.0 - middle) * blade_length)
        for i in range(point_count):
            j = (i + 1) % point_count
            quad = [rings[index][i], rings[index][j], rings[index + 1][j], rings[index + 1][i]]
            mesh.add_face(quad, material, smooth=True, desired_normal=outward_of(quad, rings[index]))
    mesh.add_face(rings[0], "Paint", desired_normal=(0.0, 0.0, -1.0))
    tip = rings[-1]
    tip_centre = tuple(sum(point[axis] for point in tip) / len(tip) for axis in range(3))
    tip_point = (tip_centre[0], tip_centre[1], tip_centre[2] + 0.05)
    for i in range(point_count):
        j = (i + 1) % point_count
        mesh.add_face([tip[i], tip[j], tip_point], band_material(model, 0.0), smooth=True,
                      desired_normal=outward_of([tip[i], tip[j]], tip))
    return mesh


def outward_of(points, ring):
    """Direction from a ring's centre to the middle of some of its points, in the XY plane."""
    centre_x = sum(point[0] for point in ring) / len(ring)
    centre_y = sum(point[1] for point in ring) / len(ring)
    middle_x = sum(point[0] for point in points) / len(points)
    middle_y = sum(point[1] for point in points) / len(points)
    return (middle_x - centre_x, middle_y - centre_y, 0.0)


# ================================================================================================= rotor
def build_spinner(model):
    """The nose cone over the hub: an elliptic dome ahead of the hub centre and a short cylinder behind it."""
    radius = model.spinner_diameter / 2.0
    front_length = model.spinner_length * 0.65
    back_length = model.spinner_length - front_length
    profile = []
    steps = 10
    for step in range(steps + 1):
        fraction = step / steps
        y = -front_length + fraction * front_length
        profile.append((y, radius * math.sqrt(max(0.0, 1.0 - (1.0 - fraction) ** 2))))
    profile.append((back_length, radius))
    mesh = Mesh()
    mesh.lathe(profile, "Paint", segments=40)
    return mesh


def build_rotor(model):
    """Spinner and three blades around the hub centre, coned forward, blade one pointing up."""
    rotor = build_spinner(model)
    root_radius = model.spinner_diameter / 2.0 * 0.7
    blade = build_blade(model, root_radius).rotated_x(model.cone_degrees)
    for index in range(3):
        rotor.add(rotated_y(blade, 120.0 * index))
    return rotor


# ================================================================================================= tower
def build_tower(model):
    """Tapered steel tube in flanged sections on a concrete foundation, with a door, steps and a transformer box."""
    mesh = Mesh()
    base_radius = model.tower_base_diameter / 2.0
    top_radius = model.tower_top_diameter / 2.0
    height = model.tower_height

    def radius_at(z):
        return base_radius + (top_radius - base_radius) * z / height

    joints = [height * index / model.tower_sections for index in range(model.tower_sections + 1)]
    if model.green_bands:
        joints = sorted(set(joints + [GREEN_BAND_COUNT * GREEN_BAND_HEIGHT]))
    for z0, z1 in zip(joints, joints[1:]):
        material = "Paint"
        if model.green_bands and z1 <= GREEN_BAND_COUNT * GREEN_BAND_HEIGHT + 1e-6:
            material = "PaintGreen"
        mesh.cylinder(0.0, 0.0, z0, z1, radius_at(z0), material, segments=48, caps="", radius_top=radius_at(z1))
    for z in joints[1:-1]:
        if model.green_bands and abs(z - GREEN_BAND_COUNT * GREEN_BAND_HEIGHT) < 1e-6:
            continue
        mesh.cylinder(0.0, 0.0, z - 0.08, z + 0.08, radius_at(z) + 0.035, "Paint", segments=48)
    mesh.cylinder(0.0, 0.0, height - 0.12, height, top_radius + 0.06, "Paint", segments=48, caps="LH")
    mesh.cylinder(0.0, 0.0, -0.3, 0.25, base_radius + 0.9, "Concrete", segments=48)
    add_door(mesh, radius_at)
    if model.transformer_box:
        add_transformer_box(mesh, base_radius)
    return mesh


STAIR_RISER = 0.22
STAIR_GOING = 0.28
STAIR_HALF_WIDTH = 0.55
RAIL_X = STAIR_HALF_WIDTH + 0.03
HANDRAIL_HEIGHT = 1.0
RAIL_RADIUS = 0.024
DOOR_BOTTOM = 1.1
DOOR_WIDTH = 0.85
DOOR_HEIGHT = 2.1
DOOR_CORNER = 0.2
# The door leaf stands a little proud of the tower skin and the welded frame (collar) around it further still, so
# neither needs a hole cut into the tower.
LEAF_PROUD = 0.012
COLLAR_PROUD = 0.075
COLLAR_WIDTH = 0.13


def on_tower(radius_at, arc, z, proud):
    """Point on the tower surface facing -Y, at an arc length from the front line and a height, pushed out by proud."""
    radius = radius_at(z) + proud
    angle = arc / radius_at(z)
    return (radius * math.sin(angle), -radius * math.cos(angle), z)


def door_outline(width, height, corner):
    """Rounded door outline as (arc, z) around the door's centre line, starting at the bottom middle."""
    outline = rounded_rectangle(width, height, corner, DOOR_BOTTOM, corner_segments=5)
    return [(x, z) for x, z in outline]


def add_door(mesh, radius_at):
    """Steel tower door: a welded collar following the curve of the tower, the leaf with hinges, lever handle, lock
    and a vent grille, then a landing on posts and a steel stair down to the ground."""
    inner = door_outline(DOOR_WIDTH, DOOR_HEIGHT, DOOR_CORNER)
    outer = door_outline(DOOR_WIDTH + 2 * COLLAR_WIDTH, DOOR_HEIGHT + 2 * COLLAR_WIDTH, DOOR_CORNER + COLLAR_WIDTH)
    outer = [(x, z - COLLAR_WIDTH) for x, z in outer]
    add_collar(mesh, radius_at, inner, outer)
    add_leaf(mesh, radius_at, inner)
    add_door_fittings(mesh, radius_at)
    face_y = -radius_at(0.0)
    landing_front = face_y - 1.3
    landing_half = RAIL_X + 0.04
    mesh.box(-landing_half, landing_half, landing_front, face_y + 0.3, DOOR_BOTTOM - 0.06, DOOR_BOTTOM, "Metal")
    for x in (-RAIL_X, RAIL_X):
        mesh.tube((x, landing_front + 0.05, 0.0), (x, landing_front + 0.05, DOOR_BOTTOM - 0.06), 0.04, "Metal", segments=8)
    add_stair(mesh, landing_front, DOOR_BOTTOM, radius_at)


def add_collar(mesh, radius_at, inner, outer):
    """The raised frame between the inner (opening) and outer outlines: front face, outer edge and inner reveal."""
    count = len(inner)
    for i in range(count):
        j = (i + 1) % count
        front = [on_tower(radius_at, *outer[i], COLLAR_PROUD), on_tower(radius_at, *outer[j], COLLAR_PROUD),
                 on_tower(radius_at, *inner[j], COLLAR_PROUD), on_tower(radius_at, *inner[i], COLLAR_PROUD)]
        mesh.add_face(front, "Paint", desired_normal=outward_on_tower(front))
        edge = [on_tower(radius_at, *outer[i], -0.01), on_tower(radius_at, *outer[j], -0.01),
                on_tower(radius_at, *outer[j], COLLAR_PROUD), on_tower(radius_at, *outer[i], COLLAR_PROUD)]
        mesh.add_face(edge, "Paint", desired_normal=away_from(edge, inner_centre(radius_at)))
        reveal = [on_tower(radius_at, *inner[i], LEAF_PROUD), on_tower(radius_at, *inner[j], LEAF_PROUD),
                  on_tower(radius_at, *inner[j], COLLAR_PROUD), on_tower(radius_at, *inner[i], COLLAR_PROUD)]
        mesh.add_face(reveal, "Paint", desired_normal=toward(reveal, inner_centre(radius_at)))


def inner_centre(radius_at):
    """Centre of the door opening on the tower surface."""
    return on_tower(radius_at, 0.0, DOOR_BOTTOM + DOOR_HEIGHT / 2.0, LEAF_PROUD)


def face_middle(points):
    """Average of the points."""
    return tuple(sum(point[k] for point in points) / len(points) for k in range(3))


def away_from(points, centre):
    """Direction from centre to the middle of the points, flattened onto the tower surface (no radial part)."""
    middle = face_middle(points)
    return (middle[0] - centre[0], 0.0, middle[2] - centre[2])


def toward(points, centre):
    """Direction from the middle of the points to centre, flattened onto the tower surface."""
    away = away_from(points, centre)
    return (-away[0], -away[1], -away[2])


def outward_on_tower(points):
    """Radial outward direction of the tower at the middle of the points."""
    middle = face_middle(points)
    return (middle[0], middle[1], 0.0)


def add_leaf(mesh, radius_at, inner):
    """The door leaf: a fan from its centre to the opening outline, curved with the tower."""
    centre = inner_centre(radius_at)
    count = len(inner)
    for i in range(count):
        j = (i + 1) % count
        triangle = [centre, on_tower(radius_at, *inner[i], LEAF_PROUD), on_tower(radius_at, *inner[j], LEAF_PROUD)]
        mesh.add_face(triangle, "Paint", smooth=True, desired_normal=outward_on_tower(triangle))


def add_door_fittings(mesh, radius_at):
    """Three hinges on the left edge, the lever handle and lock on the right, a louvred vent near the bottom."""
    hinge_arc = -DOOR_WIDTH / 2.0 + 0.02
    for height in (0.25, DOOR_HEIGHT / 2.0, DOOR_HEIGHT - 0.3):
        z = DOOR_BOTTOM + height
        bottom = on_tower(radius_at, hinge_arc, z - 0.09, LEAF_PROUD + 0.03)
        top = on_tower(radius_at, hinge_arc, z + 0.09, LEAF_PROUD + 0.03)
        mesh.tube(bottom, top, 0.025, "Metal", segments=8)
    handle_z = DOOR_BOTTOM + 1.05
    handle_arc = DOOR_WIDTH / 2.0 - 0.11
    rosette_root = on_tower(radius_at, handle_arc, handle_z, LEAF_PROUD)
    rosette_tip = on_tower(radius_at, handle_arc, handle_z, LEAF_PROUD + 0.06)
    mesh.tube(rosette_root, rosette_tip, 0.03, "Metal", segments=10)
    lever_end = on_tower(radius_at, handle_arc - 0.13, handle_z, LEAF_PROUD + 0.06)
    mesh.tube(rosette_tip, lever_end, 0.012, "Metal", segments=8)
    lock_root = on_tower(radius_at, handle_arc, handle_z - 0.12, LEAF_PROUD)
    lock_tip = on_tower(radius_at, handle_arc, handle_z - 0.12, LEAF_PROUD + 0.02)
    mesh.tube(lock_root, lock_tip, 0.018, "Metal", segments=10)
    add_vent(mesh, radius_at)


def add_vent(mesh, radius_at):
    """Louvred vent grille in the lower part of the leaf: a frame and slanted slats."""
    half_width = 0.25
    bottom = DOOR_BOTTOM + 0.15
    top = bottom + 0.35
    frame = [(-half_width, bottom), (half_width, bottom), (half_width, top), (-half_width, top), (-half_width, bottom)]
    path = [on_tower(radius_at, arc, z, LEAF_PROUD + 0.012) for arc, z in frame]
    mesh.pipe(path, 0.012, "Metal", segments=6, caps=False)
    slat_count = 6
    for slat in range(slat_count):
        z = bottom + (slat + 0.5) * (top - bottom) / slat_count
        row = []
        for arc in (-half_width, 0.0, half_width):
            row.append((on_tower(radius_at, arc, z + 0.022, LEAF_PROUD), on_tower(radius_at, arc, z - 0.022, LEAF_PROUD + 0.025)))
        for (upper_a, lower_a), (upper_b, lower_b) in zip(row, row[1:]):
            quad = [lower_a, lower_b, upper_b, upper_a]
            mesh.add_face(quad, "Metal", desired_normal=(lower_a[0], lower_a[1], -0.5))


def add_stair(mesh, top_y, top_z, radius_at):
    """Straight steel stair running toward -Y from a landing edge at (top_y, top_z) down to the ground: two sloped
    stringers with the treads between them, and on each side one bent handrail from the ground to the tower wall."""
    riser_count = max(2, round(top_z / STAIR_RISER))
    riser = top_z / riser_count
    run = (riser_count - 1) * STAIR_GOING
    foot_y = top_y - run
    stringer_depth = 0.2
    # Stringer side profile in (y, z): from the foot on the ground up to just under the landing.
    profile = [
        (foot_y - STAIR_GOING, 0.0),
        (foot_y - STAIR_GOING + stringer_depth, 0.0),
        (top_y, top_z - stringer_depth * 1.4),
        (top_y, top_z - 0.06),
    ]
    for x in (-STAIR_HALF_WIDTH - 0.03, STAIR_HALF_WIDTH):
        mesh.prism(profile, "yz", x, x + 0.03, "Metal")
    for step in range(1, riser_count):
        tread_z = step * riser
        tread_front = foot_y - STAIR_GOING + (step - 1) * STAIR_GOING
        mesh.box(-STAIR_HALF_WIDTH, STAIR_HALF_WIDTH, tread_front, tread_front + STAIR_GOING, tread_z - 0.04, tread_z,
                 "Metal")
    foot_post_y = foot_y - STAIR_GOING * 0.5
    rail_z = top_z + HANDRAIL_HEIGHT
    wall_y = -math.sqrt(radius_at(rail_z) ** 2 - RAIL_X ** 2) + 0.01
    for x in (-RAIL_X, RAIL_X):
        mesh.pipe([
            (x, foot_post_y, 0.0),
            (x, foot_post_y, riser + HANDRAIL_HEIGHT),
            (x, top_y, rail_z),
            (x, wall_y, rail_z),
        ], RAIL_RADIUS, "Metal")
        # Post at the landing corner, ending inside the rail.
        mesh.tube((x, top_y + 0.05, top_z), (x, top_y + 0.05, rail_z), RAIL_RADIUS, "Metal", segments=8, caps=False)


def add_transformer_box(mesh, base_radius):
    """The small concrete transformer station next to the tower foot."""
    x0 = base_radius + 3.5
    mesh.box(x0, x0 + 3.0, -1.2, 1.2, 0.0, 2.6, "Concrete")
    mesh.box(x0 - 0.1, x0 + 3.1, -1.3, 1.3, 2.6, 2.75, "Concrete")
    for door_x in (x0 + 0.3, x0 + 1.6):
        mesh.box(door_x, door_x + 1.1, -1.23, -1.2, 0.1, 2.2, "Metal", skip="B")


# ================================================================================================= nacelle
def build_nacelle(model):
    """Yaw bearing, machine housing and roof equipment; the rotor socket is at (0, -overhang, axis height)."""
    mesh = Mesh()
    top_radius = model.tower_top_diameter / 2.0
    mesh.cylinder(0.0, 0.0, 0.0, 0.45, top_radius + 0.08, "Metal", segments=48)
    if model.nacelle_shape == "egg":
        add_egg_housing(mesh, model)
        roof_z = model.axis_height + model.nacelle_height / 2.0 * 0.62
        roof_y = model.nacelle_length * 0.45 - model.overhang
    else:
        roof_z = add_box_housing(mesh, model)
        roof_y = -model.overhang + model.spinner_length * 0.35 + model.nacelle_length * 0.85
    add_roof_equipment(mesh, roof_y, roof_z)
    return mesh


def add_box_housing(mesh, model):
    """Geared nacelle: a long box of rounded section from just behind the spinner backward; returns the roof height."""
    floor = 0.45
    radius = 0.45 if model.nacelle_shape == "box" else 1.25
    radius = min(radius, model.nacelle_width / 2.0 - 0.05)
    outline = rounded_rectangle(model.nacelle_width, model.nacelle_height, radius, floor)
    front = -model.overhang + model.spinner_length * 0.35
    mesh.prism(outline, "xz", front, front + model.nacelle_length, "Paint", smooth_sides=model.nacelle_shape != "box")
    # The main bearing housing between the spinner and the box.
    nose = Mesh()
    nose.lathe([(0.0, model.spinner_diameter * 0.42), (0.35, model.spinner_diameter * 0.46)], "Metal", segments=32)
    mesh.add(nose.rotated_x(-model.tilt_degrees), 0.0, front - 0.35, model.axis_height)
    if model.nacelle_shape == "rounded":
        add_top_cooler(mesh, model, front, floor)
    return floor + model.nacelle_height


def add_top_cooler(mesh, model, front, floor):
    """Nordex-style passive cooler: a slanted fin plate on struts over the rear of the roof."""
    roof = floor + model.nacelle_height
    rear = front + model.nacelle_length
    half = model.nacelle_width / 2.0 - 0.3
    plate = Mesh()
    plate.box(-half, half, 0.0, 3.2, 0.0, 0.12, "Metal")
    mesh.add(plate.rotated_x(12.0), 0.0, rear - 4.0, roof + 0.6)
    for x in (-half + 0.2, half - 0.2):
        for y in (rear - 3.8, rear - 1.0):
            mesh.tube((x, y, roof), (x, y, roof + 0.6 + (y - rear + 4.0) * math.sin(math.radians(12.0))), 0.05, "Metal")


def add_egg_housing(mesh, model):
    """Enercon direct drive: the ring generator right behind the spinner, then the egg-shaped machine housing, all
    tilted with the rotor axis, on a neck over the yaw bearing."""
    generator_radius = model.spinner_diameter / 2.0
    length = model.nacelle_length
    housing_radius = model.nacelle_height / 2.0
    profile = [
        (0.0, generator_radius),
        (0.5, housing_radius * 0.98),
        (1.6, housing_radius),
        (2.6, housing_radius * 0.9),
        (4.2, housing_radius * 0.78),
        (6.2, housing_radius * 0.72),
        (8.2, housing_radius * 0.6),
        (length - 1.0, housing_radius * 0.42),
        (length - 0.35, housing_radius * 0.22),
        (length, 0.0),
    ]
    egg = Mesh()
    egg.lathe(profile, "Paint", segments=48)
    front = -model.overhang + model.spinner_length * 0.3
    mesh.add(egg.translated(0.0, front + model.overhang, 0.0).rotated_x(-model.tilt_degrees), 0.0, -model.overhang,
             model.axis_height)
    top_radius = model.tower_top_diameter / 2.0
    mesh.cylinder(0.0, 0.0, 0.45, model.axis_height - housing_radius * 0.5, top_radius * 0.95, "Paint", segments=40)


def add_roof_equipment(mesh, y, roof_z):
    """Wind vane and anemometer mast with the two red obstruction beacons, at the rear of the roof."""
    for x in (-0.7, 0.7):
        mesh.tube((x, y, roof_z), (x, y, roof_z + 1.2), 0.04, "Metal", segments=6)
        mesh.cylinder(x, y, roof_z + 1.2, roof_z + 1.25, 0.18, "Metal", segments=10)
        mesh.cylinder(x, y, roof_z + 1.25, roof_z + 1.55, 0.13, "Beacon", segments=10, radius_top=0.09)
    mesh.tube((-0.7, y, roof_z + 0.9), (0.7, y, roof_z + 0.9), 0.03, "Metal", segments=6)
    mesh.tube((0.0, y + 0.4, roof_z), (0.0, y + 0.4, roof_z + 1.9), 0.035, "Metal", segments=6)
    for arm in range(3):
        angle = 2 * math.pi * arm / 3
        tip = (0.3 * math.cos(angle), y + 0.4 + 0.3 * math.sin(angle), roof_z + 1.9)
        mesh.tube((0.0, y + 0.4, roof_z + 1.9), tip, 0.012, "Metal", segments=4)
        mesh.cylinder(tip[0], tip[1], roof_z + 1.86, roof_z + 1.94, 0.05, "Metal", segments=8)


# ================================================================================================= kit entry
def build_turbines():
    """All models as pieces <Model>_Tower, <Model>_Nacelle and <Model>_Rotor, and a spec with the sockets the
    runtime and the assembly need."""
    pieces = OrderedDict()
    spec = {}
    for model in MODELS.values():
        pieces[model.name + "_Tower"] = build_tower(model)
        pieces[model.name + "_Nacelle"] = build_nacelle(model)
        pieces[model.name + "_Rotor"] = build_rotor(model)
        spec[model.name] = {
            "tower_height": round(model.tower_height, 3),
            "rotor_socket": [0.0, -model.overhang, model.axis_height],
            "rotor_tilt_degrees": model.tilt_degrees,
            "rotor_diameter": model.rotor_diameter,
            "hub_height": model.hub_height,
            "rated_rpm": model.rated_rpm,
            "tower_clearance": round(tower_clearance(model), 2),
        }
    return pieces, spec


def tower_clearance(model):
    """Gap between the tip of a blade pointing down and the tower surface, the check that the geometry is plausible."""
    cone = math.radians(model.cone_degrees)
    tilt = math.radians(model.tilt_degrees)
    # A blade pointing down in the rotor frame: tip at z = -R cos(cone), y = -R sin(cone) - prebend (upwind is -Y).
    tip_y = -model.rotor_radius * math.sin(cone) - model.prebend
    tip_z = -model.rotor_radius * math.cos(cone)
    # Tilting the front end up turns the lower half of the rotor upwind, away from the tower.
    tip_world_y = -model.overhang + tip_y * math.cos(tilt) + tip_z * math.sin(tilt)
    tip_world_z = model.hub_height - tip_y * math.sin(tilt) + tip_z * math.cos(tilt)
    height_fraction = min(max(tip_world_z, 0.0), model.tower_height) / model.tower_height
    tower_radius = (model.tower_base_diameter + (model.tower_top_diameter - model.tower_base_diameter) * height_fraction) / 2.0
    return -tip_world_y - tower_radius
