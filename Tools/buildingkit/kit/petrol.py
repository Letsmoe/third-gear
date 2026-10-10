"""Petrol stations (#105): the pieces of a German filling station and the rule that lays them out on a plot.

Frame: metres, Z up, the road at -Y. Pieces:
- Canopy_2, Canopy_3, Canopy_4: the roof over two to four pump islands; origin the centre of its footprint on the
  ground, the islands run along X and follow each other along Y, one column on each island's centre.
- PumpIsland: the raised island with two dispensers, end guards and a bin; origin its centre, long along X.
- PricePylon: the price totem at the road; origin its foot, its two faces look along -Y and +Y, so the runtime turns
  it 90 degrees to face the traffic.
- Shop: the shop with its glass front; origin the middle of the front wall's foot, the front faces -Y.
- CarWash: the wash hall with an open portal at each end; origin the middle of the entrance, the hall runs to +Y.
- AirVacuum: the air and vacuum bay on its small island; origin its centre.

Brand surfaces are slots of their own (BrandPaint, BrandLogo, PriceBoard, PumpFace, WashSign); a brand is a set of
material instances for them, so every piece serves all brands. Clear height and spacing follow common German
practice (4.8 m under the canopy, islands 7.5 m apart); the rest is estimated from photos.
"""

import math
import os
from collections import OrderedDict

from .geom import Mesh, vec_cross
from .shelters import FRONT_UVS, rounded_outline

CANOPY_CLEAR = 4.8
FASCIA_HEIGHT = 0.9
LOGO_BOX = (3.6, 1.35, 0.2)  # width, height and depth of the raised logo box on the fascia
CANOPY_LENGTH = 14.0
ISLAND_SPACING = 7.5
ISLAND_LENGTH, ISLAND_WIDTH, ISLAND_HEIGHT = 6.6, 1.3, 0.15
DISPENSER_X = 1.9
LOGO_PANEL = (3.2, 0.8)  # the 4:1 logo image
PYLON_HEIGHT = 7.2
PRICE_BOARD = (1.5, 5.6)
SHOP_WIDTH, SHOP_DEPTH, SHOP_HEIGHT = 16.0, 10.0, 4.8
SHOP_GLASS_RIGHT = 3.0  # the glass front runs from the left wall to here; the storeroom behind the rest is solid
SHOP_FASCIA_BOTTOM = 3.4
SHOP_DOOR_X = -2.5
WASH_WIDTH, WASH_LENGTH, WASH_HEIGHT = 6.0, 12.0, 4.5
WASH_OPENING = (3.6, 3.4)
WALL = 0.3

BRANDS = ("nordtank", "hopp", "vierlaender")
BRAND_COLOURS = {"nordtank": (0.0, 0.07, 0.32, 1.0), "hopp": (0.70, 0.01, 0.01, 1.0),
                 "vierlaender": (0.0, 0.13, 0.04, 1.0)}


def printed_face(mesh, centre, normal, width, bottom, top, material):
    """A printed face with 0 to 1 UVs, upright, centred on centre's X and Y and looking along a horizontal normal;
    the image reads the right way round from that side."""
    right = vec_cross((0.0, 0.0, 1.0), normal)
    half = width / 2.0
    left_point = (centre[0] - right[0] * half, centre[1] - right[1] * half)
    right_point = (centre[0] + right[0] * half, centre[1] + right[1] * half)
    points = [(left_point[0], left_point[1], bottom), (right_point[0], right_point[1], bottom),
              (right_point[0], right_point[1], top), (left_point[0], left_point[1], top)]
    mesh.add_face(points, material, desired_normal=normal, uvs=FRONT_UVS)


def ceiling_light(mesh, x, y, z, size_x, size_y):
    """A flush light panel facing down."""
    mesh.add_face([(x - size_x / 2.0, y - size_y / 2.0, z), (x + size_x / 2.0, y - size_y / 2.0, z),
                   (x + size_x / 2.0, y + size_y / 2.0, z), (x - size_x / 2.0, y + size_y / 2.0, z)],
                  "CanopyLight", desired_normal=(0.0, 0.0, -1.0))


