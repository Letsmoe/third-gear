"""Bus stops (#102): Hamburg's JCDecaux glass shelter in two lengths, the rural timber shelter, the red HVV stop
mast with its timetable case and street bin.

Frame: metres, Z up. A shelter's origin is the middle of its open front edge on the pavement; the open side faces -Y,
toward the road, and the back wall stands at +Y. The sign's origin is the foot of its pole, the sign faces -Y.

The glass shelter follows a photo of the Rathausmarkt stop; its bay width (1.5 m) and heights are estimated from it,
not measured.
"""

import math
import os
from collections import OrderedDict

from .geom import Mesh, smooth_noise

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


POSTER_WIDTH, POSTER_HEIGHT = 1.16, 1.71  # visible area of a CityLight poster
FRONT_UVS = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]  # for corners listed left, right, top right, top left
MIRRORED_UVS = [(1.0, 0.0), (0.0, 0.0), (0.0, 1.0), (1.0, 1.0)]


def add_advert_case(mesh, x):
    """The back-lit advertising case that closes one end: a black steel case with a CityLight poster on both faces,
    standing on two feet against the back post. Each poster has 0 to 1 UVs, upright as seen from its side."""
    case_depth = 0.14
    frame = 0.06
    low_x, high_x = x - case_depth / 2.0, x + case_depth / 2.0
    y1 = GLASS_DEPTH - POST_SIZE
    y0 = y1 - POSTER_WIDTH - 2 * frame
    bottom = 0.3
    top = bottom + POSTER_HEIGHT + 2 * frame
    mesh.box(low_x, high_x, y0, y1, bottom, top, "PowderCoat")
    for face_x, normal in ((low_x - 0.003, -1.0), (high_x + 0.003, 1.0)):
        poster = [(face_x, y0 + frame, bottom + frame), (face_x, y1 - frame, bottom + frame),
                  (face_x, y1 - frame, top - frame), (face_x, y0 + frame, top - frame)]
        # Seen from -X, right is -Y, so the poster's U runs against Y there.
        uvs = MIRRORED_UVS if normal < 0 else FRONT_UVS
        mesh.add_face(poster, "AdPanel", desired_normal=(normal, 0.0, 0.0), uvs=uvs)
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
    """The information case with the timetable poster (A-format, 0 to 1 UVs), fixed to the back glass at the glass
    end."""
    poster_width, poster_height = 0.8, 1.13
    frame = 0.04
    right = half_length - 0.12
    left = right - poster_width - 2 * frame
    bottom = 0.95
    top = bottom + poster_height + 2 * frame
    back_y = GLASS_DEPTH - POST_SIZE / 2.0 - GLASS_THICKNESS / 2.0
    mesh.box(left, right, back_y - 0.06, back_y, bottom, top, "PowderCoat")
    face_y = back_y - 0.061
    face = [(left + frame, face_y, bottom + frame), (right - frame, face_y, bottom + frame),
            (right - frame, face_y, top - frame), (left + frame, face_y, top - frame)]
    mesh.add_face(face, "Timetable", desired_normal=(0.0, -1.0, 0.0), uvs=FRONT_UVS)


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


# ================================================================================================= stop mast
MAST_HEIGHT = 3.6
MAST_RADIUS = 0.045
SIGN_PANEL = (0.44, 0.78)  # width and height of the white stop panel beside the mast top


def add_sign_panel(mesh):
    """The white stop panel fixed beside the top of the mast: Zeichen 224, the stop name, the HVV mark and the line
    strip are all printed on it (StopSignFace), on both sides, upright from each side."""
    width, height = SIGN_PANEL
    right = -MAST_RADIUS - 0.005
    left = right - width
    top = MAST_HEIGHT - 0.02
    bottom = top - height
    mesh.box(left, right, -0.012, 0.012, bottom, top, "Metal")
    front = [(left + 0.008, -0.0125, bottom + 0.008), (right - 0.008, -0.0125, bottom + 0.008),
             (right - 0.008, -0.0125, top - 0.008), (left + 0.008, -0.0125, top - 0.008)]
    mesh.add_face(front, "StopSignFace", desired_normal=(0.0, -1.0, 0.0), uvs=FRONT_UVS)
    back = [(x, 0.0125, z) for x, _y, z in front]
    mesh.add_face(back, "StopSignFace", desired_normal=(0.0, 1.0, 0.0), uvs=MIRRORED_UVS)
    for z in (top - 0.1, bottom + 0.1):
        mesh.box(right - 0.02, MAST_RADIUS + 0.01, -0.03, 0.03, z - 0.03, z + 0.03, "Metal")


