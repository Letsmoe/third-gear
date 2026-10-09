"""Pitched roofs of any outline from the straight skeleton (skeleton.py): planes with a texture mapping, the hip and ridge
lines for the ridge caps, and the profiles of a plain slope, a mansard and a slope that flattens out at a maximum height.

The skeleton is built on the footprint grown by the eave overhang, so the roof planes start at the eave line at height 0;
the game puts that line at the wall top. Height at skeleton time t (the distance from the eave line) is the profile's
piecewise linear function, which is why a plane that crosses a break of the profile is cut at the contour of that time.
"""
import math
from dataclasses import dataclass, field

import mapbox_earcut
import numpy as np
import shapely

from .skeleton import straight_skeleton

MAX_RISE = 7.0
MANSARD_LOWER_RISE = 2.6
MANSARD_LOWER_DEGREES = 70.0
MANSARD_UPPER_DEGREES = 28.0

KIND_SLOPE = 0       # the roof tile material, v runs up the slope
KIND_PLATEAU = 1     # flat top where the rise is capped, flat roof material, uv in plan metres


@dataclass
class RoofFace:
    kind: int
    vertices: np.ndarray   # (n, 5): x, y, height above the eave line, u, v
    triangles: np.ndarray  # (m, 3) indices


@dataclass
class RoofGeometry:
    faces: list = field(default_factory=list)
    caps: list = field(default_factory=list)   # (x0, y0, z0, x1, y1, z1) of hips and ridges
    top: float = 0.0


def profile_for(shape, pitch_degrees):
    """Piecewise slope profile as [(slope in degrees, time where it ends or None)], then capped at MAX_RISE."""
    if shape == "mansard":
        lower_time = MANSARD_LOWER_RISE / math.tan(math.radians(MANSARD_LOWER_DEGREES))
        upper_start = MANSARD_LOWER_RISE
        cap_time = lower_time + (MAX_RISE - upper_start) / math.tan(math.radians(MANSARD_UPPER_DEGREES))
        return [(MANSARD_LOWER_DEGREES, lower_time), (MANSARD_UPPER_DEGREES, cap_time), (0.0, None)]
    pitch = max(min(pitch_degrees, 65.0), 8.0)
    return [(pitch, MAX_RISE / math.tan(math.radians(pitch))), (0.0, None)]


def _height_and_length(profile, time):
    """Height and slope length of the roof at skeleton time (the distance from the eave line)."""
    height = length = 0.0
    start = 0.0
    for slope, end in profile:
        span = (time - start) if end is None else min(time, end) - start
        if span <= 0:
            break
        height += span * math.tan(math.radians(slope))
        length += span / math.cos(math.radians(slope))
        if end is None or time <= end:
            break
        start = end
    return height, length


def _break_times(profile):
    return [end for _, end in profile if end is not None]


def _clip_at(polygon, time, keep_above):
    """Sutherland-Hodgman clip of an (x, y, t) polygon against t = time."""
    result = []
    for index, current in enumerate(polygon):
        previous = polygon[index - 1]
        current_inside = current[2] >= time - 1e-9 if keep_above else current[2] <= time + 1e-9
        previous_inside = previous[2] >= time - 1e-9 if keep_above else previous[2] <= time + 1e-9
        if current_inside != previous_inside:
            span = current[2] - previous[2]
            if abs(span) > 1e-12:
                fraction = (time - previous[2]) / span
                result.append(previous + (current - previous) * fraction)
        if current_inside:
            result.append(current)
    return result


def _split_by_regimes(polygon, profile):
    """Pieces of an (x, y, t) polygon, each inside one slope regime of the profile: [(regime index, polygon)]."""
    bounds = [0.0] + _break_times(profile) + [float("inf")]
    pieces = []
    for regime in range(len(profile)):
        low, high = bounds[regime], bounds[regime + 1]
        piece = _clip_at(list(polygon), low, True) if low > 0 else list(polygon)
        if high != float("inf") and piece:
            piece = _clip_at(piece, high, False)
        if len(piece) >= 3 and _area(piece) > 1e-4:
            pieces.append((regime, piece))
    return pieces


def _area(polygon):
    points = np.array([point[:2] for point in polygon])
    x, y = points[:, 0], points[:, 1]
    return 0.5 * abs(float(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1))))


def _triangulate(points):
    ring = np.ascontiguousarray(points[:, :2], dtype=np.float64)
    indices = mapbox_earcut.triangulate_float64(ring, np.array([len(ring)], dtype=np.uint32))
    return np.asarray(indices, dtype=np.int64).reshape(-1, 3)


def _oriented(ring, counter_clockwise):
    points = np.asarray(ring.coords, dtype=float)[:-1]
    area = 0.5 * float(np.sum(points[:, 0] * np.roll(points[:, 1], -1) - np.roll(points[:, 0], -1) * points[:, 1]))
    return points if (area > 0) == counter_clockwise else points[::-1]


def eave_outline(footprint, overhang):
    """The footprint grown by the overhang with mitred corners, or None if it falls apart."""
    grown = footprint.buffer(overhang, join_style="mitre", mitre_limit=4.0) if overhang > 0 else footprint
    if isinstance(grown, shapely.MultiPolygon):
        grown = max(grown.geoms, key=lambda part: part.area)
    if not isinstance(grown, shapely.Polygon) or grown.is_empty:
        return None
    return grown.simplify(0.05)


