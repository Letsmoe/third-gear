"""Bus stops (#102): Hamburg's JCDecaux glass shelter in two lengths, the rural timber shelter, the red HVV stop
mast and its bin.

Frame: metres, Z up. A shelter's origin is the middle of its open front edge on the pavement; the open side faces -Y,
toward the road, and the back wall stands at +Y. The sign's origin is the foot of its pole, the sign faces -Y.

The glass shelter follows a photo of the Rathausmarkt stop; its bay width (1.5 m) and heights are estimated from it,
not measured.
"""

from collections import OrderedDict

from .geom import Mesh

BAY_WIDTH = 1.5
GLASS_DEPTH = 1.5
ROOF_BACK_HEIGHT = 2.45  # underside of the roof frame at the back wall; it rises toward the road
ROOF_RISE = 0.15
POST_SIZE = 0.07
GLASS_THICKNESS = 0.012
GLASS_BOTTOM = 0.12
SEAT_HEIGHT = 0.45


def roof_underside(y):
    """Height of the roof frame's underside at a depth y (0 at the open front, GLASS_DEPTH at the back wall)."""
    return ROOF_BACK_HEIGHT + ROOF_RISE * (GLASS_DEPTH - y) / GLASS_DEPTH


def glass_pane(mesh, x0, x1, y0, y1, z0, z1):
    """A glass pane as a thin box between two corners, along X or along Y."""
    mesh.box(x0, x1, y0, y1, z0, z1, "Glass")


def add_posts(mesh, half_length):
    """Slim black posts at the back corners, between the back panes, and at the front corner of the glass end; the
    advertising case carries the other front corner."""
    bay_count = round(2 * half_length / BAY_WIDTH)
    half = POST_SIZE / 2.0
    back_y = GLASS_DEPTH - half
    for bay in range(bay_count + 1):
        x = -half_length + bay * BAY_WIDTH
        mesh.box(x - half, x + half, back_y - half, back_y + half, 0.0, roof_underside(back_y), "PowderCoat")
    mesh.box(half_length - half, half_length + half, half, POST_SIZE + half, 0.0, roof_underside(half), "PowderCoat")


def add_marking_dots(mesh, x0, x1, y, z, facing):
    """The band of red dots on frameless glass that makes it visible, on the side facing -Y or +Y."""
    spacing = 0.12
    size = 0.035
    count = int((x1 - x0) / spacing)
    for dot in range(count):
        x = x0 + (dot + 0.5) * (x1 - x0) / count
        offset = facing * (GLASS_THICKNESS / 2.0 + 0.001)
        square = [(x - size / 2.0, y + offset, z - size / 2.0), (x + size / 2.0, y + offset, z - size / 2.0),
                  (x + size / 2.0, y + offset, z + size / 2.0), (x - size / 2.0, y + offset, z + size / 2.0)]
        mesh.add_face(square, "PaintRed", desired_normal=(0.0, facing, 0.0))


def add_back_wall(mesh, half_length):
    """Frameless panes between the back posts, held by small clamps, clear of the ground, with the dot band."""
    bay_count = round(2 * half_length / BAY_WIDTH)
    back_y = GLASS_DEPTH - POST_SIZE / 2.0
    top = roof_underside(back_y) - 0.08
    for bay in range(bay_count):
        x0 = -half_length + bay * BAY_WIDTH + POST_SIZE / 2.0 + 0.01
        x1 = x0 + BAY_WIDTH - POST_SIZE - 0.02
        glass_pane(mesh, x0, x1, back_y - GLASS_THICKNESS / 2.0, back_y + GLASS_THICKNESS / 2.0, GLASS_BOTTOM, top)
        for x in (x0 + 0.02, x1 - 0.06):
            for z in (GLASS_BOTTOM + 0.1, top - 0.14):
                mesh.box(x, x + 0.04, back_y - 0.02, back_y + 0.02, z, z + 0.04, "Metal")
        for facing in (-1.0, 1.0):
            add_marking_dots(mesh, x0 + 0.05, x1 - 0.05, back_y, 1.45, facing)


def add_glass_end(mesh, x):
    """The framed glass pane that closes the end opposite the advertising case."""
    y0, y1 = POST_SIZE, GLASS_DEPTH - POST_SIZE
    bottom, top = GLASS_BOTTOM, roof_underside(GLASS_DEPTH) - 0.08
    frame = 0.04
    glass_pane(mesh, x - GLASS_THICKNESS / 2.0, x + GLASS_THICKNESS / 2.0, y0, y1, bottom, top)
    for y_low, y_high, z_low, z_high in ((y0, y1, bottom - frame, bottom), (y0, y1, top, top + frame),
                                         (y0, y0 + frame, bottom, top), (y1 - frame, y1, bottom, top)):
        mesh.box(x - 0.02, x + 0.02, y_low, y_high, z_low, z_high, "PowderCoat")
    for y in (y0 + 0.2, y1 - 0.2):
        mesh.box(x - 0.015, x + 0.015, y - 0.015, y + 0.015, 0.0, bottom - frame, "PowderCoat")


