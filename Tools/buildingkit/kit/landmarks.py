"""Landmarks (#111): buildings that get a model of their own, built from measured data rather than from kit pieces.

The Elbphilharmonie comes from two Hamburg open data sets in the data root: the footprint from the LoD2 model
(downloads/lod2_hamburg, where the building is a plain prism) and the wave roof from the 2020 bDOM, the 1 m surface
model (downloads/bdom_hamburg). Heights of the brick base and the plaza come from the German Wikipedia article.

Frame: metres, Z up, x east and y north, the origin at the middle of the footprint on the quay (8.3 m above sea
level). The spec records the UTM 32N position and the sea level height of the origin, so the runtime can place it.
"""

import io
import math
import os
import re
import subprocess
from collections import OrderedDict

import numpy

from .geom import Mesh

DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
LOD2_ZIP = os.path.join(DATA_ROOT, "downloads", "lod2_hamburg", "LoD2-DE_HH.zip")
BDOM_ZIP = os.path.join(DATA_ROOT, "downloads", "bdom_hamburg", "dom1_xyz_hh_2020.zip")
TILE = (565000, 5932000)  # south-west corner of the 1 km tile that holds the building
QUAY_LEVEL = 8.3  # sea level height of the quay around the building, measured in the bDOM
PLAZA_LEVEL = 37.0  # the plaza on top of the Kaispeicher, above sea level
PLAZA_CLEAR = 3.5  # from the plaza floor to the underside of the glass superstructure, estimated from photos
PLAZA_SETBACK = 3.0  # how far the plaza's glazing stands back from the facade
GLASS_PANEL = (5.0, 3.5)  # width and height of one facade element
ROOF_SAMPLE_INSET = 3.0  # roof heights are read this far inside the edge, where the bDOM is not mixed with the quay
# The arch over the plaza on the south facade: position along the facade from the west, half width and rise above
# the plaza floor, from the photo.
ARCH = (0.67, 7.0, 7.5)
# Dark recessed slots between the brick blocks of the Kaispeicher, as fractions along the long facades from the west.
BRICK_SLOTS = (0.32, 0.53, 0.71, 0.89)


# ================================================================================================= measured data
def read_zip_member(archive, member):
    """One file from a data root archive. The Hamburg archives use a compression Python's zipfile can't read, so
    this goes through the unzip tool."""
    return subprocess.run(["unzip", "-p", archive, member], check=True, capture_output=True).stdout


def read_footprint():
    """The building's ground outline from the LoD2 model as UTM points, reduced to its four corners."""
    text = read_zip_member(LOD2_ZIP, "LoD2_32_565_5932_1_HH.gml").decode("utf-8")
    name_at = text.index("<gml:name>Elbphilharmonie</gml:name>")
    start = text.rindex("<bldg:Building ", 0, name_at)
    end = text.index("</bldg:Building>", name_at)
    ground = re.search(r"<bldg:GroundSurface.*?<gml:posList[^>]*>([^<]*)<", text[start:end], re.S).group(1)
    values = [float(value) for value in ground.split()]
    points = [(values[index], values[index + 1]) for index in range(0, len(values), 3)]
    return corners_of(points)


def corners_of(points):
    """Drops repeated and nearly collinear points from a closed outline, leaving its real corners."""
    unique = []
    for point in points:
        if not unique or math.dist(point, unique[-1]) > 1.5:
            unique.append(point)
    if math.dist(unique[0], unique[-1]) < 1.5:
        unique.pop()
    corners = []
    for index, point in enumerate(unique):
        before, after = unique[index - 1], unique[(index + 1) % len(unique)]
        heading_in = math.atan2(point[1] - before[1], point[0] - before[0])
        heading_out = math.atan2(after[1] - point[1], after[0] - point[0])
        turn = abs((heading_out - heading_in + math.pi) % (2 * math.pi) - math.pi)
        if turn > math.radians(5):
            corners.append(point)
    return corners


