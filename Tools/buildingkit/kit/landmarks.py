"""Landmarks (#111): buildings that get a model of their own instead of kit pieces.

The rule for a landmark: the footprint and heights come from the Hamburg LoD2 model, the shapes LoD2 simplifies are
measured in the bDOM (the 1 m surface model), and everything else is modelled by hand from photos.

The Elbphilharmonie reads its footprint from the LoD2 archive in the data root (downloads/lod2_hamburg). Its wave roof
is modelled by hand: the points along each facade's roof edge and the low points between them were measured once in
the 2020 bDOM and checked against photos, and are constants here. Heights of the brick base and the plaza come from
the German Wikipedia article.

Frame: metres, Z up, x east and y north, the origin at the middle of the footprint on the quay (8.3 m above sea
level). The spec records the UTM 32N position and the sea level height of the origin, so the runtime can place it.
"""

import math
import os
import random
import re
import subprocess
from collections import OrderedDict

from .geom import Mesh

DATA_ROOT = os.environ.get("THIRD_GEAR_DATA", "/mnt/storage/third-gear")
LOD2_ZIP = os.path.join(DATA_ROOT, "downloads", "lod2_hamburg", "LoD2-DE_HH.zip")
QUAY_LEVEL = 8.3  # sea level height of the quay around the building, measured in the bDOM
PLAZA_LEVEL = 37.0  # the plaza on top of the Kaispeicher, above sea level
PLAZA_CLEAR = 4.0  # from the plaza floor to the underside of the glass superstructure, estimated from photos
PLAZA_SETBACK = 3.0  # how far the plaza's glazing stands back from the facade
GLASS_PANEL = (5.0, 3.5)  # width and height of one facade element

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



class EdgeProfile:
    """The roof edge along one facade: sharp points at (fraction along the facade, height above the quay), joined by
    arcs that hang down to the given low point between each pair."""

    def __init__(self, points, lows):
        self.points = points
        self.lows = lows

    def height(self, fraction):
        """Height of the edge at a fraction along the facade."""
        for (start, start_height), (end, end_height), low in zip(self.points, self.points[1:], self.lows):
            if fraction > end and end < 1.0:
                continue
            t = min(max((fraction - start) / (end - start), 0.0), 1.0)
            straight = start_height + (end_height - start_height) * t
            sag = max(0.0, (start_height + end_height) / 2.0 - low)
            # The power below one steepens the arc toward its points, so neighbouring arcs meet in a sharp crest.
            return straight - sag * math.sin(math.pi * t) ** 0.7
        return self.points[-1][1]


# Measured in the bDOM along each facade, sharpened from photos. South and north run from west to east (fraction = u),
# east and west from south to north (fraction = v).
SOUTH_EDGE = EdgeProfile([(0.0, 100.0), (0.33, 92.0), (0.74, 83.0), (1.0, 74.0)], [88.0, 79.0, 70.0])
NORTH_EDGE = EdgeProfile([(0.0, 96.0), (0.28, 93.0), (0.73, 81.0), (1.0, 77.0)], [88.0, 77.0, 74.0])
EAST_EDGE = EdgeProfile([(0.0, 74.0), (0.52, 80.0), (1.0, 77.0)], [73.0, 74.0])
WEST_EDGE = EdgeProfile([(0.0, 100.0), (1.0, 96.0)], [93.0])
# Glazed openings cut into the roof, from the bDOM: centre (u, v), size along the length and across, depth.
ROOF_OPENINGS = [(0.33, 0.57, 4.0, 11.0, 2.5), (0.83, 0.54, 5.5, 19.0, 2.5)]