# ================================================================================================= canopy
def add_canopy_slab(mesh, half_x, half_y):
    """The roof slab: brand-coloured fascia all round, a white band along its lower edge, white soffit."""
    top = CANOPY_CLEAR + FASCIA_HEIGHT
    mesh.box(-half_x, half_x, -half_y, half_y, CANOPY_CLEAR, top, "BrandPaint", materials={"T": "Metal", "D": "Paint"})
    band_top = CANOPY_CLEAR + 0.15
    proud = 0.02
    mesh.box(-half_x - proud, half_x + proud, -half_y - proud, -half_y, CANOPY_CLEAR, band_top, "Paint")
    mesh.box(-half_x - proud, half_x + proud, half_y, half_y + proud, CANOPY_CLEAR, band_top, "Paint")
    mesh.box(-half_x - proud, -half_x, -half_y, half_y, CANOPY_CLEAR, band_top, "Paint")
    mesh.box(half_x, half_x + proud, -half_y, half_y, CANOPY_CLEAR, band_top, "Paint")


def add_logo_box(mesh, centre_x, face_y, facing, bottom):
    """A logo box standing proud of a fascia that faces along facing (-1 or +1 in Y), rising above it, with the
    lit logo panel on its face."""
    width, height, depth = LOGO_BOX
    outer_y = face_y + facing * depth
    mesh.box(centre_x - width / 2.0, centre_x + width / 2.0, min(face_y, outer_y), max(face_y, outer_y), bottom,
             bottom + height, "BrandPaint", materials={"T": "Metal"})
    panel_width, panel_height = LOGO_PANEL
    panel_bottom = bottom + (height - panel_height) / 2.0
    printed_face(mesh, (centre_x, outer_y + facing * 0.003), (0.0, facing, 0.0), panel_width, panel_bottom,
                 panel_bottom + panel_height, "BrandLogo")


def add_canopy_logos(mesh, half_x, half_y):
    """Logo boxes on the front and back fascia, toward the entry end, rising half a metre above the fascia."""
    bottom = CANOPY_CLEAR + 0.05
    centre_x = -half_x + LOGO_BOX[0] / 2.0 + 1.0
    add_logo_box(mesh, centre_x, -half_y - 0.02, -1.0, bottom)
    add_logo_box(mesh, centre_x, half_y + 0.02, 1.0, bottom)


def add_canopy_soffit(mesh, half_x, half_y):
    """The ribbed metal soffit: ribs across the canopy every 0.3 m, between them the light strips over the lanes."""
    rib_count = int(2 * half_y / 0.3)
    for rib in range(1, rib_count):
        y = -half_y + 2 * half_y * rib / rib_count
        mesh.box(-half_x, half_x, y - 0.04, y + 0.04, CANOPY_CLEAR - 0.06, CANOPY_CLEAR, "Paint", skip="T")


def add_canopy_column(mesh, y):
    """A clad column on an island's centre: brand colour below, dark grey above, a white stripe between them."""
    half = 0.3
    mesh.box(-half, half, y - half, y + half, 0.0, 2.6, "BrandPaint", skip="TD")
    mesh.box(-half - 0.01, half + 0.01, y - half - 0.01, y + half + 0.01, 2.6, 2.7, "Paint")
    mesh.box(-half, half, y - half, y + half, 2.7, CANOPY_CLEAR - 0.06, "PowderCoat", skip="BD")


def build_canopy(island_count):
    """The canopy over island_count islands with its columns and the light panels over the lanes."""
    mesh = Mesh()
    half_x, half_y = CANOPY_LENGTH / 2.0, island_count * ISLAND_SPACING / 2.0
    add_canopy_slab(mesh, half_x, half_y)
    add_canopy_logos(mesh, half_x, half_y)
    add_canopy_soffit(mesh, half_x, half_y)
    for island in range(island_count):
        island_y = -half_y + (island + 0.5) * ISLAND_SPACING
        add_canopy_column(mesh, island_y)
        for lane_offset in (-ISLAND_SPACING / 4.0, ISLAND_SPACING / 4.0):
            for x in (-4.0, 4.0):
                ceiling_light(mesh, x, island_y + lane_offset, CANOPY_CLEAR - 0.065, 4.5, 0.25)
    return mesh


