"""Bus stops (#102): the urban glass shelter in two lengths, the rural timber shelter, and the stop sign on its pole.

Frame: metres, Z up. A shelter's origin is the middle of its open front edge on the pavement; the open side faces -Y,
toward the road, and the back wall stands at +Y. The sign's origin is the foot of its pole, the sign faces -Y.

The glass shelter's bay width (1.5 m) and the heights are plausible defaults, not measured on Hamburg shelters yet.
"""

from collections import OrderedDict

from .geom import Mesh

BAY_WIDTH = 1.5
GLASS_DEPTH = 1.5
GLASS_ROOF_HEIGHT = 2.35  # underside of the roof frame
POST_SIZE = 0.08
GLASS_THICKNESS = 0.012
SEAT_HEIGHT = 0.45


# ================================================================================================= glass shelter
def glass_pane(mesh, x0, x1, y0, y1, z0, z1):
    """A glass pane as a thin box between two corners, along X or along Y."""
    mesh.box(x0, x1, y0, y1, z0, z1, "Glass")


def add_posts(mesh, half_length):
    """Corner posts at the front and back, and intermediate posts in the back wall at every bay."""
    bay_count = round(2 * half_length / BAY_WIDTH)
    half = POST_SIZE / 2.0
    back_y = GLASS_DEPTH
    for bay in range(bay_count + 1):
        x = -half_length + bay * BAY_WIDTH
        mesh.box(x - half, x + half, back_y - POST_SIZE, back_y, 0.0, GLASS_ROOF_HEIGHT, "PowderCoat")
    for x in (-half_length, half_length):
        mesh.box(x - half, x + half, 0.0, POST_SIZE, 0.0, GLASS_ROOF_HEIGHT, "PowderCoat")


def add_back_wall(mesh, half_length):
    """One glass pane per bay between the posts, with a rail at the foot and under the roof."""
    bay_count = round(2 * half_length / BAY_WIDTH)
    back_y = GLASS_DEPTH - POST_SIZE / 2.0
    for bay in range(bay_count):
        x0 = -half_length + bay * BAY_WIDTH + POST_SIZE / 2.0
        x1 = x0 + BAY_WIDTH - POST_SIZE
        glass_pane(mesh, x0, x1, back_y - GLASS_THICKNESS / 2.0, back_y + GLASS_THICKNESS / 2.0, 0.12, 2.25)
    mesh.box(-half_length, half_length, back_y - 0.025, back_y + 0.025, 0.08, 0.12, "PowderCoat")
    mesh.box(-half_length, half_length, back_y - 0.03, back_y + 0.03, 2.25, GLASS_ROOF_HEIGHT, "PowderCoat")


def add_glass_side(mesh, x):
    """A glass side wall between the front post and the back post at x."""
    y0 = POST_SIZE
    y1 = GLASS_DEPTH - POST_SIZE
    glass_pane(mesh, x - GLASS_THICKNESS / 2.0, x + GLASS_THICKNESS / 2.0, y0, y1, 0.12, 2.25)
    mesh.box(x - 0.025, x + 0.025, y0, y1, 0.08, 0.12, "PowderCoat")
    mesh.box(x - 0.03, x + 0.03, y0, y1, 2.25, GLASS_ROOF_HEIGHT, "PowderCoat")


def add_advert_side(mesh, x):
    """The back-lit advertising case that closes one end: a deep steel case with a poster face on both sides, on two
    feet."""
    case_depth = 0.16
    low_x, high_x = x - case_depth / 2.0, x + case_depth / 2.0
    y0, y1 = POST_SIZE, GLASS_DEPTH - POST_SIZE
    bottom, top = 0.25, GLASS_ROOF_HEIGHT
    mesh.box(low_x, high_x, y0, y1, bottom, top, "PowderCoat")
    inset = 0.07
    for face_x, normal in ((low_x - 0.003, -1.0), (high_x + 0.003, 1.0)):
        poster = [(face_x, y0 + inset, bottom + inset), (face_x, y1 - inset, bottom + inset),
                  (face_x, y1 - inset, top - 0.2), (face_x, y0 + inset, top - 0.2)]
        mesh.add_face(poster, "AdPanel", desired_normal=(normal, 0.0, 0.0))
    for foot_y in (y0 + 0.1, y1 - 0.2):
        mesh.box(low_x + 0.03, high_x - 0.03, foot_y, foot_y + 0.1, 0.0, bottom, "PowderCoat")