class HandRoof:
    """The wave roof as sheets spanned between the four edge profiles. Each point on the south edge is paired with
    one on the north edge, and the crease between them runs straight across the roof; the mesh follows the creases,
    so they stay sharp."""

    def __init__(self, wedge, row_spacing=1.0, column_spacing=1.0):
        self.wedge = wedge
        self.south_creases = [fraction for fraction, _ in SOUTH_EDGE.points]
        self.north_creases = [fraction for fraction, _ in NORTH_EDGE.points]
        rows = max(4, round(min(wedge.width(0.0), wedge.width(1.0)) / row_spacing))
        cusp_rows = [fraction for fraction, _ in EAST_EDGE.points + WEST_EDGE.points]
        self.rows = sorted(set([row / rows for row in range(rows + 1)] + cusp_rows))
        self.columns = []
        for index in range(len(self.south_creases) - 1):
            share = (self.south_creases[index + 1] - self.south_creases[index]
                     + self.north_creases[index + 1] - self.north_creases[index]) / 2.0
            self.columns.append(max(3, round(share * wedge.length() / column_spacing)))

    def creases_at(self, v):
        """u of each crease where it crosses the row at v."""
        return [(1.0 - v) * south + v * north for south, north in zip(self.south_creases, self.north_creases)]

    def height(self, u, v):
        """Roof height above the quay at (u, v): the south and north edges blended across along the creases, with
        the west and east edges' differences blended in along the length (a Coons patch), less any opening."""
        creases = self.creases_at(v)
        index = 0
        while index < len(creases) - 2 and u > creases[index + 1]:
            index += 1
        t = (u - creases[index]) / (creases[index + 1] - creases[index])
        south_u = self.south_creases[index] + t * (self.south_creases[index + 1] - self.south_creases[index])
        north_u = self.north_creases[index] + t * (self.north_creases[index + 1] - self.north_creases[index])
        across = (1.0 - v) * SOUTH_EDGE.height(south_u) + v * NORTH_EDGE.height(north_u)
        west_difference = WEST_EDGE.height(v) - ((1.0 - v) * SOUTH_EDGE.height(0.0) + v * NORTH_EDGE.height(0.0))
        east_difference = EAST_EDGE.height(v) - ((1.0 - v) * SOUTH_EDGE.height(1.0) + v * NORTH_EDGE.height(1.0))
        height = across + (1.0 - u) * west_difference + u * east_difference
        opening = self.opening_at(u, v)
        if opening is not None:
            height -= opening[4]
        return height

    def opening_at(self, u, v):
        """The roof opening that contains (u, v), if any."""
        for opening in ROOF_OPENINGS:
            centre_u, centre_v, size_u, size_v, _depth = opening
            along = abs(u - centre_u) * self.wedge.length()
            across = abs(v - centre_v) * self.wedge.width(u)
            if along < size_u / 2.0 and across < size_v / 2.0:
                return opening
        return None

    def node_grid(self):
        """(u, v) of every mesh node, row by row from the south edge, columns placed between the creases."""
        grid = []
        for v in self.rows:
            creases = self.creases_at(v)
            row = []
            for index, count in enumerate(self.columns):
                start, end = creases[index], creases[index + 1]
                steps = range(count) if index < len(self.columns) - 1 else range(count + 1)
                row += [(start + (end - start) * step / count, v) for step in steps]
            grid.append(row)
        return grid

    def outline(self):
        """The roof edge counter-clockwise from the south-west corner, as (u, v, facade index), on the mesh's nodes:
        south, east, north and west facade."""
        grid = self.node_grid()
        outline = [(u, v, 0) for u, v in grid[0][:-1]]
        outline += [(1.0, v, 1) for v in self.rows[:-1]]
        outline += [(u, v, 2) for u, v in reversed(grid[-1][1:])]
        outline += [(0.0, v, 3) for v in reversed(self.rows[1:])]
        return outline


# ================================================================================================= geometry
PLAZA_FLOOR = PLAZA_LEVEL - QUAY_LEVEL
GLASS_BOTTOM = PLAZA_FLOOR + PLAZA_CLEAR


class Arch:
    """An arch cut into the glass superstructure above the plaza: elliptical, centred at a fraction u of the length
    from the west, with a white vault running depth metres into the building."""

    def __init__(self, side, centre, half_width, rise, depth):
        self.side = side  # "south" or "north"
        self.centre = centre
        self.half_width = half_width
        self.rise = rise
        self.depth = depth

    def height(self, offset):
        """Height above the quay of the arch's edge at offset (-1 to 1 across its width), or None outside it."""
        if abs(offset) >= 1.0:
            return None
        return PLAZA_FLOOR + self.rise * math.sqrt(1.0 - offset * offset)