def read_surface():
    """The bDOM tile as a 1000 by 1000 grid of sea level heights, row = metres north of the tile corner, smoothed over
    3 by 3 cells against single-cell noise."""
    data = numpy.loadtxt(io.BytesIO(read_zip_member(BDOM_ZIP, "dom1_32_565_5932_1_hh.xyz")))
    grid = numpy.full((1000, 1000), numpy.nan)
    grid[(data[:, 1] - TILE[1]).astype(int), (data[:, 0] - TILE[0]).astype(int)] = data[:, 2]
    padded = numpy.pad(grid, 1, mode="edge")
    stack = [padded[1 + dy:1001 + dy, 1 + dx:1001 + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
    return numpy.nanmean(numpy.stack(stack), axis=0)


def surface_height(surface, x, y):
    """Bilinear height of the surface grid at a point in tile metres."""
    column, row = int(math.floor(x)), int(math.floor(y))
    fraction_x, fraction_y = x - column, y - row
    lower = surface[row, column] * (1 - fraction_x) + surface[row, column + 1] * fraction_x
    upper = surface[row + 1, column] * (1 - fraction_x) + surface[row + 1, column + 1] * fraction_x
    return lower * (1 - fraction_y) + upper * fraction_y


# ================================================================================================= the wedge
class Wedge:
    """The four-cornered footprint as a bilinear patch: u runs from the narrow west edge to the wide east edge, v from
    the south facade to the north facade. Points are in the model frame."""

    def __init__(self, corners_utm):
        centre = (sum(x for x, _ in corners_utm) / 4.0, sum(y for _, y in corners_utm) / 4.0)
        self.origin_utm = centre
        local = [(x - centre[0], y - centre[1]) for x, y in corners_utm]
        edges = [(index, math.dist(local[index], local[(index + 1) % 4])) for index in range(4)]
        west_index = min(edges, key=lambda edge: edge[1])[0]
        west = [local[west_index], local[(west_index + 1) % 4]]
        east = [local[(west_index + 3) % 4], local[(west_index + 2) % 4]]
        # Make index 0 of each edge the southern end.
        if west[0][1] > west[1][1]:
            west.reverse()
            east.reverse()
        self.west_south, self.west_north = west
        self.east_south, self.east_north = east

    def point(self, u, v):
        """The footprint point at (u, v)."""
        south = [self.west_south[k] + (self.east_south[k] - self.west_south[k]) * u for k in range(2)]
        north = [self.west_north[k] + (self.east_north[k] - self.west_north[k]) * u for k in range(2)]
        return (south[0] + (north[0] - south[0]) * v, south[1] + (north[1] - south[1]) * v)

    def length(self):
        """Mean length from the west edge to the east edge."""
        return (math.dist(self.west_south, self.east_south) + math.dist(self.west_north, self.east_north)) / 2.0

    def width(self, u):
        """Width from the south to the north facade at u."""
        return math.dist(self.point(u, 0.0), self.point(u, 1.0))

    def inset_point(self, u, v, inset):
        """The point at (u, v) moved inward by inset metres, for reading the roof clear of the facade edge."""
        u_margin = inset / self.length()
        v_margin = inset / self.width(u)
        return self.point(min(max(u, u_margin), 1 - u_margin), min(max(v, v_margin), 1 - v_margin))

    def ring(self, u_steps, v_steps):
        """(u, v) around the outline counter-clockwise from the south-west corner: south, east, north, west facade."""
        ring = [(step / u_steps, 0.0) for step in range(u_steps)]
        ring += [(1.0, step / v_steps) for step in range(v_steps)]
        ring += [(1.0 - step / u_steps, 1.0) for step in range(u_steps)]
        ring += [(0.0, 1.0 - step / v_steps) for step in range(v_steps)]
        return ring


class Roof:
    """The measured roof on the wedge's grid of u_steps by v_steps cells: heights above the quay, read inside the
    facade and smoothed, so the crests along the facades run as clean curves instead of following the bDOM's noise."""

    def __init__(self, wedge, surface, u_steps, v_steps, smoothing_passes=3):
        self.u_steps = u_steps
        self.v_steps = v_steps
        heights = numpy.empty((u_steps + 1, v_steps + 1))
        for i in range(u_steps + 1):
            for j in range(v_steps + 1):
                x, y = wedge.inset_point(i / u_steps, j / v_steps, ROOF_SAMPLE_INSET)
                tile_x = x + wedge.origin_utm[0] - TILE[0]
                tile_y = y + wedge.origin_utm[1] - TILE[1]
                heights[i, j] = surface_height(surface, tile_x, tile_y) - QUAY_LEVEL
        for _ in range(smoothing_passes):
            padded = numpy.pad(heights, 1, mode="edge")
            heights = sum(padded[1 + di:u_steps + 2 + di, 1 + dj:v_steps + 2 + dj]
                          for di in (-1, 0, 1) for dj in (-1, 0, 1)) / 9.0
        self.heights = heights

    def height(self, u, v):
        """Roof height above the quay at a grid node (u, v)."""
        return float(self.heights[round(u * self.u_steps), round(v * self.v_steps)])


# ================================================================================================= geometry
PLAZA_FLOOR = PLAZA_LEVEL - QUAY_LEVEL
GLASS_BOTTOM = PLAZA_FLOOR + PLAZA_CLEAR


def outward_normal(start, end):
    """Horizontal outward normal of a counter-clockwise outline edge."""
    dx, dy = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dy)
    return (dy / length, -dx / length, 0.0)


def facade_point(point, normal, proud):
    """A footprint point pushed out from the facade."""
    return (point[0] + normal[0] * proud, point[1] + normal[1] * proud)


class Elbphilharmonie:
    """Builds the model from the measured wedge and roof."""

    def __init__(self, wedge, roof):
        self.wedge = wedge
        self.roof = roof
        self.u_steps = roof.u_steps
        self.v_steps = roof.v_steps
        self.mesh = Mesh()

    def build(self):
        """All parts in order, bottom to top."""
        self.add_brick_base()
        self.add_plaza()
        self.add_glass_walls()
        self.add_arch()
        self.add_roof()
        return self.mesh

    # ------------------------------------------------------------------ brick base
    def add_brick_base(self):
        """The Kaispeicher: brick walls up to the plaza floor, its recessed slots, small windows and loading doors."""
        corners = [self.wedge.west_south, self.wedge.east_south, self.wedge.east_north, self.wedge.west_north]
        self.mesh.prism(corners, "xy", 0.0, PLAZA_FLOOR, "Brick", caps="H")
        for index in range(4):
            start, end = corners[index], corners[(index + 1) % 4]
            self.add_brick_facade(start, end, long_facade=index in (0, 2), reverse=index == 2)

    def add_brick_facade(self, start, end, long_facade, reverse):
        """Windows, slots and doors on one brick facade from start to end; reverse counts the slot positions from
        the end, so they are measured from the west on the north facade too."""
        normal = outward_normal(start, end)
        length = math.dist(start, end)
        slots = [1.0 - fraction if reverse else fraction for fraction in BRICK_SLOTS] if long_facade else []
        for fraction in slots:
            self.facade_rectangle(start, end, normal, fraction * length - 1.5, fraction * length + 1.5, 0.6,
                                  PLAZA_FLOOR - 0.4, "PowderCoat", 0.03)
        self.add_brick_windows(start, end, normal, length, slots)
        door_count = int(length / 16.0)
        for door in range(door_count):
            along = (door + 0.5) * length / door_count
            self.facade_rectangle(start, end, normal, along - 1.6, along + 1.6, 0.0, 3.6, "PowderCoat", 0.03)

    def add_brick_windows(self, start, end, normal, length, slots):
        """The warehouse's small square windows in rows a storey apart, clear of the slots and the doors' row."""
        columns = int(length / 4.2)
        for row in range(1, 9):
            z = 2.0 + row * 2.9
            if z + 0.9 > PLAZA_FLOOR - 0.6:
                break
            for column in range(columns):
                along = (column + 0.5) * length / columns
                if any(abs(along - fraction * length) < 2.6 for fraction in slots):
                    continue
                self.facade_rectangle(start, end, normal, along - 0.45, along + 0.45, z, z + 0.9, "PowderCoat",
                                      0.02)

    def facade_rectangle(self, start, end, normal, along_start, along_end, bottom, top, material, proud):
        """A flat rectangle on a facade, from along_start to along_end metres from start, slightly proud."""
        length = math.dist(start, end)
        direction = ((end[0] - start[0]) / length, (end[1] - start[1]) / length)
        left = facade_point((start[0] + direction[0] * along_start, start[1] + direction[1] * along_start), normal,
                            proud)
        right = facade_point((start[0] + direction[0] * along_end, start[1] + direction[1] * along_end), normal, proud)
        self.mesh.add_face([(left[0], left[1], bottom), (right[0], right[1], bottom), (right[0], right[1], top),
                            (left[0], left[1], top)], material, desired_normal=normal)

    # ------------------------------------------------------------------ plaza
    def add_plaza(self):
        """The plaza level: glazing set back from the facade, the superstructure's white soffit above the walkway,
        and a glass balustrade at the edge."""
        inset = []
        for u, v in ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)):
            u_margin = PLAZA_SETBACK / self.wedge.length()
            v_margin = PLAZA_SETBACK / self.wedge.width(u)
            inset.append(self.wedge.point(min(max(u, u_margin), 1 - u_margin), min(max(v, v_margin), 1 - v_margin)))
        self.mesh.prism(inset, "xy", PLAZA_FLOOR, GLASS_BOTTOM, "Glass", caps="")
        outer = [self.wedge.west_south, self.wedge.east_south, self.wedge.east_north, self.wedge.west_north]
        for index in range(4):
            a, b = outer[index], outer[(index + 1) % 4]
            c, d = inset[(index + 1) % 4], inset[index]
            self.mesh.add_face([(a[0], a[1], GLASS_BOTTOM), (b[0], b[1], GLASS_BOTTOM), (c[0], c[1], GLASS_BOTTOM),
                                (d[0], d[1], GLASS_BOTTOM)], "Paint", desired_normal=(0.0, 0.0, -1.0))
            self.mesh.quad_double([(a[0], a[1], PLAZA_FLOOR), (b[0], b[1], PLAZA_FLOOR),
                                   (b[0], b[1], PLAZA_FLOOR + 1.1), (a[0], a[1], PLAZA_FLOOR + 1.1)], "Glass")

    # ------------------------------------------------------------------ glass superstructure
    def wall_bottom(self, u, v):
        """Lower edge of the glass wall at an outline point: the glass bottom, raised over the plaza arch."""
        centre, half_width, rise = ARCH
        if v > 0.0:
            return GLASS_BOTTOM
        offset = (u - centre) * self.wedge.length() / half_width
        if abs(offset) >= 1.0:
            return GLASS_BOTTOM
        return max(GLASS_BOTTOM, PLAZA_FLOOR + rise * math.sqrt(1.0 - offset * offset))

    def add_glass_walls(self):
        """The superstructure's facades from the glass bottom up to the measured roof edge, one metre at a time, with
        the dark grid of the facade elements on them."""
        ring = self.wedge.ring(self.u_steps, self.v_steps)
        for index, (u, v) in enumerate(ring):
            next_u, next_v = ring[(index + 1) % len(ring)]
            start, end = self.wedge.point(u, v), self.wedge.point(next_u, next_v)
            normal = outward_normal(start, end)
            bottoms = (self.wall_bottom(u, v), self.wall_bottom(next_u, next_v))
            tops = (self.roof.height(u, v), self.roof.height(next_u, next_v))
            self.mesh.add_face([(start[0], start[1], bottoms[0]), (end[0], end[1], bottoms[1]),
                                (end[0], end[1], tops[1]), (start[0], start[1], tops[0])], "FacadeGlass",
                               desired_normal=normal)
        self.add_panel_grid(ring)

    def add_panel_grid(self, ring):
        """Mullions every element width along each facade and transoms every element height, slightly proud."""
        width, height = GLASS_PANEL
        travelled = 0.0
        for index, (u, v) in enumerate(ring):
            next_u, next_v = ring[(index + 1) % len(ring)]
            start, end = self.wedge.point(u, v), self.wedge.point(next_u, next_v)
            normal = outward_normal(start, end)
            step = math.dist(start, end)
            bottom = self.wall_bottom(u, v)
            top = self.roof.height(u, v)
            if int((travelled + step) / width) > int(travelled / width):
                self.mullion(start, normal, bottom, top)
            travelled += step
            z = GLASS_BOTTOM + height
            while z < min(top, self.roof.height(next_u, next_v)) - 0.2:
                if z > max(bottom, self.wall_bottom(next_u, next_v)):
                    a, b = facade_point(start, normal, 0.06), facade_point(end, normal, 0.06)
                    self.mesh.add_face([(a[0], a[1], z - 0.06), (b[0], b[1], z - 0.06), (b[0], b[1], z + 0.06),
                                        (a[0], a[1], z + 0.06)], "Metal", desired_normal=normal)
                z += height

    def mullion(self, point, normal, bottom, top):
        """One vertical frame line on the facade."""
        side = (-normal[1] * 0.06, normal[0] * 0.06)
        a = facade_point((point[0] - side[0], point[1] - side[1]), normal, 0.06)
        b = facade_point((point[0] + side[0], point[1] + side[1]), normal, 0.06)
        self.mesh.add_face([(a[0], a[1], bottom), (b[0], b[1], bottom), (b[0], b[1], top - 0.1),
                            (a[0], a[1], top - 0.1)], "Metal", desired_normal=normal)

    # ------------------------------------------------------------------ arch and roof
    def add_arch(self):
        """The vault behind the arch over the plaza on the south facade: a white curved soffit running six metres
        into the building, closed by dark glass."""
        depth = 6.0
        steps = 24
        centre, half_width, rise = ARCH
        previous = None
        for step in range(steps + 1):
            offset = -1.0 + 2.0 * step / steps
            u = centre + offset * half_width / self.wedge.length()
            z = PLAZA_FLOOR + rise * math.sqrt(max(0.0, 1.0 - offset * offset))
            front = self.wedge.point(u, 0.0)
            back = self.wedge.point(u, depth / self.wedge.width(u))
            if previous is not None:
                front_before, back_before, z_before = previous
                self.mesh.add_face([(front_before[0], front_before[1], z_before), (front[0], front[1], z),
                                    (back[0], back[1], z), (back_before[0], back_before[1], z_before)], "Paint",
                                   smooth=True, desired_normal=(0.0, 0.0, -1.0))
                self.mesh.add_face([(back_before[0], back_before[1], PLAZA_FLOOR), (back[0], back[1], PLAZA_FLOOR),
                                    (back[0], back[1], z), (back_before[0], back_before[1], z_before)], "Glass",
                                   desired_normal=(0.0, -1.0, 0.0))
            previous = (front, back, z)

    def add_roof(self):
        """The measured roof as a smooth surface over the wedge."""
        heights = [[self.roof.height(i / self.u_steps, j / self.v_steps) for j in range(self.v_steps + 1)]
                   for i in range(self.u_steps + 1)]
        for i in range(self.u_steps):
            for j in range(self.v_steps):
                corners = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
                points = []
                for ci, cj in corners:
                    x, y = self.wedge.point(ci / self.u_steps, cj / self.v_steps)
                    points.append((x, y, heights[ci][cj]))
                self.mesh.add_face(points, "RoofSequins", smooth=True, desired_normal=(0.0, 0.0, 1.0))