def build_roof(footprint, shape, pitch_degrees, overhang):
    """RoofGeometry for the footprint polygon (shapely), or None when the skeleton fails."""
    outline = eave_outline(footprint, overhang)
    if outline is None:
        return None
    rings = [_oriented(outline.exterior, True)] + [_oriented(hole, False) for hole in outline.interiors]
    skeleton = straight_skeleton(rings)
    if not skeleton.ok or not skeleton.faces:
        return None
    profile = profile_for(shape, pitch_degrees)
    geometry = RoofGeometry()
    plane_faces = []
    for edge_id, nodes in skeleton.faces:
        origin, direction = skeleton.edges[edge_id]
        polygon = [np.array(skeleton.nodes[node], dtype=float) for node in nodes]
        for regime, piece in _split_by_regimes(polygon, profile):
            face = _make_face(piece, regime, profile, origin, direction)
            if face is not None:
                geometry.faces.append(face)
                plane_faces.append((edge_id, regime, face))
    if not geometry.faces:
        return None
    geometry.top = max(float(face.vertices[:, 2].max()) for face in geometry.faces)
    geometry.caps = _hip_lines(plane_faces)
    return geometry


def _make_face(piece, regime, profile, origin, direction):
    plateau = profile[regime][0] == 0.0
    rows = []
    for x, y, time in piece:
        height, length = _height_and_length(profile, time)
        if plateau:
            rows.append((x, y, height, x, y))
        else:
            along = (x - origin[0]) * direction[0] + (y - origin[1]) * direction[1]
            rows.append((x, y, height, along, -length))
    vertices = np.array(rows, dtype=np.float64)
    triangles = _triangulate(vertices)
    if not len(triangles):
        return None
    return RoofFace(KIND_PLATEAU if plateau else KIND_SLOPE, vertices, triangles)


def _plane(vertices):
    """z = a x + b y + c through the vertices (least squares)."""
    matrix = np.column_stack([vertices[:, 0], vertices[:, 1], np.ones(len(vertices))])
    solution, *_ = np.linalg.lstsq(matrix, vertices[:, 2], rcond=None)
    return solution


def _hip_lines(plane_faces):
    """Convex edges shared by two sloped faces of different original edges: hips and ridges, as 3D segments."""
    edges = {}
    for index, (edge_id, regime, face) in enumerate(plane_faces):
        if face.kind != KIND_SLOPE:
            continue
        count = len(face.vertices)
        for corner in range(count):
            a, b = face.vertices[corner], face.vertices[(corner + 1) % count]
            if min(a[2], b[2]) < 0.05 and max(a[2], b[2]) < 0.05:
                continue
            key = tuple(sorted([(round(a[0], 3), round(a[1], 3), round(a[2], 3)), (round(b[0], 3), round(b[1], 3), round(b[2], 3))]))
            edges.setdefault(key, []).append((index, a, b))
    caps = []
    for key, sides in edges.items():
        if len(sides) != 2 or plane_faces[sides[0][0]][0] == plane_faces[sides[1][0]][0]:
            continue
        a, b = sides[0][1], sides[0][2]
        if np.hypot(*(b[:2] - a[:2])) < 0.5 or min(a[2], b[2]) < 0.3 and max(a[2], b[2]) < 0.3:
            continue
        first, second = plane_faces[sides[0][0]][2], plane_faces[sides[1][0]][2]
        centre = second.vertices[:, :2].mean(axis=0)
        plane = _plane(first.vertices)
        actual = float(_plane(second.vertices) @ [centre[0], centre[1], 1.0])
        extended = float(plane @ [centre[0], centre[1], 1.0])
        if extended > actual + 1e-3:  # the first plane runs above the second beyond the line: a peak, not a valley
            caps.append((a[0], a[1], a[2], b[0], b[1], b[2]))
    return caps


# Typology classes that the game builds from the kit (WorldKitBuildings.cpp), with the style's eave overhang in metres.
KIT_OVERHANG = {0: 0.42, 1: 0.42, 7: 0.42, 9: 0.42, 2: 0.32, 4: 0.32, 5: 0.32, 6: 0.32, 10: 0.6, 15: 0.6}
PITCHED_SHAPES = {"gabled", "hipped", "half_hipped", "pyramidal", "mansard", "gambrel"}


def roof_for_building(footprint, class_id, roof_shape, pitch_degrees):
    """The skeleton roof for a building the game builds from the kit, or None when the game's own roof does: flat roofs,
    classes without a kit style, and gabled roofs over a plain rectangle (they need gable end walls)."""
    overhang = KIT_OVERHANG.get(class_id)
    if overhang is None or roof_shape not in PITCHED_SHAPES:
        return None
    simple_rectangle = len(footprint.interiors) == 0 and footprint.area / footprint.minimum_rotated_rectangle.area > 0.93
    if roof_shape == "gabled" and simple_rectangle:
        return None
    effective = "mansard" if roof_shape in ("mansard", "gambrel") else "hipped"
    return build_roof(footprint, effective, pitch_degrees, overhang)
