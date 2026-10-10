"""Pure-Python mesh builder for the building kit (no Blender needed).

Conventions (see README.md): metres, Z up, the wall face on the plane Y = 0, the wall body toward +Y, the facade seen
from -Y with X to the right. UVs are in world metres, so one UV unit is one metre of surface.
"""

import math
import random

# Material slot names shared by every style.
MATERIALS = ("Brick", "Plaster", "Timber", "Frame", "Glass", "Sill", "RoofTile", "Thatch", "Metal", "Concrete", "Paint",
             "PaintRed", "PaintGreen", "Beacon", "Lattice", "Insulator",
             "PowderCoat", "AdPanel", "Timetable", "StopSignFace", "BinSticker", "Litter", "BinBag",
             "GreenhouseGlass", "Foil", "Aluminium",
             "BrandPaint", "BrandLogo", "PriceBoard", "PumpFace", "WashSign", "CanopyLight", "SafetyYellow",
             "WashBrush", "Asphalt", "Ground", "Water")


def vec_sub(first, second):
    return (first[0] - second[0], first[1] - second[1], first[2] - second[2])


def vec_cross(first, second):
    return (
        first[1] * second[2] - first[2] * second[1],
        first[2] * second[0] - first[0] * second[2],
        first[0] * second[1] - first[1] * second[0],
    )


def vec_dot(first, second):
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2]


def vec_normalize(vector):
    length = math.sqrt(vec_dot(vector, vector))
    if length < 1e-12:
        return (0.0, 0.0, 0.0)
    return (vector[0] / length, vector[1] / length, vector[2] / length)


def polygon_normal(points):
    """Newell normal of a planar polygon, unnormalised."""
    normal_x = normal_y = normal_z = 0.0
    for index, current in enumerate(points):
        following = points[(index + 1) % len(points)]
        normal_x += (current[1] - following[1]) * (current[2] + following[2])
        normal_y += (current[2] - following[2]) * (current[0] + following[0])
        normal_z += (current[0] - following[0]) * (current[1] + following[1])
    return (normal_x, normal_y, normal_z)


def uv_for_face(points):
    """Planar UVs in metres. U runs horizontally to the right as seen from outside, V runs up the face (or along Y for
    horizontal faces), so a texture that tiles per metre lines up across neighbouring faces of the same plane."""
    normal = vec_normalize(polygon_normal(points))
    if abs(normal[2]) > 0.999:
        u_axis = (1.0, 0.0, 0.0)
        v_axis = (0.0, 1.0, 0.0) if normal[2] > 0 else (0.0, -1.0, 0.0)
    else:
        u_axis = vec_normalize((-normal[1], normal[0], 0.0))
        up_dot_normal = normal[2]
        v_axis = vec_normalize((-normal[0] * up_dot_normal, -normal[1] * up_dot_normal, 1.0 - normal[2] * up_dot_normal))
    return [(vec_dot(point, u_axis), vec_dot(point, v_axis)) for point in points]