def add_timetable_case(mesh):
    """The small red timetable case on the front of the mast at reading height, with an A-format timetable."""
    poster_width, poster_height = 0.29, 0.41
    frame = 0.025
    left, right = -poster_width / 2.0 - frame, poster_width / 2.0 + frame
    bottom = 1.35
    top = bottom + poster_height + 2 * frame
    front_y = -MAST_RADIUS - 0.05
    mesh.box(left, right, front_y, -MAST_RADIUS + 0.005, bottom, top, "PaintRed")
    face = [(left + frame, front_y - 0.001, bottom + frame), (right - frame, front_y - 0.001, bottom + frame),
            (right - frame, front_y - 0.001, top - frame), (left + frame, front_y - 0.001, top - frame)]
    mesh.add_face(face, "Timetable", desired_normal=(0.0, -1.0, 0.0), uvs=FRONT_UVS)


def rounded_outline(half_width, half_depth, radius, centre_y, corner_segments=4):
    """Counter-clockwise (x, y) outline of a rectangle with rounded corners, centred on (0, centre_y)."""
    corners = [(half_width - radius, centre_y - half_depth + radius, -90.0),
               (half_width - radius, centre_y + half_depth - radius, 0.0),
               (-half_width + radius, centre_y + half_depth - radius, 90.0),
               (-half_width + radius, centre_y - half_depth + radius, 180.0)]
    outline = []
    for corner_x, corner_y, start in corners:
        for step in range(corner_segments + 1):
            angle = math.radians(start + 90.0 * step / corner_segments)
            outline.append((corner_x + radius * math.cos(angle), corner_y + radius * math.sin(angle)))
    return outline


BIN_WIDTH, BIN_DEPTH, BIN_BOTTOM, BIN_TOP = 0.44, 0.3, 0.32, 1.06
BIN_CORNER_RADIUS = 0.07
BIN_EDGE_RADIUS = 0.035
BIN_WALL = 0.02
# The inner container stays clear of the rounded corners of the shell.
BIN_INNER_HALF_WIDTH = BIN_WIDTH / 2.0 - 0.06
BIN_OPENING_HALF_WIDTH = 0.12
BIN_OPENING_BOTTOM, BIN_OPENING_TOP = BIN_TOP - 0.21, BIN_TOP - 0.065
# Fits the flat front between the rounded corners, in the 760 by 320 aspect of the sticker images.
STICKER = (0.3, 0.126)


def bin_profile(edge_steps=4):
    """(z, inset) of the bin body's horizontal rings from the foot to the lid: quarter rounds at the bottom and top
    edges, and rings at the opening's sill and lintel so the opening can be cut along them."""
    profile = []
    for step in range(edge_steps + 1):
        angle = 0.5 * math.pi * step / edge_steps
        profile.append((BIN_BOTTOM + BIN_EDGE_RADIUS * (1 - math.cos(angle)), BIN_EDGE_RADIUS * (1 - math.sin(angle))))
    profile += [(BIN_OPENING_BOTTOM, 0.0), (BIN_OPENING_TOP, 0.0)]
    for step in range(edge_steps + 1):
        angle = 0.5 * math.pi * step / edge_steps
        profile.append((BIN_TOP - BIN_EDGE_RADIUS + BIN_EDGE_RADIUS * math.sin(angle),
                        BIN_EDGE_RADIUS * (1 - math.cos(angle))))
    return profile


