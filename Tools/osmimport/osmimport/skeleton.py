"""Straight skeleton of a polygon with holes, as the roof planes of a hipped roof.

Every edge of the outline moves inward at unit speed; the vertices ride the bisectors. The wavefront changes at two
kinds of events: an edge shrinks to nothing (edge event, which makes ridges and hips) or a reflex vertex runs into a
wavefront edge (split event, which makes valleys and separates two blocks that meet). The time of a point on the
skeleton is its distance from the outline, so a roof of pitch p has height tan(p) times that time.

The result is a list of faces, one or more per outline edge, each a polygon of (x, y, time) nodes lying in the plane of
its edge, plus the skeleton's arcs. The implementation recomputes every event each step, which is O(n^3) in the vertex
count and fine for building outlines (a few dozen vertices); callers give up on outlines with more than MAX_VERTICES.

Rings are oriented with the interior on the left: counter-clockwise outer ring, clockwise holes.
"""
import math
from dataclasses import dataclass, field

import numpy as np

MAX_VERTICES = 70
EPSILON = 1e-9
TIME_EPSILON = 1e-7
POSITION_TOLERANCE = 1e-6


@dataclass
class _Edge:
    """A line moving inward: points x with normal . x = offset + time."""
    original: int
    direction: np.ndarray
    normal: np.ndarray
    offset: float


@dataclass
class _Instance:
    """The part of an edge between two wavefront vertices, with the nodes its two end vertices have passed."""
    edge: _Edge
    tail_chain: list
    head_chain: list


@dataclass(eq=False)
class _Vertex:
    in_edge: _Edge
    out_edge: _Edge
    birth_node: int
    birth_time: float
    previous: "_Vertex" = None
    next: "_Vertex" = None
    out_instance: _Instance = None
    alive: bool = True
    reflex: bool = False
    base: np.ndarray = None
    velocity: np.ndarray = None

    def position(self, time):
        return self.base + self.velocity * time


@dataclass
class Skeleton:
    """nodes: (x, y, time); faces: (original edge index, [node indices]); arcs: (node a, node b, is_valley)."""
    nodes: list = field(default_factory=list)
    faces: list = field(default_factory=list)
    arcs: list = field(default_factory=list)
    ok: bool = True
    #: per original edge: (start point, unit direction)
    edges: dict = field(default_factory=dict)

    def add_node(self, x, y, time):
        self.nodes.append((float(x), float(y), float(time)))
        return len(self.nodes) - 1


def _unit(vector):
    length = float(np.hypot(vector[0], vector[1]))
    return vector / length if length > EPSILON else vector


def _motion(vertex):
    """Base point and velocity of a vertex: position(t) = base + velocity * t for the time its edges stay the same."""
    first, second = vertex.in_edge, vertex.out_edge
    matrix = np.array([first.normal, second.normal])
    determinant = float(np.linalg.det(matrix))
    if abs(determinant) < 1e-9:
        # Parallel edges: the vertex slides along their common normal from where it was born.
        velocity = first.normal.copy()
        return np.asarray(vertex.base, dtype=float) - velocity * vertex.birth_time if vertex.base is not None else None, velocity
    solution_base = np.linalg.solve(matrix, np.array([first.offset, second.offset]))
    velocity = np.linalg.solve(matrix, np.array([1.0, 1.0]))
    return solution_base, velocity


def _loop_vertices(start):
    result = [start]
    current = start.next
    while current is not start:
        result.append(current)
        current = current.next
        if len(result) > 10000:
            raise RuntimeError("broken wavefront loop")
    return result


def _make_vertex(skeleton, in_edge, out_edge, position, time, birth_node):
    vertex = _Vertex(in_edge, out_edge, birth_node, time)
    cross = in_edge.direction[0] * out_edge.direction[1] - in_edge.direction[1] * out_edge.direction[0]
    vertex.reflex = cross < -1e-9
    vertex.base = np.asarray(position, dtype=float)
    base, velocity = _motion(vertex)
    if base is None:
        base = np.asarray(position, dtype=float)
    vertex.base, vertex.velocity = base, velocity
    return vertex