# The low wide arch over the plaza facing the Elbe, and the tall narrow one on the city side, both from photos.
ARCHES = [Arch("south", 0.67, 10.0, 7.0, 8.0), Arch("north", 0.67, 5.0, 20.0, 10.0)]
# Slots between the brick blocks, as fractions along the long facades from the west: dark with balcony boxes on the
# south side, white stair towers on the north side.
SOUTH_SLOTS = (0.32, 0.53, 0.71, 0.89)
SOUTH_SLOT_BALCONIES = ((16.0,), (10.0, 22.0), (14.0,), (19.0,))
NORTH_SLOTS = (0.32, 0.56, 0.83)
ARCH_BACK_SCALE = 0.55  # the vault's back opening, as a share of the arch at the facade
KAISTUDIO_WINDOW = (0.07, 6.5, 18.0, 25.0)  # fraction from the west on the south facade, width, bottom, top


def outward_normal(start, end):
    """Horizontal outward normal of a counter-clockwise outline edge."""
    dx, dy = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dy)
    return (dy / length, -dx / length, 0.0)


def facade_point(point, normal, proud):
    """A footprint point pushed out from the facade."""
    return (point[0] + normal[0] * proud, point[1] + normal[1] * proud)


class Facade:
    """One straight facade of the wedge: its ends, outward normal, and how a fraction along it maps to (u, v)."""

    def __init__(self, name, start, end, to_uv):
        self.name = name
        self.start = start
        self.end = end
        self.to_uv = to_uv
        self.length = math.dist(start, end)
        self.direction = ((end[0] - start[0]) / self.length, (end[1] - start[1]) / self.length)
        self.normal = outward_normal(start, end)

    def point(self, along, z, proud):
        """The 3D point along metres from the start at height z, proud metres in front of the facade."""
        x = self.start[0] + self.direction[0] * along + self.normal[0] * proud
        y = self.start[1] + self.direction[1] * along + self.normal[1] * proud
        return (x, y, z)