def bin_ring(z, inset, centre_y):
    """One ring of the bin body: the rounded outline shrunk by inset, with two extra points on the front edge where
    the opening's jambs are. The segment between them is the last one."""
    half_width, half_depth = BIN_WIDTH / 2.0 - inset, BIN_DEPTH / 2.0 - inset
    outline = rounded_outline(half_width, half_depth, BIN_CORNER_RADIUS - inset, centre_y)
    front_y = centre_y - half_depth
    outline += [(-BIN_OPENING_HALF_WIDTH, front_y), (BIN_OPENING_HALF_WIDTH, front_y)]
    return [(x, y, z) for x, y in outline]


def add_bin_body(mesh, centre_y):
    """The bin's red shell as one smooth loft with rounded foot and lid edges, leaving out the throw-in opening."""
    profile = bin_profile()
    rings = [bin_ring(z, inset, centre_y) for z, inset in profile]
    opening_segment = len(rings[0]) - 2
    for index in range(len(rings) - 1):
        lower, upper = rings[index], rings[index + 1]
        in_opening = abs(profile[index][0] - BIN_OPENING_BOTTOM) < 1e-9
        for point_index in range(len(lower)):
            if in_opening and point_index == opening_segment:
                continue
            following = (point_index + 1) % len(lower)
            quad = [lower[point_index], lower[following], upper[following], upper[point_index]]
            middle_x = sum(point[0] for point in quad) / 4.0
            middle_y = sum(point[1] for point in quad) / 4.0
            mesh.add_face(quad, "PaintRed", smooth=True, desired_normal=(middle_x, middle_y - centre_y, 0.0))
    mesh.add_face(rings[0], "PaintRed", desired_normal=(0.0, 0.0, -1.0))
    mesh.add_face(rings[-1], "PaintRed", desired_normal=(0.0, 0.0, 1.0))


def add_bin_opening(mesh, front_y, back_y):
    """The wall thickness around the throw-in opening, and the dark inside of the bin behind it."""
    half_width = BIN_OPENING_HALF_WIDTH
    inner_y = front_y + BIN_WALL
    bottom, top = BIN_OPENING_BOTTOM, BIN_OPENING_TOP
    mesh.add_face([(-half_width, front_y, bottom), (half_width, front_y, bottom), (half_width, inner_y, bottom),
                   (-half_width, inner_y, bottom)], "PaintRed", desired_normal=(0.0, 0.0, 1.0))
    mesh.add_face([(-half_width, front_y, top), (half_width, front_y, top), (half_width, inner_y, top),
                   (-half_width, inner_y, top)], "PaintRed", desired_normal=(0.0, 0.0, -1.0))
    for side in (-1.0, 1.0):
        x = side * half_width
        mesh.add_face([(x, front_y, bottom), (x, inner_y, bottom), (x, inner_y, top), (x, front_y, top)], "PaintRed",
                      desired_normal=(-side, 0.0, 0.0))
    # The galvanised inner container, faces turned inward; the rubbish covers its floor.
    inner_half_width = BIN_INNER_HALF_WIDTH
    floor, ceiling = BIN_OPENING_BOTTOM - 0.15, BIN_TOP - BIN_WALL
    mesh.box(-inner_half_width, inner_half_width, inner_y, back_y - BIN_WALL, floor, ceiling, "Metal", skip="FD")
    for face in mesh.faces[-4:]:
        face["points"].reverse()
        face["uvs"].reverse()
    around_opening = [(-inner_half_width, -half_width, floor, ceiling), (half_width, inner_half_width, floor, ceiling),
                      (-half_width, half_width, floor, bottom), (-half_width, half_width, top, ceiling)]
    for left, right, lower, upper in around_opening:
        mesh.add_face([(left, inner_y, lower), (right, inner_y, lower), (right, inner_y, upper), (left, inner_y, upper)],
                      "Metal", desired_normal=(0.0, 1.0, 0.0))