class Mesh:
    """A bag of polygons; each polygon carries its own UVs, material name and smooth flag."""

    def __init__(self):
        self.faces = []

    # ------------------------------------------------------------------ basic building blocks
    def add_face(self, points, material, smooth=False, desired_normal=None, uvs=None):
        """Adds one polygon. If desired_normal is given the winding is flipped when it disagrees."""
        points = [tuple(point) for point in points]
        if desired_normal is not None:
            if vec_dot(polygon_normal(points), desired_normal) < 0:
                points.reverse()
                if uvs is not None:
                    uvs = list(reversed(uvs))
        if uvs is None:
            uvs = uv_for_face(points)
        self.faces.append({"points": points, "uvs": uvs, "material": material, "smooth": smooth})

    def box(self, x0, x1, y0, y1, z0, z1, material, skip="", materials=None):
        """Axis-aligned box. skip is a string of face codes to leave out: F front (-Y), B back (+Y), L (-X), R (+X),
        T top (+Z), D bottom (-Z). materials maps a face code to a different material."""
        if x1 - x0 <= 1e-9 or y1 - y0 <= 1e-9 or z1 - z0 <= 1e-9:
            return
        materials = materials or {}
        corners = {
            "F": [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
            "B": [(x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1)],
            "L": [(x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)],
            "R": [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],
            "T": [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
            "D": [(x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0)],
        }
        for code, points in corners.items():
            if code in skip:
                continue
            self.add_face(points, materials.get(code, material))

    def quad_double(self, points, material):
        """A thin sheet visible from both sides, e.g. glass."""
        self.add_face(points, material)
        self.add_face(list(reversed(points)), material)

    def prism(self, polygon, plane, low, high, material, caps="LH", smooth_sides=False, side_material=None, sides=True):
        """Extrudes a 2D polygon (counter-clockwise in the plane's own axes) between two coordinates.

        plane 'xz' extrudes along Y (polygon points are (x, z)), 'yz' along X (points are (y, z)), 'xy' along Z.
        caps: 'L' keeps the low cap, 'H' the high cap."""

        def lift(point, coordinate):
            if plane == "xz":
                return (point[0], coordinate, point[1])
            if plane == "yz":
                return (coordinate, point[0], point[1])
            return (point[0], point[1], coordinate)

        axis_vector = {"xz": (0.0, 1.0, 0.0), "yz": (1.0, 0.0, 0.0), "xy": (0.0, 0.0, 1.0)}[plane]
        negative_axis = (-axis_vector[0], -axis_vector[1], -axis_vector[2])
        if "L" in caps:
            self.add_face([lift(point, low) for point in polygon], material, desired_normal=negative_axis)
        if "H" in caps:
            self.add_face([lift(point, high) for point in polygon], material, desired_normal=axis_vector)
        # Polygon winding decides which way the outward edge normal points.
        signed_area = 0.0
        for index, point in enumerate(polygon):
            following = polygon[(index + 1) % len(polygon)]
            signed_area += point[0] * following[1] - following[0] * point[1]
        winding = 1.0 if signed_area >= 0 else -1.0
        for index, point in enumerate(polygon):
            if not sides:
                break
            following = polygon[(index + 1) % len(polygon)]
            edge_x = following[0] - point[0]
            edge_y = following[1] - point[1]
            if abs(edge_x) < 1e-12 and abs(edge_y) < 1e-12:
                continue
            outward_2d = (edge_y * winding, -edge_x * winding)
            outward = vec_sub(lift(outward_2d, 0.0), lift((0.0, 0.0), 0.0))
            quad = [lift(point, low), lift(following, low), lift(following, high), lift(point, high)]
            self.add_face(quad, side_material or material, smooth=smooth_sides, desired_normal=outward)

    def sweep(self, profile, path, material, closed_ends=True, smooth=False, base_z=0.0):
        """Pulls a profile along a horizontal polyline with mitred corners.

        profile: list of (offset, z) with offset measured outward (to the right of the travel direction), listed from
        the bottom, outward and up, and back toward the wall at the top. path: list of (x, y). Outward is to the right
        of the direction of travel, so a straight run along +X faces -Y."""
        count = len(path)
        directions = []
        for index in range(count - 1):
            dx = path[index + 1][0] - path[index][0]
            dy = path[index + 1][1] - path[index][1]
            length = math.hypot(dx, dy)
            directions.append((dx / length, dy / length))
        normals = [(direction[1], -direction[0]) for direction in directions]
        mitres = []
        for index in range(count):
            if index == 0:
                mitres.append(normals[0])
            elif index == count - 1:
                mitres.append(normals[-1])
            else:
                before, after = normals[index - 1], normals[index]
                denominator = 1.0 + before[0] * after[0] + before[1] * after[1]
                mitres.append(((before[0] + after[0]) / denominator, (before[1] + after[1]) / denominator))

        def ring(path_index):
            return [
                (path[path_index][0] + mitres[path_index][0] * offset, path[path_index][1] + mitres[path_index][1] * offset, z + base_z)
                for offset, z in profile
            ]

        rings = [ring(index) for index in range(count)]
        for path_index in range(count - 1):
            tangent = directions[path_index]
            for profile_index in range(len(profile) - 1):
                p00 = rings[path_index][profile_index]
                p01 = rings[path_index + 1][profile_index]
                p11 = rings[path_index + 1][profile_index + 1]
                p10 = rings[path_index][profile_index + 1]
                offset_change = profile[profile_index + 1][0] - profile[profile_index][0]
                z_change = profile[profile_index + 1][1] - profile[profile_index][1]
                outward_normal = (
                    normals[path_index][0] * z_change - 0.0,
                    normals[path_index][1] * z_change - 0.0,
                    -offset_change,
                )
                if abs(z_change) < 1e-12 and abs(offset_change) < 1e-12:
                    continue
                self.add_face([p00, p01, p11, p10], material, smooth=smooth, desired_normal=outward_normal)
        if closed_ends:
            wall_ring_start = [(path[0][0], path[0][1], z + base_z) for _offset, z in profile]
            self.add_face(rings[0], material, desired_normal=(-directions[0][0], -directions[0][1], 0.0))
            self.add_face(rings[-1], material, desired_normal=(directions[-1][0], directions[-1][1], 0.0))
            del wall_ring_start

    def heightfield(self, x0, x1, y0, y1, columns, rows, height_function, material, skirt_depth=0.0, skirt_material=None):
        """A regular grid whose Z comes from height_function(x, y); optional vertical skirt down to -skirt_depth."""
        xs = [x0 + (x1 - x0) * column / columns for column in range(columns + 1)]
        ys = [y0 + (y1 - y0) * row / rows for row in range(rows + 1)]
        self.heightfield_nonuniform(xs, ys, height_function, material, skirt_depth, skirt_material)

    def heightfield_nonuniform(self, xs, ys, height_function, material, skirt_depth=0.0, skirt_material=None):
        """Like heightfield but with explicit grid lines, so a sharp step can be two nearly equal Y values."""
        columns = len(xs) - 1
        rows = len(ys) - 1
        x0, x1, y0, y1 = xs[0], xs[-1], ys[0], ys[-1]
        points = [[(xs[column], ys[row]) for column in range(columns + 1)] for row in range(rows + 1)]
        heights = [[height_function(px, py) for px, py in line] for line in points]
        for row in range(rows):
            for column in range(columns):
                corner_points = [
                    (*points[row][column], heights[row][column]),
                    (*points[row][column + 1], heights[row][column + 1]),
                    (*points[row + 1][column + 1], heights[row + 1][column + 1]),
                    (*points[row + 1][column], heights[row + 1][column]),
                ]
                self.add_face(corner_points, material, desired_normal=(0.0, 0.0, 1.0))
        if skirt_depth <= 0:
            return
        skirt_material = skirt_material or material
        edges = []
        for column in range(columns):
            edges.append(((points[0][column], heights[0][column]), (points[0][column + 1], heights[0][column + 1]), (0.0, -1.0, 0.0)))
            edges.append(((points[rows][column], heights[rows][column]), (points[rows][column + 1], heights[rows][column + 1]), (0.0, 1.0, 0.0)))
        for row in range(rows):
            edges.append(((points[row][0], heights[row][0]), (points[row + 1][0], heights[row + 1][0]), (-1.0, 0.0, 0.0)))
            edges.append(((points[row][columns], heights[row][columns]), (points[row + 1][columns], heights[row + 1][columns]), (1.0, 0.0, 0.0)))
        for (start, start_height), (end, end_height), outward in edges:
            quad = [
                (*start, start_height), (*end, end_height),
                (*end, -skirt_depth), (*start, -skirt_depth),
            ]
            self.add_face(quad, skirt_material, desired_normal=outward)
        self.add_face(
            [(x0, y0, -skirt_depth), (x1, y0, -skirt_depth), (x1, y1, -skirt_depth), (x0, y1, -skirt_depth)],
            skirt_material, desired_normal=(0.0, 0.0, -1.0),
        )

    def cylinder(self, center_x, center_y, z0, z1, radius, material, segments=12, caps="LH", radius_top=None):
        """Vertical cylinder (or cone frustum when radius_top differs), smooth sides."""
        radius_top = radius if radius_top is None else radius_top
        bottom = [(center_x + radius * math.cos(2 * math.pi * i / segments), center_y + radius * math.sin(2 * math.pi * i / segments), z0) for i in range(segments)]
        top = [(center_x + radius_top * math.cos(2 * math.pi * i / segments), center_y + radius_top * math.sin(2 * math.pi * i / segments), z1) for i in range(segments)]
        for i in range(segments):
            j = (i + 1) % segments
            mid_angle = 2 * math.pi * (i + 0.5) / segments
            outward = (math.cos(mid_angle), math.sin(mid_angle), 0.0)
            self.add_face([bottom[i], bottom[j], top[j], top[i]], material, smooth=True, desired_normal=outward)
        if "L" in caps and radius > 0:
            self.add_face(bottom, material, desired_normal=(0.0, 0.0, -1.0))
        if "H" in caps and radius_top > 0:
            self.add_face(top, material, desired_normal=(0.0, 0.0, 1.0))

    def tube(self, start, end, radius, material, segments=8, caps=True):
        """Round bar between two 3D points, smooth sides."""
        axis = vec_normalize(vec_sub(end, start))
        helper = (0.0, 0.0, 1.0) if abs(axis[2]) < 0.9 else (1.0, 0.0, 0.0)
        side_a = vec_normalize(vec_cross(axis, helper))
        side_b = vec_cross(axis, side_a)
        ring_start = []
        ring_end = []
        for i in range(segments):
            angle = 2 * math.pi * i / segments
            offset = tuple(radius * (math.cos(angle) * side_a[k] + math.sin(angle) * side_b[k]) for k in range(3))
            ring_start.append(tuple(start[k] + offset[k] for k in range(3)))
            ring_end.append(tuple(end[k] + offset[k] for k in range(3)))
        for i in range(segments):
            j = (i + 1) % segments
            angle = 2 * math.pi * (i + 0.5) / segments
            outward = tuple(math.cos(angle) * side_a[k] + math.sin(angle) * side_b[k] for k in range(3))
            self.add_face([ring_start[i], ring_start[j], ring_end[j], ring_end[i]], material, smooth=True, desired_normal=outward)
        if caps:
            self.add_face(ring_start, material, desired_normal=tuple(-c for c in axis))
            self.add_face(ring_end, material, desired_normal=axis)

    def pipe(self, points, radius, material, segments=8, caps=True):
        """Round bar along a 3D polyline with mitred bends, like a bent railing; smooth sides."""
        directions = [vec_normalize(vec_sub(end, start)) for start, end in zip(points, points[1:])]
        helper = (0.0, 0.0, 1.0) if abs(directions[0][2]) < 0.9 else (1.0, 0.0, 0.0)
        side_a = vec_normalize(vec_cross(directions[0], helper))
        side_b = vec_cross(directions[0], side_a)
        offsets = [ring_offsets(side_a, side_b, radius, segments)]
        for before, after in zip(directions, directions[1:]):
            side_a = rotate_between(side_a, before, after)
            side_b = rotate_between(side_b, before, after)
            offsets.append(ring_offsets(side_a, side_b, radius, segments))
        rings = [[tuple(points[0][k] + offset[k] for k in range(3)) for offset in offsets[0]]]
        for joint in range(1, len(points) - 1):
            rings.append(mitre_ring(points[joint], offsets[joint - 1], directions[joint - 1], directions[joint]))
        rings.append([tuple(points[-1][k] + offset[k] for k in range(3)) for offset in offsets[-1]])
        for index in range(len(rings) - 1):
            for i in range(segments):
                j = (i + 1) % segments
                outward = tuple(offsets[index][i][k] + offsets[index][j][k] for k in range(3))
                quad = [rings[index][i], rings[index][j], rings[index + 1][j], rings[index + 1][i]]
                self.add_face(quad, material, smooth=True, desired_normal=outward)
        if caps:
            self.add_face(rings[0], material, desired_normal=tuple(-c for c in directions[0]))
            self.add_face(rings[-1], material, desired_normal=directions[-1])

    def lathe(self, profile, material, segments=32, materials=None):
        """Body of revolution about the Y axis, smooth sides. profile is a list of (y, radius) with y never decreasing; two
        points with the same y make a flat ring step. An end with a radius above zero gets a flat cap. materials
        optionally maps a profile segment index to a material."""
        materials = materials or {}
        rings = []
        for y, radius in profile:
            rings.append([
                (radius * math.cos(2 * math.pi * i / segments), y, radius * math.sin(2 * math.pi * i / segments))
                for i in range(segments)
            ])
        for index in range(len(profile) - 1):
            segment_material = materials.get(index, material)
            (y0, r0), (y1, r1) = profile[index], profile[index + 1]
            for i in range(segments):
                j = (i + 1) % segments
                angle = 2 * math.pi * (i + 0.5) / segments
                # The outward normal leans along the axis by the slope of the profile.
                outward = (math.cos(angle) * (y1 - y0), -(r1 - r0), math.sin(angle) * (y1 - y0))
                if abs(y1 - y0) < 1e-9:
                    outward = (0.0, 1.0 if r0 > r1 else -1.0, 0.0)
                quad = [rings[index][i], rings[index][j], rings[index + 1][j], rings[index + 1][i]]
                if r0 < 1e-9:
                    quad = [rings[index][i], rings[index + 1][j], rings[index + 1][i]]
                elif r1 < 1e-9:
                    quad = [rings[index][i], rings[index][j], rings[index + 1][i]]
                # Flat ring steps stay flat-shaded; smoothing them across the axis gives crinkled shading.
                self.add_face(quad, segment_material, smooth=abs(y1 - y0) > 1e-9, desired_normal=outward)
        if profile[0][1] > 1e-9:
            self.add_face(rings[0], materials.get(0, material), desired_normal=(0.0, -1.0, 0.0))
        if profile[-1][1] > 1e-9:
            self.add_face(rings[-1], materials.get(len(profile) - 2, material), desired_normal=(0.0, 1.0, 0.0))

    # ------------------------------------------------------------------ transforms and queries
    def copy(self):
        clone = Mesh()
        clone.faces = [
            {"points": list(face["points"]), "uvs": list(face["uvs"]), "material": face["material"], "smooth": face["smooth"]}
            for face in self.faces
        ]
        return clone

    def add(self, other, dx=0.0, dy=0.0, dz=0.0):
        """Appends another mesh translated by (dx, dy, dz); UVs are kept as built."""
        for face in other.faces:
            self.faces.append({
                "points": [(p[0] + dx, p[1] + dy, p[2] + dz) for p in face["points"]],
                "uvs": list(face["uvs"]),
                "material": face["material"],
                "smooth": face["smooth"],
            })

    def transformed(self, matrix_function, flip=False):
        """Returns a copy with every point mapped through matrix_function. flip reverses the winding (for mirrors)."""
        result = Mesh()
        for face in self.faces:
            points = [matrix_function(point) for point in face["points"]]
            uvs = list(face["uvs"])
            if flip:
                points.reverse()
                uvs.reverse()
            result.faces.append({"points": points, "uvs": uvs, "material": face["material"], "smooth": face["smooth"]})
        return result

    def rotated_x(self, degrees):
        """Rotates about the X axis (positive tilts +Y toward +Z)."""
        cosine, sine = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
        return self.transformed(lambda p: (p[0], p[1] * cosine - p[2] * sine, p[1] * sine + p[2] * cosine))

    def rotated_z(self, degrees):
        cosine, sine = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
        return self.transformed(lambda p: (p[0] * cosine - p[1] * sine, p[0] * sine + p[1] * cosine, p[2]))

    def translated(self, dx=0.0, dy=0.0, dz=0.0):
        return self.transformed(lambda p: (p[0] + dx, p[1] + dy, p[2] + dz))

    def mirrored_x(self, width):
        """Mirrors across the plane x = width / 2, so a piece of that width stays in [0, width]."""
        return self.transformed(lambda p: (width - p[0], p[1], p[2]), flip=True)

    def bounds(self):
        xs = [p[0] for face in self.faces for p in face["points"]]
        ys = [p[1] for face in self.faces for p in face["points"]]
        zs = [p[2] for face in self.faces for p in face["points"]]
        return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))

    def triangle_count(self):
        return sum(len(face["points"]) - 2 for face in self.faces)

    def materials_used(self):
        used = []
        for face in self.faces:
            if face["material"] not in used:
                used.append(face["material"])
        return used