# ================================================================================================= pump island
def add_hose(mesh, x, end, side):
    """A hose from the dispenser's header looping down past the cabinet and up into its nozzle in the holster."""
    outlet = (x + end * 0.58, side * 0.18, 1.95)
    sag = (x + end * 0.9, side * 0.34, 0.15)
    nozzle = (x + end * 0.64, side * 0.31, 1.22)
    mesh.pipe(bezier_points(outlet, sag, nozzle, 12), 0.014, "PowderCoat", segments=6)
    nozzle_x = x + end * 0.64
    mesh.box(nozzle_x - 0.03, nozzle_x + 0.03, side * 0.31 - 0.04, side * 0.31 + 0.04, 1.22, 1.46, "PowderCoat")
    mesh.box(nozzle_x - 0.012, nozzle_x + 0.012, side * 0.31 - 0.012, side * 0.31 + 0.012, 1.1, 1.22, "Metal")


def bezier_points(start, control, end, steps):
    """Points along a quadratic Bezier curve, for hoses that sag smoothly."""
    points = []
    for step in range(steps + 1):
        t = step / steps
        points.append(tuple((1 - t) ** 2 * start[k] + 2 * (1 - t) * t * control[k] + t * t * end[k] for k in range(3)))
    return points


def add_dispenser(mesh, x):
    """A multi-product dispenser: cabinet, the display section with a pump face on each side, end columns with the
    holsters, the brand-coloured header, and a hose and nozzle at each corner."""
    mesh.box(x - 0.55, x + 0.55, -0.28, 0.28, ISLAND_HEIGHT, 0.25, "Metal", skip="D")
    mesh.box(x - 0.5, x + 0.5, -0.25, 0.25, 0.25, 1.05, "BrandPaint")
    mesh.box(x - 0.5, x + 0.5, -0.22, 0.22, 1.05, 1.95, "Paint", skip="TD")
    for side in (-1.0, 1.0):
        printed_face(mesh, (x, side * 0.221), (0.0, side, 0.0), 0.9, 1.08, 1.93, "PumpFace")
    for end in (-1.0, 1.0):
        column_x = x + end * 0.55
        mesh.box(column_x - 0.05, column_x + 0.05, -0.25, 0.25, 0.25, 1.95, "Metal")
    mesh.box(x - 0.6, x + 0.6, -0.27, 0.27, 1.95, 2.3, "BrandPaint", materials={"T": "Metal"})
    for end in (-1.0, 1.0):
        for side in (-1.0, 1.0):
            add_hose(mesh, x, end, side)


def add_island_guards(mesh):
    """The yellow hoops that protect the dispensers at both ends of the island."""
    for end in (-1.0, 1.0):
        x = end * (ISLAND_LENGTH / 2.0 - 0.45)
        mesh.pipe([(x, -0.4, ISLAND_HEIGHT), (x, -0.4, 0.95), (x, 0.4, 0.95), (x, 0.4, ISLAND_HEIGHT)], 0.045,
                  "SafetyYellow", segments=8)


def add_island_bin(mesh):
    """A steel bin with a paper towel dispenser on top, and a bucket with window squeegees beside it."""
    mesh.box(0.55, 0.9, -0.17, 0.17, ISLAND_HEIGHT, 1.0, "Metal", skip="D")
    mesh.box(0.6, 0.85, -0.12, 0.12, 1.0, 1.35, "PowderCoat", skip="D")
    mesh.cylinder(-0.75, 0.0, ISLAND_HEIGHT, 0.55, 0.13, "PowderCoat", segments=12, radius_top=0.15)
    for offset in (-0.04, 0.04):
        mesh.box(-0.77 + offset, -0.73 + offset, -0.01, 0.01, 0.45, 0.95, "Metal")
        mesh.box(-0.85 + offset, -0.65 + offset, -0.02, 0.02, 0.95, 1.0, "PowderCoat")