def rubbish_height(x, y, front_y):
    """Height of the heap in the bag: just under the sill, piled up behind the opening where things land."""
    behind_opening = math.exp(-((x / 0.14) ** 2) - (((y - front_y - 0.1) / 0.1) ** 2))
    return BIN_OPENING_BOTTOM - 0.05 + 0.04 * behind_opening + 0.012 * smooth_noise(x, y, 7, 18.0)


def crumpled_ball(radius, seed):
    """A crumpled paper ball: a coarse sphere with every point pushed in or out by noise."""
    ball = Mesh()
    steps = 6
    profile = [(-radius * math.cos(math.pi * step / steps), radius * math.sin(math.pi * step / steps))
               for step in range(steps + 1)]
    ball.lathe(profile, "Litter", segments=9)

    def crumple(point):
        """Scales a point along its direction from the centre by a noise factor."""
        factor = 1.0 + 0.28 * smooth_noise(point[0] + 0.7 * point[2], point[1] - 0.4 * point[2], seed, 1.6 / radius)
        return (point[0] * factor, point[1] * factor, point[2] * factor)

    return ball.transformed(crumple)


def lying(item, around_z_degrees, x, y, z):
    """Lays an item built along +Y on its side, turned about Z, with its middle at (x, y, z)."""
    return item.rotated_z(around_z_degrees).translated(x, y, z)


def add_rubbish(mesh, front_y, back_y):
    """Stand-in rubbish behind the opening: the black bag's surface, paper balls, a coffee cup, a can and a bottle."""
    inner_half_width = BIN_INNER_HALF_WIDTH
    inner_front, inner_back = front_y + BIN_WALL, back_y - BIN_WALL
    mesh.heightfield(-inner_half_width, inner_half_width, inner_front, inner_back, 8, 6,
                     lambda x, y: rubbish_height(x, y, front_y), "BinBag")

    def on_heap(x, y, lift):
        """Height at which an item resting on the heap has its middle."""
        return rubbish_height(x, y, front_y) + lift

    mesh.add(crumpled_ball(0.035, 1), -0.07, front_y + 0.08, on_heap(-0.07, front_y + 0.08, 0.025))
    mesh.add(crumpled_ball(0.028, 2), 0.09, front_y + 0.13, on_heap(0.09, front_y + 0.13, 0.02))
    cup = Mesh()
    cup.lathe([(-0.045, 0.028), (0.045, 0.04), (0.05, 0.042)], "Litter", segments=12)
    mesh.add(lying(cup, 70.0, 0.02, front_y + 0.12, on_heap(0.02, front_y + 0.12, 0.03)))
    can = Mesh()
    can.lathe([(-0.058, 0.026), (-0.054, 0.033), (0.05, 0.033), (0.058, 0.027)], "Metal", segments=12)
    mesh.add(lying(can, -25.0, -0.1, front_y + 0.18, on_heap(-0.1, front_y + 0.18, 0.025)))
    bottle = Mesh()
    bottle.lathe([(-0.11, 0.0), (-0.11, 0.032), (0.03, 0.032), (0.07, 0.014), (0.1, 0.013), (0.1, 0.0)], "Glass",
                 segments=12)
    mesh.add(lying(bottle, 15.0, 0.08, front_y + 0.21, on_heap(0.08, front_y + 0.21, 0.025)))


def add_street_bin(mesh):
    """Hamburg's red street bin on the front of the mast: a shell with rounded edges, a throw-in opening across the
    top of its front with the rubbish visible inside, and the joke sticker (BinSticker) below it."""
    front_y = -MAST_RADIUS - 0.01 - BIN_DEPTH
    back_y = front_y + BIN_DEPTH
    add_bin_body(mesh, front_y + BIN_DEPTH / 2.0)
    add_bin_opening(mesh, front_y, back_y)
    add_rubbish(mesh, front_y, back_y)
    sticker_width, sticker_height = STICKER
    sticker_top = BIN_OPENING_BOTTOM - 0.05
    sticker = [(-sticker_width / 2.0, front_y - 0.002, sticker_top - sticker_height),
               (sticker_width / 2.0, front_y - 0.002, sticker_top - sticker_height),
               (sticker_width / 2.0, front_y - 0.002, sticker_top), (-sticker_width / 2.0, front_y - 0.002, sticker_top)]
    mesh.add_face(sticker, "BinSticker", desired_normal=(0.0, -1.0, 0.0), uvs=FRONT_UVS)
    for z in (BIN_BOTTOM + 0.15, BIN_TOP - 0.15):
        mesh.box(-0.06, 0.06, -MAST_RADIUS - 0.012, -MAST_RADIUS + 0.01, z - 0.03, z + 0.03, "Metal")