# ================================================================================================= kit entry
_CACHE = {}


def elbphilharmonie():
    """The model and its wedge, built once per run."""
    if "model" not in _CACHE:
        wedge = Wedge(read_footprint())
        roof = Roof(wedge, read_surface(), u_steps=116, v_steps=40)
        _CACHE["model"] = (Elbphilharmonie(wedge, roof).build(), wedge, roof)
    return _CACHE["model"]


def build():
    """The landmark pieces and a spec with where they stand."""
    mesh, wedge, roof = elbphilharmonie()
    pieces = OrderedDict([("Elbphilharmonie", mesh)])
    lower, upper = mesh.bounds()
    spec = {"Elbphilharmonie": {
        "origin_utm32": [round(wedge.origin_utm[0], 2), round(wedge.origin_utm[1], 2)],
        "origin_sea_level": QUAY_LEVEL,
        "axes": "x east, y north",
        "height_above_quay": round(upper[2], 2),
        "plaza_above_quay": round(PLAZA_FLOOR, 2),
    }}
    return pieces, spec


def quay_and_water(wedge):
    """Sheet-only surroundings: the quay around the building and the Elbe at mean water level."""
    mesh = Mesh()
    corners = [wedge.west_south, wedge.east_south, wedge.east_north, wedge.west_north]
    quay = []
    for x, y in corners:
        distance = math.hypot(x, y)
        quay.append((x + x / distance * 12.0, y + y / distance * 12.0))
    mesh.prism(quay, "xy", -7.5, 0.0, "Concrete", caps="H")
    water = 1.0 - QUAY_LEVEL
    mesh.add_face([(-400.0, -400.0, water), (400.0, -400.0, water), (400.0, 400.0, water), (-400.0, 400.0, water)],
                  "Water", desired_normal=(0.0, 0.0, 1.0))
    return mesh