def add_glass_roof(mesh, half_length):
    """A frame of steel profiles around a glass roof that overhangs the front, falling slightly toward the back,
    with the stop name strip on the front fascia."""
    overhang_front = 0.35
    overhang_side = 0.1
    x0, x1 = -half_length - overhang_side, half_length + overhang_side
    y0, y1 = -overhang_front, GLASS_DEPTH + 0.05
    bottom = GLASS_ROOF_HEIGHT
    fascia = 0.16
    mesh.box(x0, x1, y0, y0 + 0.06, bottom, bottom + fascia, "PowderCoat")
    mesh.box(x0, x1, y1 - 0.06, y1, bottom, bottom + fascia * 0.8, "PowderCoat")
    mesh.box(x0, x0 + 0.06, y0, y1, bottom, bottom + fascia, "PowderCoat")
    mesh.box(x1 - 0.06, x1, y0, y1, bottom, bottom + fascia, "PowderCoat")
    glass_pane(mesh, x0 + 0.06, x1 - 0.06, y0 + 0.06, y1 - 0.06, bottom + 0.1, bottom + 0.1 + GLASS_THICKNESS)
    name_strip = [(-half_length * 0.6, y0 - 0.003, bottom + 0.03), (half_length * 0.6, y0 - 0.003, bottom + 0.03),
                  (half_length * 0.6, y0 - 0.003, bottom + fascia - 0.03), (-half_length * 0.6, y0 - 0.003, bottom + fascia - 0.03)]
    mesh.add_face(name_strip, "SignFace", desired_normal=(0.0, -1.0, 0.0))
    for x in (x0 + 0.3, 0.0, x1 - 0.3):
        mesh.box(x - 0.03, x + 0.03, 0.0, GLASS_DEPTH, bottom, bottom + 0.1, "PowderCoat")


def add_glass_bench(mesh, half_length):
    """A slatted bench on two brackets, fixed to the back posts."""
    length = min(2 * half_length - 0.6, 2.4)
    back_y = GLASS_DEPTH - POST_SIZE
    seat_depth = 0.38
    for slat in range(4):
        y = back_y - 0.04 - slat * (seat_depth / 4.0)
        mesh.box(-length / 2.0, length / 2.0, y - seat_depth / 4.0 + 0.012, y, SEAT_HEIGHT - 0.035, SEAT_HEIGHT, "Timber")
    for x in (-length / 2.0 + 0.2, length / 2.0 - 0.2):
        mesh.box(x - 0.025, x + 0.025, back_y - seat_depth, back_y, SEAT_HEIGHT - 0.09, SEAT_HEIGHT - 0.035, "PowderCoat")
        mesh.box(x - 0.025, x + 0.025, back_y - 0.06, back_y, 0.0, SEAT_HEIGHT - 0.035, "PowderCoat")


def build_glass_shelter(bay_count):
    """The urban glass shelter: advertising case at the left end (seen from the road), glass at the right."""
    half_length = bay_count * BAY_WIDTH / 2.0
    mesh = Mesh()
    add_posts(mesh, half_length)
    add_back_wall(mesh, half_length)
    add_advert_side(mesh, -half_length)
    add_glass_side(mesh, half_length)
    add_glass_roof(mesh, half_length)
    add_glass_bench(mesh, half_length)
    return mesh


# ================================================================================================= timber shelter
TIMBER_LENGTH = 3.0
TIMBER_DEPTH = 1.6
TIMBER_FRONT_HEIGHT = 2.5
TIMBER_BACK_HEIGHT = 2.2


def add_timber_frame(mesh):
    """Square timber posts at the corners and a head beam along the open front."""
    half = TIMBER_LENGTH / 2.0
    post = 0.12
    for x in (-half, half - post):
        mesh.box(x, x + post, 0.0, post, 0.0, TIMBER_FRONT_HEIGHT, "Timber")
        mesh.box(x, x + post, TIMBER_DEPTH - post, TIMBER_DEPTH, 0.0, TIMBER_BACK_HEIGHT, "Timber")
    mesh.box(-half, half, 0.0, post, TIMBER_FRONT_HEIGHT - 0.2, TIMBER_FRONT_HEIGHT, "Timber")