def build_pump_island():
    """The raised island with two dispensers, end guards and the bin; the canopy column stands on its centre."""
    mesh = Mesh()
    outline = rounded_outline(ISLAND_LENGTH / 2.0, ISLAND_WIDTH / 2.0, 0.6, 0.0)
    mesh.prism(outline, "xy", 0.0, ISLAND_HEIGHT, "Concrete", smooth_sides=True)
    for x in (-DISPENSER_X, DISPENSER_X):
        add_dispenser(mesh, x)
    add_island_guards(mesh)
    add_island_bin(mesh)
    return mesh


# ================================================================================================= price pylon
PYLON_BOARD_BOTTOM = PYLON_HEIGHT - 0.2 - PRICE_BOARD[1]


def build_price_pylon():
    """The price totem: a brand-coloured panel with the price board on both faces, raised on two white legs that
    frame it up to the top, on small concrete footings."""
    mesh = Mesh()
    width, height = PRICE_BOARD
    half = width / 2.0 + 0.05
    panel_bottom = PYLON_BOARD_BOTTOM - 0.1
    mesh.box(-half, half, -0.18, 0.18, panel_bottom, PYLON_HEIGHT, "BrandPaint")
    for side_x in (-1.0, 1.0):
        leg_x = side_x * (half + 0.09)
        mesh.box(leg_x - 0.09, leg_x + 0.09, -0.2, 0.2, 0.0, PYLON_HEIGHT + 0.05, "Paint", skip="D")
        mesh.box(leg_x - 0.2, leg_x + 0.2, -0.3, 0.3, -0.05, 0.12, "Concrete", skip="D")
    mesh.box(-half - 0.2, half + 0.2, -0.22, 0.22, PYLON_HEIGHT + 0.05, PYLON_HEIGHT + 0.12, "Paint")
    for side in (-1.0, 1.0):
        printed_face(mesh, (0.0, side * 0.183), (0.0, side, 0.0), width, PYLON_BOARD_BOTTOM,
                     PYLON_BOARD_BOTTOM + height, "PriceBoard")
    return mesh


# ================================================================================================= shop
def add_shop_walls(mesh):
    """The plastered walls up to the parapet, with the storeroom's solid front right of the glass."""
    half = SHOP_WIDTH / 2.0
    mesh.box(-half, half, SHOP_DEPTH - WALL, SHOP_DEPTH, 0.0, SHOP_HEIGHT, "Plaster", skip="D")
    mesh.box(-half, -half + WALL, 0.0, SHOP_DEPTH - WALL, 0.0, SHOP_HEIGHT, "Plaster", skip="D")
    mesh.box(half - WALL, half, 0.0, SHOP_DEPTH - WALL, 0.0, SHOP_HEIGHT, "Plaster", skip="D")
    mesh.box(SHOP_GLASS_RIGHT, half - WALL, 0.0, WALL, 0.0, SHOP_FASCIA_BOTTOM, "Plaster", skip="D")
    mesh.box(-half, half, -0.05, WALL, SHOP_FASCIA_BOTTOM, SHOP_HEIGHT, "BrandPaint", materials={"B": "Paint"})
    mesh.box(-half, half, 0.0, SHOP_DEPTH, SHOP_HEIGHT - 0.4, SHOP_HEIGHT - 0.35, "Metal", skip="D")
    mesh.box(-half - 0.03, half + 0.03, -0.08, SHOP_DEPTH + 0.03, SHOP_HEIGHT, SHOP_HEIGHT + 0.06, "Metal")
    logo_width, logo_height = LOGO_PANEL
    logo_bottom = SHOP_FASCIA_BOTTOM + (SHOP_HEIGHT - SHOP_FASCIA_BOTTOM - logo_height) / 2.0
    printed_face(mesh, (SHOP_DOOR_X, -0.053), (0.0, -1.0, 0.0), logo_width, logo_bottom, logo_bottom + logo_height,
                 "BrandLogo")