def assemblies():
    """The Elbphilharmonie from the Elbe like the reference photo, from the east, the roof from above and the plaza
    arch up close."""
    mesh, wedge, _roof = elbphilharmonie()
    lower, upper = mesh.bounds()
    arch_x, arch_y = wedge.point(ARCH[0], 0.0)
    middle = (0.0, 0.0, upper[2] * 0.45)
    return [{
        "name": "Elbphilharmonie",
        "title": "Elbphilharmonie",
        "parts": [("Elbphilharmonie", (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), "XYZ")],
        "half_width": max(abs(lower[0]), upper[0]),
        "top": upper[2],
        "depth": upper[1] - lower[1],
        "extras": quay_and_water(wedge),
        "closeups": [
            {"caption": "From the Elbe", "target": middle, "direction": (0.05, -1.0, 0.12), "distance": 260.0,
             "lens": 35.0},
            {"caption": "From the east", "target": middle, "direction": (1.0, -0.35, 0.15), "distance": 230.0,
             "lens": 35.0},
            {"caption": "The roof", "target": (0.0, 0.0, upper[2] * 0.8), "direction": (-0.3, -0.6, 1.0),
             "distance": 200.0, "lens": 35.0},
            {"caption": "Plaza and arch", "target": (arch_x, arch_y, PLAZA_FLOOR + 4.0),
             "direction": (-0.15, -1.0, 0.05), "distance": 45.0, "lens": 35.0},
        ],
    }]