def add_boarding(mesh):
    """Vertical boards with small gaps on the back wall and both ends, up to the roof line, a hand's width above the
    ground."""
    half = TIMBER_LENGTH / 2.0
    board = 0.14
    gap = 0.012
    thickness = 0.022
    count = int(TIMBER_LENGTH / (board + gap))
    for index in range(count):
        x = -half + index * (board + gap)
        mesh.box(x, x + board, TIMBER_DEPTH - 0.12 - thickness, TIMBER_DEPTH - 0.12, 0.12, TIMBER_BACK_HEIGHT, "Timber")
    side_count = int((TIMBER_DEPTH - 0.24) / (board + gap))
    for x_face in (-half + 0.12, half - 0.12 - thickness):
        for index in range(side_count):
            y = 0.12 + index * (board + gap)
            fraction = (y + board / 2.0) / TIMBER_DEPTH
            top = TIMBER_FRONT_HEIGHT - 0.2 + (TIMBER_BACK_HEIGHT - TIMBER_FRONT_HEIGHT + 0.2) * fraction
            mesh.box(x_face, x_face + thickness, y, y + board, 0.12, top, "Timber")


def add_pent_roof(mesh):
    """A monopitch roof of corrugated sheet on rafters, falling toward the back, overhanging all round."""
    half = TIMBER_LENGTH / 2.0 + 0.2
    front_y, back_y = -0.3, TIMBER_DEPTH + 0.25
    slope = (TIMBER_BACK_HEIGHT - TIMBER_FRONT_HEIGHT) / TIMBER_DEPTH

    def roof_z(y):
        return TIMBER_FRONT_HEIGHT + slope * y

    thickness = 0.04
    sheet = [(-half, front_y, roof_z(front_y) + thickness), (half, front_y, roof_z(front_y) + thickness),
             (half, back_y, roof_z(back_y) + thickness), (-half, back_y, roof_z(back_y) + thickness)]
    mesh.add_face(sheet, "Metal", desired_normal=(0.0, 0.0, 1.0))
    under = [(x, y, z - thickness) for x, y, z in sheet]
    mesh.add_face(under, "Timber", desired_normal=(0.0, 0.0, -1.0))
    for (a, b), normal in (((0, 1), (0.0, -1.0, 0.0)), ((1, 2), (1.0, 0.0, 0.0)), ((2, 3), (0.0, 1.0, 0.0)),
                           ((3, 0), (-1.0, 0.0, 0.0))):
        edge = [under[a], under[b], sheet[b], sheet[a]]
        mesh.add_face(edge, "Metal", desired_normal=normal)
    for x in (-half + 0.15, -0.5, 0.5, half - 0.15):
        rafter = Mesh()
        rafter.box(x - 0.03, x + 0.03, front_y, back_y, -0.12, 0.0, "Timber")
        mesh.add(rafter.transformed(lambda p: (p[0], p[1], p[2] + roof_z(p[1]) - thickness)))


def add_timber_bench(mesh):
    """A plank bench along the back wall on two timber blocks."""
    half = TIMBER_LENGTH / 2.0 - 0.3
    back_y = TIMBER_DEPTH - 0.15
    mesh.box(-half, half, back_y - 0.35, back_y, SEAT_HEIGHT - 0.05, SEAT_HEIGHT, "Timber")
    for x in (-half + 0.15, half - 0.25):
        mesh.box(x, x + 0.1, back_y - 0.3, back_y - 0.05, 0.0, SEAT_HEIGHT - 0.05, "Timber")


def build_timber_shelter():
    """The rural shelter of the marsh villages: timber frame, boarded back and ends, open front, pent roof."""
    mesh = Mesh()
    add_timber_frame(mesh)
    add_boarding(mesh)
    add_pent_roof(mesh)
    add_timber_bench(mesh)
    return mesh