def add_shop_glass_front(mesh):
    """The glass front between the left wall and the storeroom: mullions every metre and a half, and the sliding door
    with its head and fanlight."""
    left, right = -SHOP_WIDTH / 2.0 + WALL, SHOP_GLASS_RIGHT
    top = SHOP_FASCIA_BOTTOM
    mesh.box(left, right, 0.12, 0.14, 0.05, top, "Glass")
    mesh.box(left, right, 0.05, 0.25, 0.0, 0.05, "Metal")
    count = round((right - left) / 1.5)
    for index in range(count + 1):
        x = left + (right - left) * index / count
        mesh.box(x - 0.03, x + 0.03, 0.06, 0.24, 0.05, top, "Frame")
    door_half = 1.1
    for x in (SHOP_DOOR_X - door_half, SHOP_DOOR_X + door_half):
        mesh.box(x - 0.05, x + 0.05, 0.04, 0.24, 0.05, top, "Frame")
    mesh.box(SHOP_DOOR_X - door_half, SHOP_DOOR_X + door_half, 0.04, 0.24, 2.3, 2.45, "Frame")
    mesh.box(SHOP_DOOR_X - 0.03, SHOP_DOOR_X + 0.03, 0.08, 0.12, 0.05, 2.3, "Frame")


def add_shop_interior(mesh):
    """What shows through the glass: floor, a lit ceiling, shelves of goods and the counter."""
    half = SHOP_WIDTH / 2.0 - WALL
    ceiling = 3.2
    mesh.add_face([(-half, 0.15, 0.01), (half, 0.15, 0.01), (half, SHOP_DEPTH - WALL, 0.01),
                   (-half, SHOP_DEPTH - WALL, 0.01)], "Concrete", desired_normal=(0.0, 0.0, 1.0))
    mesh.add_face([(-half, 0.15, ceiling), (half, 0.15, ceiling), (half, SHOP_DEPTH - WALL, ceiling),
                   (-half, SHOP_DEPTH - WALL, ceiling)], "Paint", desired_normal=(0.0, 0.0, -1.0))
    for x in (-6.0, -3.5, -1.0, 1.5):
        for y in (2.0, 4.5, 7.0):
            ceiling_light(mesh, x, y, ceiling - 0.005, 0.6, 1.2)
    for x in (-6.4, -5.0, -3.6):
        mesh.box(x - 0.45, x + 0.45, 3.5, 8.5, 0.0, 1.6, "Paint", skip="D")
        for shelf_z in (0.45, 0.9, 1.35):
            mesh.box(x - 0.5, x + 0.5, 3.6, 8.4, shelf_z, shelf_z + 0.25, "Litter")
    mesh.box(0.0, 2.6, 2.2, 2.9, 0.0, 1.05, "BrandPaint", materials={"T": "Paint"})
    mesh.box(0.2, 2.4, SHOP_DEPTH - WALL - 0.5, SHOP_DEPTH - WALL, 0.0, 2.2, "Litter")


def build_shop():
    """The shop: plastered box with a parapet, brand fascia with the logo over the door, glass front and an interior."""
    mesh = Mesh()
    add_shop_walls(mesh)
    add_shop_glass_front(mesh)
    add_shop_interior(mesh)
    return mesh


# ================================================================================================= car wash
def add_wash_end_wall(mesh, y0, y1):
    """An end wall with the portal opening."""
    half = WASH_WIDTH / 2.0
    opening_half, opening_height = WASH_OPENING[0] / 2.0, WASH_OPENING[1]
    mesh.box(-half, -opening_half, y0, y1, 0.0, WASH_HEIGHT, "Paint", skip="D")
    mesh.box(opening_half, half, y0, y1, 0.0, WASH_HEIGHT, "Paint", skip="D")
    mesh.box(-opening_half, opening_half, y0, y1, opening_height, WASH_HEIGHT, "Paint")


def add_wash_side_walls(mesh):
    """Side walls with a translucent band of polycarbonate under the eaves."""
    half = WASH_WIDTH / 2.0
    for x0, x1 in ((-half, -half + 0.25), (half - 0.25, half)):
        mesh.box(x0, x1, 0.25, WASH_LENGTH - 0.25, 0.0, 2.4, "Paint", skip="D")
        mesh.box(x0, x1, 0.25, WASH_LENGTH - 0.25, 3.6, WASH_HEIGHT, "Paint")
        x = (x0 + x1) / 2.0
        mesh.quad_double([(x, 0.25, 2.4), (x, WASH_LENGTH - 0.25, 2.4), (x, WASH_LENGTH - 0.25, 3.6),
                          (x, 0.25, 3.6)], "Foil")