class Elbphilharmonie:
    """Builds the model from the measured wedge and roof, with the details added by hand from photos."""

    def __init__(self, wedge, roof):
        self.wedge = wedge
        self.roof = roof
        self.mesh = Mesh()
        w = wedge
        self.facades = [
            Facade("south", w.west_south, w.east_south, lambda f: (f, 0.0)),
            Facade("east", w.east_south, w.east_north, lambda f: (1.0, f)),
            Facade("north", w.east_north, w.west_north, lambda f: (1.0 - f, 1.0)),
            Facade("west", w.west_north, w.west_south, lambda f: (0.0, 1.0 - f)),
        ]

    def build(self):
        """All parts in order, bottom to top."""
        self.add_brick_base()
        self.add_plaza()
        self.add_glass_walls()
        for arch in ARCHES:
            self.add_arch(arch)
        for index, facade in enumerate(self.facades):
            self.add_glass_openings(facade, index)
        self.add_roof()
        return self.mesh

    # ------------------------------------------------------------------ brick base
    def add_brick_base(self):
        """The Kaispeicher: brick walls up to the plaza floor with real openings (windows, slots, doors and the
        Kaistudio window set into the wall), the balcony boxes in the south slots and the white stair towers on the
        north side."""
        corners = [self.wedge.west_south, self.wedge.east_south, self.wedge.east_north, self.wedge.west_north]
        self.mesh.prism(corners, "xy", 0.0, PLAZA_FLOOR, "Brick", caps="H", sides=False)
        for facade in self.facades:
            openings = self.brick_openings(facade)
            self.add_wall_with_openings(facade, openings)
            for opening in openings:
                self.add_opening(facade, opening)
            if facade.name == "south":
                for slot_index, fraction in enumerate(SOUTH_SLOTS):
                    for bottom in SOUTH_SLOT_BALCONIES[slot_index]:
                        self.balcony_box(facade, fraction * facade.length, bottom)
            if facade.name == "north":
                for fraction in NORTH_SLOTS:
                    self.stair_tower(facade, (1.0 - fraction) * facade.length)

    def brick_openings(self, facade):
        """Every opening in one brick facade as (start, end, bottom, top, depth, back material), along the facade
        in metres from its start."""
        openings = []
        slots = []
        if facade.name == "south":
            slots = [fraction * facade.length for fraction in SOUTH_SLOTS]
            for along in slots:
                openings.append((along - 1.5, along + 1.5, 0.6, PLAZA_FLOOR - 0.4, 0.8, "PowderCoat"))
            fraction, width, bottom, top = KAISTUDIO_WINDOW
            along = fraction * facade.length
            openings.append((along - width / 2.0, along + width / 2.0, bottom, top, 0.6, "PowderCoat"))
        if facade.name == "north":
            slots = [(1.0 - fraction) * facade.length for fraction in NORTH_SLOTS]
        door_count = int(facade.length / 16.0)
        doors = [(door + 0.5) * facade.length / door_count for door in range(door_count)]
        for along in doors:
            if any(abs(along - slot) < 3.5 for slot in slots):
                continue
            openings.append((along - 1.6, along + 1.6, 0.0, 3.6, 0.4, "PowderCoat"))
        openings += self.window_openings(facade, slots)
        return openings

    def window_openings(self, facade, slots):
        """The warehouse's small square windows, set 35 cm into the wall, in rows a storey apart, clear of the slots,
        the stair towers and the Kaistudio window."""
        windows = []
        columns = int(facade.length / 4.2)
        for row in range(1, 9):
            z = 2.0 + row * 2.9
            if z + 0.9 > PLAZA_FLOOR - 0.6:
                break
            for column in range(columns):
                along = (column + 0.5) * facade.length / columns
                if any(abs(along - slot) < 3.0 for slot in slots):
                    continue
                if facade.name == "south" and abs(along - KAISTUDIO_WINDOW[0] * facade.length) < 4.5 and z > 16.0:
                    continue
                windows.append((along - 0.45, along + 0.45, z, z + 0.9, 0.35, "PowderCoat"))
        return windows

    def add_wall_with_openings(self, facade, openings):
        """The brick face of a facade with holes where the openings are: the wall is cut into bands at every
        opening's top and bottom, and each band into runs of solid wall between the openings."""
        heights = sorted({0.0, PLAZA_FLOOR} | {value for opening in openings for value in opening[2:4]})
        for bottom, top in zip(heights, heights[1:]):
            middle = (bottom + top) / 2.0
            cut_by = sorted((start, end) for start, end, low, high, _depth, _material in openings
                            if low < middle < high)
            position = 0.0
            for start, end in cut_by + [(facade.length, facade.length)]:
                if start > position + 1e-6:
                    self.rectangle(facade, position, start, bottom, top, "Brick", 0.0)
                position = max(position, end)

    def add_opening(self, facade, opening):
        """The inside of one opening: the brick reveals on all four sides, a stone sill at the bottom, and the dark
        back at its depth."""
        start, end, bottom, top, depth, material = opening
        front = [facade.point(start, bottom, 0.0), facade.point(end, bottom, 0.0), facade.point(end, top, 0.0),
                 facade.point(start, top, 0.0)]
        back = [facade.point(start, bottom, -depth), facade.point(end, bottom, -depth),
                facade.point(end, top, -depth), facade.point(start, top, -depth)]
        self.mesh.add_face(back, material, desired_normal=facade.normal)
        direction = (facade.direction[0], facade.direction[1], 0.0)
        sides = [(0, 1, (0.0, 0.0, 1.0), "Concrete"), (1, 2, (-direction[0], -direction[1], 0.0), "Brick"),
                 (2, 3, (0.0, 0.0, -1.0), "Brick"), (3, 0, direction, "Brick")]
        for first, second, normal, side_material in sides:
            if first == 0 and bottom <= 0.0:
                continue
            self.mesh.add_face([front[first], front[second], back[second], back[first]], side_material,
                               desired_normal=normal)

    def balcony_box(self, facade, along, bottom):
        """A grey box balcony sticking out of a slot, 3 m wide, 1.2 m deep and 1 m high."""
        corners = [facade.point(along - 1.5, 0.0, 0.0), facade.point(along + 1.5, 0.0, 0.0),
                   facade.point(along + 1.5, 0.0, 1.2), facade.point(along - 1.5, 0.0, 1.2)]
        self.mesh.prism([(x, y) for x, y, _ in corners], "xy", bottom, bottom + 1.0, "Metal")

    def stair_tower(self, facade, along):
        """A white stair tower standing 0.8 m proud of the north facade, up to the plaza floor."""
        corners = [facade.point(along - 2.0, 0.0, 0.0), facade.point(along + 2.0, 0.0, 0.0),
                   facade.point(along + 2.0, 0.0, 0.8), facade.point(along - 2.0, 0.0, 0.8)]
        self.mesh.prism([(x, y) for x, y, _ in corners], "xy", 0.0, PLAZA_FLOOR + 0.3, "Paint", caps="H")

    def rectangle(self, facade, along_start, along_end, bottom, top, material, proud):
        """A flat rectangle on a facade, proud metres in front of it."""
        self.mesh.add_face([facade.point(along_start, bottom, proud), facade.point(along_end, bottom, proud),
                            facade.point(along_end, top, proud), facade.point(along_start, top, proud)], material,
                           desired_normal=facade.normal)

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
    def arch_at(self, u, v):
        """The arch whose opening contains the outline point (u, v), and the point's offset across it."""
        for arch in ARCHES:
            on_side = (arch.side == "south" and v == 0.0) or (arch.side == "north" and v == 1.0)
            if not on_side:
                continue
            offset = (u - arch.centre) * self.wedge.length() / arch.half_width
            if abs(offset) < 1.0:
                return arch, offset
        return None, 0.0

    def wall_bottom(self, u, v):
        """Lower edge of the glass wall at an outline point: the glass bottom, or the edge of an arch."""
        arch, offset = self.arch_at(u, v)
        if arch is None:
            return GLASS_BOTTOM
        return max(GLASS_BOTTOM, arch.height(offset))

    def add_glass_walls(self):
        """The superstructure's facades from the glass bottom (or an arch) up to the roof edge, on the roof's outline
        nodes so the two meet exactly. Their UVs count facade elements, so one tile of the FacadeGlass texture is one
        element."""
        width, height = GLASS_PANEL
        outline = self.roof.outline()
        travelled = 0.0
        for index, (u, v, facade) in enumerate(outline):
            next_u, next_v, _next_facade = outline[(index + 1) % len(outline)]
            if index == 0 or facade != outline[index - 1][2]:
                travelled = 0.0
            start, end = self.wedge.point(u, v), self.wedge.point(next_u, next_v)
            step = math.dist(start, end)
            bottoms = (self.wall_bottom(u, v), self.wall_bottom(next_u, next_v))
            tops = (self.roof.height(u, v), self.roof.height(next_u, next_v))
            uvs = [(travelled / width, (bottoms[0] - GLASS_BOTTOM) / height),
                   ((travelled + step) / width, (bottoms[1] - GLASS_BOTTOM) / height),
                   ((travelled + step) / width, (tops[1] - GLASS_BOTTOM) / height),
                   (travelled / width, (tops[0] - GLASS_BOTTOM) / height)]
            self.mesh.add_face([(start[0], start[1], bottoms[0]), (end[0], end[1], bottoms[1]),
                                (end[0], end[1], tops[1]), (start[0], start[1], tops[0])], "FacadeGlass",
                               desired_normal=outward_normal(start, end), uvs=uvs)
            travelled += step

    def add_glass_openings(self, facade, seed):
        """The balconies and slits in the glass, one element at a time: white-lipped horseshoe balconies, clustered
        high up toward the west as in the photos, and dark eye-shaped slits, denser toward the east."""
        rng = random.Random(seed * 7 + 1)
        width, height = GLASS_PANEL
        columns = int(facade.length / width)
        for column in range(columns):
            fraction = (column + 0.5) / columns
            u, v = facade.to_uv(fraction)
            roof = self.roof.height(u, v)
            along = fraction * facade.length
            row = 0
            while GLASS_BOTTOM + (row + 1) * height < roof - 1.5:
                bottom = GLASS_BOTTOM + row * height
                row += 1
                if self.near_arch(u, v, bottom + height):
                    continue
                self.maybe_opening(facade, rng, along, bottom, u)

    def near_arch(self, u, v, top):
        """Whether an element reaching up to top would cut into an arch or its frame."""
        for arch in ARCHES:
            on_side = (arch.side == "south" and v == 0.0) or (arch.side == "north" and v == 1.0)
            reach = (arch.half_width + 4.0) / self.wedge.length()
            if on_side and abs(u - arch.centre) < reach and top < PLAZA_FLOOR + arch.rise + 6.0:
                return True
        return False

    def maybe_opening(self, facade, rng, along, bottom, u):
        """Rolls for a balcony or a slit in one element."""
        horseshoe_chance = 0.4 if u < 0.55 and bottom > 22.0 else 0.06
        slit_chance = 0.16 if u > 0.35 else 0.05
        if facade.name == "east":
            slit_chance = 0.25
        roll = rng.random()
        if roll < horseshoe_chance:
            self.horseshoe(facade, along + rng.uniform(-0.8, 0.8), bottom + 0.4)
        elif roll < horseshoe_chance + slit_chance:
            self.slit(facade, along + rng.uniform(-1.2, 1.2), bottom + 0.6)

    def horseshoe(self, facade, along, bottom, half_width=1.3, depth=1.5, segments=12):
        """A horseshoe balcony: the dark opening of a half ellipse hanging from a straight top edge, with the white
        bent-glass lip around its curve standing out from the facade."""
        top = bottom + depth + 0.6
        opening = []
        lip_inner = []
        lip_outer = []
        for step in range(segments + 1):
            angle = math.pi * step / segments
            x, z = math.cos(angle), math.sin(angle)
            opening.append(facade.point(along + half_width * x, top - depth * z, 0.03))
            lip_inner.append(facade.point(along + half_width * x, top - depth * z, 0.04))
            lip_outer.append(facade.point(along + half_width * 1.18 * x, top - depth * 1.18 * z, 0.35))
        self.mesh.add_face(opening, "PowderCoat", desired_normal=facade.normal)
        for step in range(segments):
            quad = [lip_inner[step], lip_inner[step + 1], lip_outer[step + 1], lip_outer[step]]
            self.mesh.add_face(quad, "Paint", smooth=True, desired_normal=facade.normal)

    def slit(self, facade, along, bottom, half_width=0.28, height=2.2, segments=8):
        """A dark eye-shaped slit where the glass is bent open."""
        right = []
        left = []
        for step in range(segments + 1):
            t = step / segments
            bulge = half_width * math.sin(math.pi * t)
            z = bottom + height * t
            right.append(facade.point(along + bulge, z, 0.03))
            left.append(facade.point(along - bulge, z, 0.03))
        self.mesh.add_face(right + list(reversed(left[1:-1])), "PowderCoat", desired_normal=facade.normal)

    # ------------------------------------------------------------------ arches and roof
    def add_arch(self, arch):
        """The vault behind an arch: a white soffit that narrows into the building like a funnel, so its inside shows
        from the front, closed at the back by a dark wall."""
        steps = 24
        facade_v = 0.0 if arch.side == "south" else 1.0
        inward = 1.0 if arch.side == "south" else -1.0
        previous = None
        for step in range(steps + 1):
            offset = -1.0 + 2.0 * step / steps
            front = self.arch_point(arch, offset, 1.0, facade_v, 0.0)
            back = self.arch_point(arch, offset, ARCH_BACK_SCALE, facade_v, inward * arch.depth)
            if previous is not None:
                self.add_vault_step(arch, previous, (front, back), facade_v, inward)
            previous = (front, back)

    def arch_point(self, arch, offset, scale, facade_v, depth):
        """A point on an arch's edge at offset across it, with the arch scaled down by scale, depth metres behind
        the facade (negative toward -v)."""
        u = arch.centre + offset * scale * arch.half_width / self.wedge.length()
        v = facade_v + depth / self.wedge.width(u)
        x, y = self.wedge.point(u, v)
        z = PLAZA_FLOOR + arch.rise * scale * math.sqrt(max(0.0, 1.0 - offset * offset))
        return (x, y, z)

    def add_vault_step(self, arch, previous, current, facade_v, inward):
        """One step of a vault: the soffit strip, facing the arch's axis, and the dark back wall below it."""
        front_before, back_before = previous
        front, back = current
        axis = self.wedge.point(arch.centre, facade_v + inward * arch.depth / 2.0 / self.wedge.width(arch.centre))
        middle = [(front[k] + back[k]) / 2.0 for k in range(3)]
        toward_axis = (axis[0] - middle[0], axis[1] - middle[1], PLAZA_FLOOR - middle[2])
        self.mesh.add_face([front_before, front, back, back_before], "Paint", smooth=True, desired_normal=toward_axis)
        self.mesh.add_face([(back_before[0], back_before[1], PLAZA_FLOOR), (back[0], back[1], PLAZA_FLOOR), back,
                            back_before], "PowderCoat", desired_normal=(0.0, -inward, 0.0))

    def add_roof(self):
        """The roof sheets on the roof's node grid, flat-shaded so the creases stay sharp; cells in an opening are
        dark glass."""
        grid = self.roof.node_grid()
        for row, next_row in zip(grid, grid[1:]):
            for column in range(len(row) - 1):
                corners = [row[column], row[column + 1], next_row[column + 1], next_row[column]]
                points = []
                for u, v in corners:
                    x, y = self.wedge.point(u, v)
                    points.append((x, y, self.roof.height(u, v)))
                middle_u = sum(u for u, _ in corners) / 4.0
                middle_v = sum(v for _, v in corners) / 4.0
                material = "RoofSequins"
                if self.roof.opening_at(middle_u, middle_v) is not None:
                    material = "PowderCoat"
                self.mesh.add_face(points, material, desired_normal=(0.0, 0.0, 1.0))