# ================================================================================================= stop sign
SIGN_POLE_HEIGHT = 2.9
SIGN_DIAMETER = 0.42


def build_stop_sign():
    """The stop sign: a pole with the round H sign (Zeichen 224) on top, the stop name plate under it and the
    timetable case at reading height. The sign faces carry the SignFace slot for their printed texture."""
    mesh = Mesh()
    mesh.cylinder(0.0, 0.0, 0.0, SIGN_POLE_HEIGHT, 0.038, "Metal", segments=12)
    disc = Mesh()
    disc.lathe([(-0.012, SIGN_DIAMETER / 2.0), (0.012, SIGN_DIAMETER / 2.0)], "Metal", segments=32)
    mesh.add(disc, 0.0, -0.06, SIGN_POLE_HEIGHT - SIGN_DIAMETER / 2.0 + 0.02)
    face = Mesh()
    face.lathe([(-0.0121, 0.0), (-0.0121, SIGN_DIAMETER / 2.0 - 0.004)], "SignFace", segments=32)
    mesh.add(face, 0.0, -0.06, SIGN_POLE_HEIGHT - SIGN_DIAMETER / 2.0 + 0.02)
    plate_top = SIGN_POLE_HEIGHT - SIGN_DIAMETER - 0.03
    mesh.box(-0.3, 0.3, -0.07, -0.05, plate_top - 0.14, plate_top, "Metal")
    mesh.add_face([(-0.29, -0.0705, plate_top - 0.13), (0.29, -0.0705, plate_top - 0.13), (0.29, -0.0705, plate_top - 0.01),
                   (-0.29, -0.0705, plate_top - 0.01)], "SignFace", desired_normal=(0.0, -1.0, 0.0))
    case_bottom = 1.2
    mesh.box(-0.24, 0.24, -0.11, -0.04, case_bottom, case_bottom + 0.62, "PowderCoat")
    mesh.box(-0.21, 0.21, -0.112, -0.11, case_bottom + 0.03, case_bottom + 0.59, "Glass")
    mesh.add_face([(-0.2, -0.105, case_bottom + 0.04), (0.2, -0.105, case_bottom + 0.04), (0.2, -0.105, case_bottom + 0.58),
                   (-0.2, -0.105, case_bottom + 0.58)], "SignFace", desired_normal=(0.0, -1.0, 0.0))
    for z in (case_bottom + 0.1, case_bottom + 0.52):
        mesh.box(-0.05, 0.05, -0.04, -0.035, z - 0.03, z + 0.03, "Metal")
    return mesh


# ================================================================================================= kit entry
def build():
    """The shelters and the sign as pieces, and a spec with their footprints."""
    pieces = OrderedDict()
    pieces["Glass_2Bay"] = build_glass_shelter(2)
    pieces["Glass_3Bay"] = build_glass_shelter(3)
    pieces["Timber"] = build_timber_shelter()
    pieces["StopSign"] = build_stop_sign()
    spec = {}
    for name, mesh in pieces.items():
        lower, upper = mesh.bounds()
        spec[name] = {"footprint_x": [round(lower[0], 3), round(upper[0], 3)],
                      "footprint_y": [round(lower[1], 3), round(upper[1], 3)]}
    return pieces, spec


TITLES = {"Glass_2Bay": "Glass shelter, 2 bays", "Glass_3Bay": "Glass shelter, 3 bays",
          "Timber": "Timber shelter", "StopSign": "Stop sign"}


def assemblies():
    """Each piece on its own with a close-up from the road side, front left."""
    pieces, _spec = build()
    result = []
    for name, mesh in pieces.items():
        lower, upper = mesh.bounds()
        half_width = max(abs(lower[0]), abs(upper[0]))
        top = upper[2]
        result.append({
            "name": name,
            "title": TITLES[name],
            "parts": [(name, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "XYZ")],
            "half_width": half_width,
            "top": top,
            "extras": None,
            "closeups": [{
                "caption": TITLES[name],
                "target": (0.0, (lower[1] + upper[1]) / 2.0, top * 0.5),
                "direction": (-0.55, -1.0, 0.3),
                "distance": max(half_width * 2.0, top) * 1.75,
                "lens": 35.0,
            }],
        })
    return result