def straight_skeleton(rings):
    """Skeleton of the polygon given as rings (list of (n, 2) arrays: outer counter-clockwise, then holes clockwise)."""
    skeleton = Skeleton()
    vertices = []
    total = sum(len(ring) for ring in rings)
    if total > MAX_VERTICES:
        skeleton.ok = False
        return skeleton
    edge_index = 0
    for ring in rings:
        ring = _remove_collinear(np.asarray(ring, dtype=float))
        count = len(ring)
        if count < 3:
            skeleton.ok = False
            return skeleton
        edges = []
        for index in range(count):
            a, b = ring[index], ring[(index + 1) % count]
            direction = _unit(b - a)
            normal = np.array([-direction[1], direction[0]])
            edges.append(_Edge(edge_index + index, direction, normal, float(normal @ a)))
            skeleton.edges[edge_index + index] = (a.copy(), direction)
        edge_index += count
        loop = []
        for index in range(count):
            node = skeleton.add_node(ring[index][0], ring[index][1], 0.0)
            loop.append(_make_vertex(skeleton, edges[index - 1], edges[index], ring[index], 0.0, node))
        for index, vertex in enumerate(loop):
            vertex.previous = loop[index - 1]
            vertex.next = loop[(index + 1) % count]
        for index, vertex in enumerate(loop):
            tail, head = ring[index], ring[(index + 1) % count]
            first_node = vertex.birth_node
            vertex.out_instance = _Instance(edges[index], [first_node], [loop[(index + 1) % count].birth_node])
        vertices.extend(loop)
    try:
        _run(skeleton, vertices)
    except (RuntimeError, np.linalg.LinAlgError, ValueError, IndexError):
        skeleton.ok = False
    return skeleton


def _remove_collinear(ring):
    """Drops repeated points and vertices where the outline doesn't turn."""
    points = [ring[0]]
    for point in ring[1:]:
        if np.hypot(*(point - points[-1])) > 1e-6:
            points.append(point)
    if len(points) > 1 and np.hypot(*(points[0] - points[-1])) < 1e-6:
        points.pop()
    changed = True
    while changed and len(points) > 3:
        changed = False
        for index in range(len(points)):
            before, here, after = points[index - 1], points[index], points[(index + 1) % len(points)]
            first, second = _unit(here - before), _unit(after - here)
            if abs(first[0] * second[1] - first[1] * second[0]) < 1e-7 and first @ second > 0:
                points.pop(index)
                changed = True
                break
    return np.array(points)


def _run(skeleton, vertices):
    alive = list(vertices)
    now = 0.0
    guard = 0
    while alive and guard < 4 * len(vertices) + 20:
        guard += 1
        event = _next_event(alive, now)
        if event is None:
            break
        time, kind, first, second = event
        now = max(now, time)
        if kind == "edge":
            _edge_event(skeleton, alive, first, second, now)
        else:
            _split_event(skeleton, alive, first, second, now)
        alive = [vertex for vertex in alive if vertex.alive]
        _finish_small_loops(skeleton, alive, now)
        alive = [vertex for vertex in alive if vertex.alive]
    if alive:
        raise RuntimeError("skeleton did not terminate")


def _next_event(alive, now):
    best = None
    for vertex in alive:
        successor = vertex.next
        time = _edge_collapse_time(vertex, successor, now)
        if time is not None and (best is None or time < best[0]):
            best = (time, "edge", vertex, successor)
        if vertex.reflex:
            hit = _split_candidate(vertex, alive, now)
            if hit is not None and (best is None or hit[0] < best[0]):
                best = (hit[0], "split", vertex, hit[1])
    return best


def _edge_collapse_time(vertex, successor, now):
    """When the edge from vertex to its successor has shrunk to nothing, or None."""
    direction = vertex.out_edge.direction
    start_gap = float(direction @ (successor.base - vertex.base))
    rate = float(direction @ (successor.velocity - vertex.velocity))
    if rate >= -1e-12:
        return None  # not shrinking
    time = -start_gap / rate
    return time if time >= now - TIME_EPSILON else None


def _split_candidate(vertex, alive, now):
    """Earliest wavefront edge the reflex vertex runs into: (time, tail vertex of that edge) or None."""
    best = None
    for tail in alive:
        edge = tail.out_edge
        if edge is vertex.in_edge or edge is vertex.out_edge or tail is vertex:
            continue
        head = tail.next
        denominator = float(edge.normal @ vertex.velocity) - 1.0
        if denominator > -1e-12:
            continue
        time = (edge.offset - float(edge.normal @ vertex.base)) / denominator
        if time < now - TIME_EPSILON:
            continue
        point = vertex.position(time)
        tail_point, head_point = tail.position(time), head.position(time)
        along = float(edge.direction @ (point - tail_point))
        length = float(edge.direction @ (head_point - tail_point))
        if length < -1e-9 or along < -1e-7 or along > length + 1e-7:
            continue
        if best is None or time < best[0]:
            best = (time, tail)
    return best


def _chain_polygon(instance, closing_head, closing_tail, middle=()):
    """Node list of the face fragment of an instance: its edge, then up the head chain, across, down the tail chain."""
    polygon = list(instance.tail_chain[:1]) + list(instance.head_chain) + list(closing_head) + list(middle) + list(closing_tail)
    polygon += list(reversed(instance.tail_chain[1:]))
    return polygon


