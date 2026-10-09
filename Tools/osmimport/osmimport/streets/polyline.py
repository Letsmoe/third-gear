"""Polyline helpers on (n, 2) numpy arrays in the world frame (x east, y south): offsets, cuts, curves.

The frame is mirrored, so "right" here is the physical right of someone moving along the line.
"""
import numpy as np


def unit(vector):
    """The vector scaled to length one; (1, 0) for a zero vector."""
    norm = float(np.hypot(*vector))
    if norm > 1e-9:
        return vector / norm
    return np.array([1.0, 0.0])


def right_of(direction):
    """The unit vector to the right of a direction."""
    return np.array([-direction[1], direction[0]])


def arclength(xy):
    """Cumulative distance along the polyline at every point."""
    steps = np.hypot(*np.diff(xy, axis=0).T)
    return np.concatenate([[0.0], np.cumsum(steps)])


def length(xy) -> float:
    """Length of the polyline."""
    return float(arclength(xy)[-1])


def offset_polyline(xy, distance):
    """Moves a polyline sideways by distance to the right of its direction, with mitred corners.
    distance is one number, or one number per point."""
    if np.ndim(distance) == 0 and abs(distance) < 1e-6:
        return xy.copy()
    steps = np.diff(xy, axis=0)
    lengths = np.hypot(steps[:, 0], steps[:, 1])
    keep = lengths > 1e-9
    steps, lengths = steps[keep], lengths[keep]
    directions = steps / lengths[:, None]
    normals = np.stack([-directions[:, 1], directions[:, 0]], axis=1)
    count = len(xy)
    vertex_normals = np.zeros((count, 2))
    vertex_normals[0] = normals[0]
    vertex_normals[-1] = normals[-1]
    scale = np.ones(count)
    for i in range(1, count - 1):
        before = normals[min(i - 1, len(normals) - 1)]
        after = normals[min(i, len(normals) - 1)]
        mean = unit(before + after)
        vertex_normals[i] = mean
        scale[i] = 1.0 / max(float(np.dot(mean, before)), 0.5)
    return xy + vertex_normals * (distance * scale)[:, None]


def resample(xy, spacing):
    """Equally spaced points along a polyline, keeping its end points."""
    along = arclength(xy)
    total = along[-1]
    if total < 1e-6:
        return xy[:1].copy()
    count = max(int(round(total / spacing)), 1) + 1
    targets = np.linspace(0.0, total, count)
    return np.stack([np.interp(targets, along, xy[:, 0]), np.interp(targets, along, xy[:, 1])], axis=1)


def point_at(xy, s):
    """The point at arc length s (clamped to the line)."""
    along = arclength(xy)
    s = min(max(s, 0.0), along[-1])
    return np.array([np.interp(s, along, xy[:, 0]), np.interp(s, along, xy[:, 1])])


def direction_at(xy, s, reach=2.0):
    """The unit direction of the polyline around arc length s, measured over reach metres either side."""
    total = length(xy)
    ahead = point_at(xy, min(s + reach, total))
    behind = point_at(xy, max(s - reach, 0.0))
    return unit(ahead - behind)


def cut_polyline(xy, start_s, end_s):
    """The part of a polyline between two arc lengths."""
    along = arclength(xy)
    total = along[-1]
    start_s = min(max(start_s, 0.0), total)
    end_s = min(max(end_s, start_s), total)
    inside = (along > start_s + 1e-6) & (along < end_s - 1e-6)
    points = [np.array([np.interp(start_s, along, xy[:, 0]), np.interp(start_s, along, xy[:, 1])])]
    points.extend(xy[inside])
    points.append(np.array([np.interp(end_s, along, xy[:, 0]), np.interp(end_s, along, xy[:, 1])]))
    return np.array(points)


def project_on_polyline(xy, point):
    """(arc length, distance) of the closest point of a polyline to a point."""
    best = (0.0, 1e18)
    travelled = 0.0
    for a, b in zip(xy[:-1], xy[1:]):
        edge = b - a
        edge_length = float(np.hypot(*edge))
        if edge_length < 1e-9:
            continue
        t = min(max(float(np.dot(point - a, edge)) / (edge_length * edge_length), 0.0), 1.0)
        distance = float(np.hypot(*(a + edge * t - point)))
        if distance < best[1]:
            best = (travelled + t * edge_length, distance)
        travelled += edge_length
    return best


def bezier(p0, p1, p2, p3, spacing):
    """Cubic Bezier curve sampled about every spacing metres."""
    coarse = np.linspace(0.0, 1.0, 40)[:, None]
    points = ((1 - coarse) ** 3 * p0 + 3 * (1 - coarse) ** 2 * coarse * p1 + 3 * (1 - coarse) * coarse ** 2 * p2
              + coarse ** 3 * p3)
    return resample(points, spacing)


def smooth_curve(start, start_direction, end, end_direction, spacing):
    """A curve from start, leaving along start_direction, to end, arriving along end_direction: a cubic Bezier
    whose handles are a third of the gap long, so straight continuations stay straight."""
    reach = float(np.hypot(*(end - start))) / 3.0
    return bezier(start, start + start_direction * reach, end - end_direction * reach, end, spacing)


def distances_to(xy, points) -> np.ndarray:
    """The distance from each of the (m, 2) points to the nearest point of the polyline."""
    points = np.asarray(points, dtype=np.float64)
    starts, ends = xy[:-1], xy[1:]
    edges = ends - starts
    squared = np.maximum((edges * edges).sum(axis=1), 1e-12)
    relative = points[:, None, :] - starts[None, :, :]
    t = np.clip((relative * edges[None, :, :]).sum(axis=2) / squared[None, :], 0.0, 1.0)
    nearest = starts[None, :, :] + edges[None, :, :] * t[:, :, None]
    return np.hypot(*(points[:, None, :] - nearest).transpose(2, 0, 1)).min(axis=1)