def add_wash_gantry(mesh, y):
    """The wash gantry: a steel portal on rails with two side brushes and the roof brush."""
    for side in (-1.0, 1.0):
        mesh.box(side * 2.6 - 0.12, side * 2.6 + 0.12, y - 0.2, y + 0.2, 0.0, 3.6, "Metal", skip="D")
        mesh.cylinder(side * 1.45, y, 0.25, 2.75, 0.5, "WashBrush", segments=20)
        mesh.box(side * 1.45 - 0.08, side * 1.45 + 0.08, y - 0.08, y + 0.08, 2.75, 3.4, "Metal")
    mesh.box(-2.72, 2.72, y - 0.25, y + 0.25, 3.4, 3.75, "Metal")
    mesh.tube((-1.9, y + 0.7, 2.3), (1.9, y + 0.7, 2.3), 0.5, "WashBrush", segments=20)
    mesh.box(-2.1, 2.1, y + 0.55, y + 0.85, 2.85, 3.4, "Metal")


def build_car_wash():
    """The wash hall: portal end walls, side walls, a roof with a brand-coloured edge, the guide rail, the gantry,
    and the wash sign over the entrance."""
    mesh = Mesh()
    add_wash_end_wall(mesh, 0.0, 0.25)
    add_wash_end_wall(mesh, WASH_LENGTH - 0.25, WASH_LENGTH)
    add_wash_side_walls(mesh)
    half = WASH_WIDTH / 2.0
    mesh.box(-half - 0.1, half + 0.1, -0.1, WASH_LENGTH + 0.1, WASH_HEIGHT, WASH_HEIGHT + 0.3, "BrandPaint",
             materials={"T": "Metal", "D": "Paint"})
    mesh.add_face([(-half, 0.0, 0.01), (half, 0.0, 0.01), (half, WASH_LENGTH, 0.01), (-half, WASH_LENGTH, 0.01)],
                  "Concrete", desired_normal=(0.0, 0.0, 1.0))
    mesh.box(-1.15, -0.95, 0.6, WASH_LENGTH - 0.6, 0.0, 0.12, "SafetyYellow", skip="D")
    add_wash_gantry(mesh, WASH_LENGTH / 2.0)
    sign_bottom = WASH_OPENING[1] + 0.05
    printed_face(mesh, (0.0, -0.005), (0.0, -1.0, 0.0), 3.8, sign_bottom, sign_bottom + 0.95, "WashSign")
    return mesh


# ================================================================================================= air and vacuum
def build_air_vacuum():
    """The air and vacuum bay: an air column with its hose and a vacuum unit with a swivel arm, on a small island."""
    mesh = Mesh()
    mesh.prism(rounded_outline(1.6, 0.55, 0.45, 0.0), "xy", 0.0, ISLAND_HEIGHT, "Concrete", smooth_sides=True)
    mesh.box(-1.0, -0.6, -0.15, 0.15, ISLAND_HEIGHT, 1.35, "Paint", skip="D")
    mesh.box(-1.02, -0.58, -0.17, 0.17, 1.35, 1.6, "BrandPaint")
    mesh.pipe(bezier_points((-0.6, -0.1, 1.2), (-0.3, -0.35, 0.0), (-0.6, -0.16, 0.95), 12), 0.015, "PowderCoat",
              segments=6)
    mesh.cylinder(0.8, 0.0, ISLAND_HEIGHT, 1.2, 0.3, "BrandPaint", segments=20)
    mesh.cylinder(0.8, 0.0, 1.2, 1.28, 0.32, "Metal", segments=20)
    mesh.pipe([(0.8, 0.0, 1.28), (0.8, 0.0, 2.5), (0.8, -1.2, 2.5)], 0.045, "Metal", segments=8)
    mesh.pipe(bezier_points((0.8, -1.2, 2.48), (0.85, -1.0, 0.0), (0.8, -0.32, 1.0), 12), 0.03, "PowderCoat",
              segments=8)
    return mesh