def build_stop_sign():
    """The red HVV stop mast as photographed at Roßweg: the white stop panel beside its top, the red timetable case at
    reading height and the street bin below. It faces -Y; the runtime turns it toward the oncoming traffic."""
    mesh = Mesh()
    mesh.cylinder(0.0, 0.0, 0.0, MAST_HEIGHT, MAST_RADIUS, "PaintRed", segments=16)
    mesh.cylinder(0.0, 0.0, MAST_HEIGHT, MAST_HEIGHT + 0.02, MAST_RADIUS + 0.003, "PaintRed", segments=16)
    add_sign_panel(mesh)
    add_timetable_case(mesh)
    add_street_bin(mesh)
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


def preview_textures():
    """Images for the contact sheet's preview materials, from <data root>/building_kit/posters (posters/README)."""
    posters = os.path.join(os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear"), "building_kit", "posters")
    return {
        "AdPanel": os.path.join(posters, "ad_bus_drivers.png"),
        "Timetable": os.path.join(posters, "timetable.png"),
        "StopSignFace": os.path.join(posters, "stop_panel.png"),
        "BinSticker": os.path.join(posters, "bin_sticker_0.png"),
    }


TITLES = {"Glass_2Bay": "Glass shelter, 2 bays", "Glass_3Bay": "Glass shelter, 3 bays",
          "Timber": "Timber shelter", "StopSign": "Stop mast"}


# Extra close-ups of the printed faces, per piece.
DETAIL_CLOSEUPS = {
    "Glass_2Bay": [{"caption": "Advertising case", "target": (-1.6, 0.75, 1.25), "direction": (-1.0, -0.3, 0.05),
                    "distance": 3.4, "lens": 35.0},
                   {"caption": "Information case", "target": (0.95, 1.3, 1.5), "direction": (-0.2, -1.0, 0.0),
                    "distance": 2.2, "lens": 35.0}],
    "StopSign": [{"caption": "Stop panel", "target": (-0.27, 0.0, 3.2), "direction": (-0.15, -1.0, -0.2),
                  "distance": 2.0, "lens": 35.0},
                 {"caption": "Timetable and bin", "target": (0.0, -0.2, 1.2), "direction": (-0.35, -1.0, 0.1),
                  "distance": 2.3, "lens": 35.0},
                 {"caption": "Into the bin", "target": (0.0, -0.22, 0.86), "direction": (-0.35, -1.0, 0.6),
                  "distance": 1.25, "lens": 45.0}],
}


def assemblies():
    """Each piece on its own with a close-up from the road side, front left, plus close-ups of the printed faces."""
    pieces, _spec = build()
    result = []
    for name, mesh in pieces.items():
        lower, upper = mesh.bounds()
        half_width = max(abs(lower[0]), abs(upper[0]))
        top = upper[2]
        overview = {
            "caption": TITLES[name],
            "target": (0.0, (lower[1] + upper[1]) / 2.0, top * 0.5),
            "direction": (-0.55, -1.0, 0.3),
            "distance": max(half_width * 2.0, top) * 1.75,
            "lens": 35.0,
        }
        result.append({
            "name": name,
            "title": TITLES[name],
            "parts": [(name, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "XYZ")],
            "half_width": half_width,
            "top": top,
            "extras": None,
            "closeups": [overview] + DETAIL_CLOSEUPS.get(name, []),
        })
    return result