def _add_fragment(skeleton, edge, polygon):
    cleaned = []
    for node in polygon:
        if not cleaned or not _same_point(skeleton, cleaned[-1], node):
            cleaned.append(node)
    while len(cleaned) > 1 and _same_point(skeleton, cleaned[0], cleaned[-1]):
        cleaned.pop()
    if len(cleaned) >= 3:
        skeleton.faces.append((edge.original, cleaned))


def _same_point(skeleton, first, second):
    a, b = skeleton.nodes[first], skeleton.nodes[second]
    return abs(a[0] - b[0]) < POSITION_TOLERANCE and abs(a[1] - b[1]) < POSITION_TOLERANCE and abs(a[2] - b[2]) < POSITION_TOLERANCE


def _kill(skeleton, vertex, node):
    vertex.alive = False
    skeleton.arcs.append((vertex.birth_node, node, vertex.reflex))


def _edge_event(skeleton, alive, tail, head, time):
    point = tail.position(time) * 0.5 + head.position(time) * 0.5
    node = skeleton.add_node(point[0], point[1], time)
    instance = tail.out_instance
    instance.tail_chain.append(node)
    instance.head_chain.append(node)
    _add_fragment(skeleton, instance.edge, _chain_polygon(instance, [], []))
    before, after = tail.previous, head.next
    if before is head:  # the whole loop is these two vertices: handled by the small loop rule
        before = after = None
    before_instance = tail.previous.out_instance
    before_instance.head_chain.append(node)
    after_instance = head.out_instance
    after_instance.tail_chain.append(node)
    merged = _make_vertex(skeleton, tail.in_edge, head.out_edge, point, time, node)
    _kill(skeleton, tail, node)
    _kill(skeleton, head, node)
    merged.out_instance = head.out_instance
    if tail.previous is head:
        return
    merged.previous, merged.next = tail.previous, head.next
    tail.previous.next = merged
    head.next.previous = merged
    alive.append(merged)


def _split_event(skeleton, alive, vertex, tail, time):
    head = tail.next
    point = vertex.position(time)
    node = skeleton.add_node(point[0], point[1], time)
    tail_point, head_point = tail.position(time), head.position(time)
    tail_node = skeleton.add_node(tail_point[0], tail_point[1], time)
    head_node = skeleton.add_node(head_point[0], head_point[1], time)
    hit_instance = tail.out_instance
    hit_instance.tail_chain.append(tail_node)
    hit_instance.head_chain.append(head_node)
    # Whatever the edge swept so far, closed along the wavefront through the hit point.
    polygon = list(hit_instance.tail_chain[:1]) + list(hit_instance.head_chain) + [node] + list(reversed(hit_instance.tail_chain[1:]))
    _add_fragment(skeleton, hit_instance.edge, polygon)
    vertex.previous.out_instance.head_chain.append(node)
    vertex.out_instance.tail_chain.append(node)
    first_part = _Instance(hit_instance.edge, [tail_node], [node])
    second_part = _Instance(hit_instance.edge, [node], [head_node])
    after_tail = _make_vertex(skeleton, hit_instance.edge, vertex.out_edge, point, time, node)
    before_head = _make_vertex(skeleton, vertex.in_edge, hit_instance.edge, point, time, node)
    after_tail.out_instance = vertex.out_instance
    before_head.out_instance = second_part
    tail.out_instance = first_part
    previous, following = vertex.previous, vertex.next
    tail.next, after_tail.previous = after_tail, tail
    after_tail.next, following.previous = following, after_tail
    previous.next, before_head.previous = before_head, previous
    before_head.next, head.previous = head, before_head
    _kill(skeleton, vertex, node)
    alive.extend([after_tail, before_head])


def _finish_small_loops(skeleton, alive, time):
    """A loop of two vertices has collapsed onto a line: close its two faces, a loop of one is nothing."""
    seen = set()
    for vertex in alive:
        if not vertex.alive or id(vertex) in seen:
            continue
        loop = _loop_vertices(vertex)
        seen.update(id(member) for member in loop)
        if len(loop) > 2:
            continue
        if len(loop) == 1:
            _kill(skeleton, vertex, skeleton.add_node(*vertex.position(time), time))
            continue
        first, second = loop
        nodes = {}
        for member in (first, second):
            position = member.position(time)
            nodes[id(member)] = skeleton.add_node(position[0], position[1], time)
        for member, other in ((first, second), (second, first)):
            instance = member.out_instance
            instance.tail_chain.append(nodes[id(member)])
            instance.head_chain.append(nodes[id(other)])
            _add_fragment(skeleton, instance.edge, _chain_polygon(instance, [], []))
        for member in (first, second):
            _kill(skeleton, member, nodes[id(member)])