# ================================================================================================= site layout
def station_layout(island_count, car_wash):
    """Where the pieces of a station go on a plot whose road edge is y = 0 and whose middle is x = 0: the pylon at the
    entry corner, the canopy behind a 5 m apron with a lane on either side, the shop facing the pumps 5 m behind the
    canopy, and the car wash and air bay to the right."""
    canopy_depth = island_count * ISLAND_SPACING
    right_side = WASH_WIDTH + 7.0 if car_wash else 6.0
    plot_width = 4.5 + CANOPY_LENGTH + right_side
    canopy_x = -plot_width / 2.0 + 4.5 + CANOPY_LENGTH / 2.0
    canopy_front = 5.0
    shop_front = canopy_front + canopy_depth + 5.0
    return {
        "plot_width": plot_width,
        "plot_depth": shop_front + SHOP_DEPTH + 2.0,
        "canopy_x": canopy_x,
        "canopy_front": canopy_front,
        "canopy_depth": canopy_depth,
        "shop_front": shop_front,
        "pylon": (-plot_width / 2.0 + 1.5, 1.5),
        "wash_x": canopy_x + CANOPY_LENGTH / 2.0 + 3.5 + WASH_WIDTH / 2.0,
        "air": (canopy_x + CANOPY_LENGTH / 2.0 + 2.0, shop_front - 2.0),
    }


def station_parts(island_count, car_wash):
    """The parts of one station, (piece, location, rotation, mode), following station_layout."""
    layout = station_layout(island_count, car_wash)
    canopy_x, canopy_front = layout["canopy_x"], layout["canopy_front"]
    no_rotation = (0.0, 0.0, 0.0)
    parts = [("Canopy_%d" % island_count, (canopy_x, canopy_front + layout["canopy_depth"] / 2.0, 0.0), no_rotation,
              "XYZ")]
    for island in range(island_count):
        island_y = canopy_front + (island + 0.5) * ISLAND_SPACING
        parts.append(("PumpIsland", (canopy_x, island_y, 0.0), no_rotation, "XYZ"))
    parts.append(("Shop", (canopy_x, layout["shop_front"], 0.0), no_rotation, "XYZ"))
    parts.append(("PricePylon", (layout["pylon"][0], layout["pylon"][1], 0.0), (0.0, 0.0, 90.0), "XYZ"))
    parts.append(("AirVacuum", (layout["air"][0], layout["air"][1], 0.0), (0.0, 0.0, 90.0), "XYZ"))
    if car_wash:
        parts.append(("CarWash", (layout["wash_x"], canopy_front, 0.0), no_rotation, "XYZ"))
    return parts


def forecourt(layout):
    """Sheet-only ground: the paved plot and a strip of the road in front of it."""
    mesh = Mesh()
    half = layout["plot_width"] / 2.0
    mesh.add_face([(-half, 0.0, 0.005), (half, 0.0, 0.005), (half, layout["plot_depth"], 0.005),
                   (-half, layout["plot_depth"], 0.005)], "Concrete", desired_normal=(0.0, 0.0, 1.0))
    mesh.add_face([(-half - 6.0, -7.0, 0.002), (half + 6.0, -7.0, 0.002), (half + 6.0, 0.0, 0.002),
                   (-half - 6.0, 0.0, 0.002)], "PowderCoat", desired_normal=(0.0, 0.0, 1.0))
    return mesh


# ================================================================================================= kit entry
def build():
    """The station pieces, and a spec with their footprints and the layout constants."""
    pieces = OrderedDict()
    for island_count in (2, 3, 4):
        pieces["Canopy_%d" % island_count] = build_canopy(island_count)
    pieces["PumpIsland"] = build_pump_island()
    pieces["PricePylon"] = build_price_pylon()
    pieces["Shop"] = build_shop()
    pieces["CarWash"] = build_car_wash()
    pieces["AirVacuum"] = build_air_vacuum()
    spec = {"canopy_clear_height": CANOPY_CLEAR, "island_spacing": ISLAND_SPACING, "brands": list(BRANDS)}
    for name, mesh in pieces.items():
        lower, upper = mesh.bounds()
        spec[name] = {"footprint_x": [round(lower[0], 3), round(upper[0], 3)],
                      "footprint_y": [round(lower[1], 3), round(upper[1], 3)]}
    return pieces, spec