def add_advert_case(mesh, x):
    """The back-lit advertising case that closes one end: a black steel case with a poster on both faces, standing
    on two feet."""
    case_depth = 0.14
    low_x, high_x = x - case_depth / 2.0, x + case_depth / 2.0
    y0, y1 = -0.05, GLASS_DEPTH - POST_SIZE
    bottom, top = 0.3, 2.3
    mesh.box(low_x, high_x, y0, y1, bottom, top, "PowderCoat")
    inset = 0.06
    for face_x, normal in ((low_x - 0.003, -1.0), (high_x + 0.003, 1.0)):
        poster = [(face_x, y0 + inset, bottom + inset), (face_x, y1 - inset, bottom + inset),
                  (face_x, y1 - inset, top - inset), (face_x, y0 + inset, top - inset)]
        mesh.add_face(poster, "AdPanel", desired_normal=(normal, 0.0, 0.0))
    for foot_y in (y0 + 0.15, y1 - 0.25):
        mesh.box(low_x + 0.03, high_x - 0.03, foot_y, foot_y + 0.1, 0.0, bottom, "PowderCoat")


def add_glass_roof(mesh, half_length):
    """A thin black frame around a glass roof, overhanging the front far and the sides a little, rising toward the
    road, on a beam over every post line."""
    roof = Mesh()
    front, back = -0.75, GLASS_DEPTH + 0.05
    x0, x1 = -half_length - 0.15, half_length + 0.15
    depth = 0.08
    roof.box(x0, x1, front, front + 0.06, 0.0, depth, "PowderCoat")
    roof.box(x0, x1, back - 0.06, back, 0.0, depth, "PowderCoat")
    roof.box(x0, x0 + 0.06, front, back, 0.0, depth, "PowderCoat")
    roof.box(x1 - 0.06, x1, front, back, 0.0, depth, "PowderCoat")
    bay_count = round(2 * half_length / BAY_WIDTH)
    for bay in range(bay_count + 1):
        x = -half_length + bay * BAY_WIDTH
        roof.box(x - 0.03, x + 0.03, front, back, -0.04, depth, "PowderCoat")
    glass_pane(roof, x0 + 0.06, x1 - 0.06, front + 0.06, back - 0.06, depth - 0.02, depth - 0.02 + GLASS_THICKNESS)
    # Shear the flat roof so its underside follows roof_underside().
    mesh.add(roof.transformed(lambda p: (p[0], p[1], p[2] + roof_underside(p[1]))))


def add_steel_bench(mesh, half_length):
    """A black steel bench along the back wall: a perforated seat with arm dividers on two brackets, and a lean rail
    above it on the glass."""
    length = min(2 * half_length - 0.8, 1.9)
    back_y = GLASS_DEPTH - POST_SIZE
    seat_depth = 0.32
    left = -half_length + 0.5
    mesh.box(left, left + length, back_y - seat_depth, back_y - 0.02, SEAT_HEIGHT - 0.03, SEAT_HEIGHT, "PowderCoat")
    for x in (left + length / 3.0, left + 2.0 * length / 3.0):
        mesh.box(x - 0.015, x + 0.015, back_y - seat_depth + 0.04, back_y - 0.04, SEAT_HEIGHT, SEAT_HEIGHT + 0.2,
                 "PowderCoat")
    for x in (left + 0.15, left + length - 0.15):
        mesh.box(x - 0.02, x + 0.02, back_y - seat_depth + 0.05, back_y, SEAT_HEIGHT - 0.1, SEAT_HEIGHT - 0.03,
                 "PowderCoat")
        mesh.box(x - 0.02, x + 0.02, back_y - 0.05, back_y, 0.0, SEAT_HEIGHT - 0.03, "PowderCoat")
    mesh.box(left, left + length, back_y - 0.05, back_y - 0.01, 0.82, 0.86, "PowderCoat")


def add_info_case(mesh, half_length):
    """The information case with the network map and timetables, fixed to the back glass at the glass end."""
    width, height = 0.85, 1.15
    right = half_length - 0.12
    left = right - width
    bottom = 1.0
    back_y = GLASS_DEPTH - POST_SIZE / 2.0 - GLASS_THICKNESS / 2.0
    mesh.box(left, right, back_y - 0.06, back_y, bottom, bottom + height, "PowderCoat")
    face = [(left + 0.04, back_y - 0.061, bottom + 0.04), (right - 0.04, back_y - 0.061, bottom + 0.04),
            (right - 0.04, back_y - 0.061, bottom + height - 0.04), (left + 0.04, back_y - 0.061, bottom + height - 0.04)]
    mesh.add_face(face, "SignFace", desired_normal=(0.0, -1.0, 0.0))