def ring_offsets(side_a, side_b, radius, segments):
    """Offsets of a ring of points around an axis spanned by two unit side vectors."""
    offsets = []
    for i in range(segments):
        angle = 2 * math.pi * i / segments
        offsets.append(tuple(radius * (math.cos(angle) * side_a[k] + math.sin(angle) * side_b[k]) for k in range(3)))
    return offsets


def rotate_between(vector, before, after):
    """Rotates a vector by the rotation that turns the unit direction before into after (Rodrigues), so a pipe's
    cross-section frame follows its bends without twisting."""
    axis = vec_cross(before, after)
    sine = math.sqrt(vec_dot(axis, axis))
    cosine = vec_dot(before, after)
    if sine < 1e-9:
        return vector
    axis = (axis[0] / sine, axis[1] / sine, axis[2] / sine)
    cross = vec_cross(axis, vector)
    along = vec_dot(axis, vector) * (1 - cosine)
    return tuple(vector[k] * cosine + cross[k] * sine + axis[k] * along for k in range(3))


def mitre_ring(joint, offsets, before, after):
    """Points where a pipe arriving along before meets the mitre plane of the bend at joint."""
    plane_normal = vec_normalize((before[0] + after[0], before[1] + after[1], before[2] + after[2]))
    ring = []
    for offset in offsets:
        shift = -vec_dot(offset, plane_normal) / vec_dot(before, plane_normal)
        ring.append(tuple(joint[k] + offset[k] + before[k] * shift for k in range(3)))
    return ring