# ================================================================================================= kit entry
_CACHE = {}


def elbphilharmonie():
    """The model and its wedge, built once per run."""
    if "model" not in _CACHE:
        wedge = Wedge(read_footprint())
        roof = HandRoof(wedge)
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


def preview_textures():
    """The facade element texture from posters/draw_landmarks.py, for the sheet."""
    posters = os.path.join(DATA_ROOT, "building_kit", "posters")
    return {"FacadeGlass": os.path.join(posters, "elbphilharmonie_glass.png")}


def assemblies():
    """The Elbphilharmonie from the Elbe and from the city side like the reference photos, from the east, the roof
    from above, and both arches up close."""
    mesh, wedge, _roof = elbphilharmonie()
    lower, upper = mesh.bounds()
    south_arch, north_arch = ARCHES
    south_x, south_y = wedge.point(south_arch.centre, 0.0)
    north_x, north_y = wedge.point(north_arch.centre, 1.0)
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
            {"caption": "From the city side", "target": middle, "direction": (0.35, 1.0, 0.35), "distance": 240.0,
             "lens": 35.0},
            {"caption": "From the east", "target": middle, "direction": (1.0, -0.35, 0.15), "distance": 230.0,
             "lens": 35.0},
            {"caption": "The roof", "target": (0.0, 0.0, upper[2] * 0.8), "direction": (-0.3, -0.6, 1.0),
             "distance": 200.0, "lens": 35.0},
            {"caption": "Plaza arch, Elbe side", "target": (south_x, south_y, PLAZA_FLOOR + 5.0),
             "direction": (-0.15, -1.0, 0.1), "distance": 55.0, "lens": 35.0},
            {"caption": "Tall arch, city side", "target": (north_x, north_y, PLAZA_FLOOR + 12.0),
             "direction": (0.1, 1.0, 0.15), "distance": 70.0, "lens": 35.0},
        ],
    }]