def poster_dir():
    """Where posters/draw_brands.py writes the brand images."""
    return os.path.join(os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear"), "building_kit", "posters")


def preview_textures():
    """The brand images per variant, for the sheet."""
    textures = {}
    images = {"BrandLogo": "fuel_logo_%s.png", "PriceBoard": "fuel_prices_%s.png", "PumpFace": "fuel_pump_%s.png",
              "WashSign": "fuel_wash_%s.png"}
    for brand in BRANDS:
        for slot, pattern in images.items():
            textures["%s@%s" % (slot, brand)] = os.path.join(poster_dir(), pattern % brand)
    return textures


def preview_colours():
    """The brand colour per variant, for the sheet."""
    return {"BrandPaint@%s" % brand: colour for brand, colour in BRAND_COLOURS.items()}


STATIONS = [
    {"name": "Small", "title": "hopp!, 2 islands", "variant": "hopp", "islands": 2, "car_wash": False},
    {"name": "Medium", "title": "Vierländer Öl, 3 islands\nand car wash", "variant": "vierlaender", "islands": 3,
     "car_wash": True},
    {"name": "Large", "title": "NORDTANK, 4 islands\nand car wash", "variant": "nordtank", "islands": 4,
     "car_wash": True},
]


def station_closeups(station, layout):
    """An overview from the road, front left, and close-ups of the pieces, spread over the three stations."""
    closeups = [{"caption": station["title"].replace("\n", " "), "target": (0.0, layout["plot_depth"] * 0.4, 2.0),
                 "direction": (-0.55, -1.0, 0.16), "distance": layout["plot_width"] * 1.15, "lens": 35.0}]
    canopy_x, first_island = layout["canopy_x"], layout["canopy_front"] + ISLAND_SPACING / 2.0
    if station["name"] == "Small":
        closeups.append({"caption": "Price pylon", "target": (layout["pylon"][0], layout["pylon"][1], 3.7),
                         "direction": (-1.0, -0.35, 0.05), "distance": 11.0, "lens": 35.0})
        closeups.append({"caption": "Pump island", "target": (canopy_x + DISPENSER_X, first_island, 1.3),
                         "direction": (0.4, -1.0, 0.22), "distance": 4.5, "lens": 35.0})
    if station["name"] == "Medium":
        closeups.append({"caption": "Car wash", "target": (layout["wash_x"], layout["canopy_front"] + 5.0, 1.9),
                         "direction": (-0.2, -1.0, 0.12), "distance": 10.0, "lens": 35.0})
        closeups.append({"caption": "Air and vacuum", "target": (layout["air"][0], layout["air"][1], 1.2),
                         "direction": (-1.0, -0.6, 0.3), "distance": 5.0, "lens": 35.0})
    if station["name"] == "Large":
        closeups.append({"caption": "Shop", "target": (canopy_x - 1.0, layout["shop_front"], 2.2),
                         "direction": (-0.35, -1.0, 0.1), "distance": 11.0, "lens": 35.0})
        closeups.append({"caption": "Under the canopy", "target": (canopy_x, first_island + ISLAND_SPACING, 2.4),
                         "direction": (-1.0, -0.5, 0.18), "distance": 11.0, "lens": 28.0})
    return closeups


def assemblies():
    """Three stations, one per brand and size, laid out by station_parts."""
    result = []
    for station in STATIONS:
        layout = station_layout(station["islands"], station["car_wash"])
        result.append({
            "name": station["name"],
            "title": station["title"],
            "variant": station["variant"],
            "parts": station_parts(station["islands"], station["car_wash"]),
            "half_width": layout["plot_width"] / 2.0,
            "top": PYLON_HEIGHT + 0.1,
            "depth": layout["plot_depth"],
            "extras": forecourt(layout),
            "closeups": station_closeups(station, layout),
        })
    return result