def segmental_arch_points(chord_width, rise, segments=14):
    """Points (x, z) of a circular arc over a chord from (0, 0) to (chord_width, 0) with the given rise."""
    radius = (chord_width * chord_width / 4.0 + rise * rise) / (2.0 * rise)
    center_x = chord_width / 2.0
    center_z = rise - radius
    start_angle = math.atan2(0.0 - center_z, 0.0 - center_x)
    end_angle = math.atan2(0.0 - center_z, chord_width - center_x)
    points = []
    for index in range(segments + 1):
        angle = start_angle + (end_angle - start_angle) * index / segments
        points.append((center_x + radius * math.cos(angle), center_z + radius * math.sin(angle)))
    return points


def smooth_noise(x, y, seed, frequency):
    """Cheap deterministic value noise in -1..1 for thatch and plaster irregularities."""
    generator_x = x * frequency
    generator_y = y * frequency
    cell_x = math.floor(generator_x)
    cell_y = math.floor(generator_y)
    fraction_x = generator_x - cell_x
    fraction_y = generator_y - cell_y

    def corner(ix, iy):
        return random.Random(hash((ix, iy, seed)) & 0xFFFFFFFF).uniform(-1.0, 1.0)

    def fade(t):
        return t * t * (3 - 2 * t)

    top = corner(cell_x, cell_y) * (1 - fade(fraction_x)) + corner(cell_x + 1, cell_y) * fade(fraction_x)
    bottom = corner(cell_x, cell_y + 1) * (1 - fade(fraction_x)) + corner(cell_x + 1, cell_y + 1) * fade(fraction_x)
    return top * (1 - fade(fraction_y)) + bottom * fade(fraction_y)