def build_glass_shelter(bay_count):
    """Hamburg's JCDecaux shelter: advertising case at the left end (seen from the road), a framed pane at the right,
    frameless back glass with the information case, a glass roof overhanging the kerb and a steel bench."""
    half_length = bay_count * BAY_WIDTH / 2.0
    mesh = Mesh()
    add_posts(mesh, half_length)
    add_back_wall(mesh, half_length)
    add_advert_case(mesh, -half_length)
    add_glass_end(mesh, half_length)
    add_glass_roof(mesh, half_length)
    add_steel_bench(mesh, half_length)
    add_info_case(mesh, half_length)
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
MAST_HEIGHT = 3.5
SIGN_SIZE = 0.45


def sign_plate(mesh, x0, x1, z0, z1, y_front=-0.08, thickness=0.015):
    """A flat plate facing -Y whose front carries a printed face (SignFace) inside a thin metal edge."""
    mesh.box(x0, x1, y_front, y_front + thickness, z0, z1, "Metal")
    face = [(x0 + 0.01, y_front - 0.0005, z0 + 0.01), (x1 - 0.01, y_front - 0.0005, z0 + 0.01),
            (x1 - 0.01, y_front - 0.0005, z1 - 0.01), (x0 + 0.01, y_front - 0.0005, z1 - 0.01)]
    mesh.add_face(face, "SignFace", desired_normal=(0.0, -1.0, 0.0))


def build_stop_sign():
    """The red HVV stop mast: the square H sign (Zeichen 224) at the top, the stop name plate and a stack of line
    plates under it, on brackets. It faces -Y; the runtime turns it toward the oncoming traffic."""
    mesh = Mesh()
    mesh.cylinder(0.0, 0.0, 0.0, MAST_HEIGHT, 0.05, "PaintRed", segments=16)
    mesh.cylinder(0.0, 0.0, MAST_HEIGHT, MAST_HEIGHT + 0.02, 0.052, "Metal", segments=16)
    sign_top = MAST_HEIGHT - 0.05
    sign_plate(mesh, -SIGN_SIZE / 2.0, SIGN_SIZE / 2.0, sign_top - SIGN_SIZE, sign_top)
    name_top = sign_top - SIGN_SIZE - 0.03
    sign_plate(mesh, -0.3, 0.3, name_top - 0.13, name_top)
    line_top = name_top - 0.16
    for line in range(3):
        top = line_top - line * 0.105
        sign_plate(mesh, -0.3, 0.3, top - 0.095, top)
    for z in (sign_top - 0.1, sign_top - SIGN_SIZE + 0.1, name_top - 0.06, line_top - 0.15):
        mesh.box(-0.06, 0.06, -0.065, -0.04, z - 0.025, z + 0.025, "Metal")
    return mesh


def build_stop_bin():
    """The red HVV litter bin on its own short post, which stands next to the mast."""
    mesh = Mesh()
    mesh.cylinder(0.0, 0.0, 0.0, 0.75, 0.035, "PaintRed", segments=12)
    mesh.box(-0.18, 0.18, -0.2, 0.08, 0.45, 1.05, "PaintRed")
    mesh.box(-0.16, 0.16, -0.202, -0.2, 0.92, 0.99, "SignFace")
    mesh.box(-0.12, 0.12, -0.205, -0.2, 0.8, 0.86, "PowderCoat")
    return mesh


# ================================================================================================= kit entry
def build():
    """The shelters and the sign as pieces, and a spec with their footprints."""
    pieces = OrderedDict()
    pieces["Glass_2Bay"] = build_glass_shelter(2)
    pieces["Glass_3Bay"] = build_glass_shelter(3)
    pieces["Timber"] = build_timber_shelter()
    pieces["StopSign"] = build_stop_sign()
    pieces["StopBin"] = build_stop_bin()
    spec = {}
    for name, mesh in pieces.items():
        lower, upper = mesh.bounds()
        spec[name] = {"footprint_x": [round(lower[0], 3), round(upper[0], 3)],
                      "footprint_y": [round(lower[1], 3), round(upper[1], 3)]}
    return pieces, spec


TITLES = {"Glass_2Bay": "Glass shelter, 2 bays", "Glass_3Bay": "Glass shelter, 3 bays",
          "Timber": "Timber shelter", "StopSign": "Stop mast", "StopBin": "HVV bin"}


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
