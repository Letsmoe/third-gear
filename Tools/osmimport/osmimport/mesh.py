"""Mesh building (in world metres) and the .dgmesh binary format read by the Unreal importer.

.dgmesh (little-endian):
  char[4] "DGM1", u32 section_count
  per section:
    u32 name_len, utf-8 name (material slot, e.g. "Road_Asphalt")
    u32 vertex_count, u32 index_count
    f32[vertex_count*3] positions (Unreal centimetres, relative to the tile pivot)
    f32[vertex_count*3] normals
    f32[vertex_count*2] uv0 (metres-based, material decides the scale)
    u8[vertex_count*4]  colors (RGBA)
    u32[index_count]    triangle indices (clockwise when seen from the front, Unreal convention)
"""
import struct
from collections import defaultdict

import mapbox_earcut as earcut
import numpy as np
import shapely


class MeshBuilder:
    def __init__(self):
        self.sections = defaultdict(lambda: {"pos": [], "uv": [], "col": [], "idx": [], "count": 0})

    def add(self, section, positions, uvs, indices, colors=None):
        """positions (n,3) world metres; uvs (n,2); indices (m,3) counter-clockwise seen from above/outside
        in a right-handed x-east/y-north sense — converted on write."""
        if len(indices) == 0:
            return
        s = self.sections[section]
        positions = np.asarray(positions, dtype=np.float64)
        s["pos"].append(positions)
        s["uv"].append(np.asarray(uvs, dtype=np.float32))
        if colors is None:
            colors = np.full((len(positions), 4), 255, dtype=np.uint8)
        s["col"].append(np.asarray(colors, dtype=np.uint8))
        s["idx"].append(np.asarray(indices, dtype=np.int64) + s["count"])
        s["count"] += len(positions)

    def is_empty(self):
        return not self.sections

    def write(self, path, pivot):
        """pivot: world metres (x, y, z) that becomes the actor location."""
        px, py, pz = pivot
        with open(path, "wb") as f:
            f.write(b"DGM1")
            f.write(struct.pack("<I", len(self.sections)))
            for name, s in sorted(self.sections.items()):
                pos = np.concatenate(s["pos"])
                uv = np.concatenate(s["uv"]).astype(np.float32)
                col = np.concatenate(s["col"]).astype(np.uint8)
                idx = np.concatenate(s["idx"])
                pos, uv, col, idx = _weld(pos, uv, col, idx)
                normals = _vertex_normals(pos, idx)
                # world metres -> Unreal cm relative to pivot. Our y already points south (Unreal +Y).
                ue = np.empty_like(pos, dtype=np.float32)
                ue[:, 0] = (pos[:, 0] - px) * 100.0
                ue[:, 1] = (pos[:, 1] - py) * 100.0
                ue[:, 2] = (pos[:, 2] - pz) * 100.0
                # Triangles are CCW in a y-north frame; with y south they are CW = Unreal front faces.
                name_bytes = name.encode("utf-8")
                f.write(struct.pack("<I", len(name_bytes)))
                f.write(name_bytes)
                f.write(struct.pack("<II", len(ue), idx.size))
                f.write(ue.astype("<f4").tobytes())
                f.write(normals.astype("<f4").tobytes())
                f.write(uv.astype("<f4").tobytes())
                f.write(col.tobytes())
                f.write(idx.astype("<u4").tobytes())


def _weld(pos, uv, col, idx):
    key = np.concatenate([np.round(pos * 1000.0), np.round(uv * 1000.0)], axis=1).astype(np.int64)
    _, first, inverse = np.unique(key, axis=0, return_index=True, return_inverse=True)
    idx = inverse.reshape(-1)[idx].reshape(-1, 3)
    # drop degenerate triangles
    ok = (idx[:, 0] != idx[:, 1]) & (idx[:, 1] != idx[:, 2]) & (idx[:, 0] != idx[:, 2])
    return pos[first], uv[first], col[first], idx[ok].reshape(-1)


def _vertex_normals(pos, idx):
    tris = idx.reshape(-1, 3)
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    # Our frame is x east, y SOUTH, z up (left-handed) and triangles are CCW seen from above in the y-north
    # sense, i.e. CW in this frame -> cross(c - a, b - a) points up.
    face = np.cross(c - a, b - a)
    normals = np.zeros_like(pos)
    for k in range(3):
        np.add.at(normals, tris[:, k], face)
    length = np.linalg.norm(normals, axis=1, keepdims=True)
    normals = np.where(length > 1e-12, normals / np.maximum(length, 1e-12), [0.0, 0.0, 1.0])
    # Unreal is left-handed too, but normals are just directions: same x/y/z.
    return normals.astype(np.float32)


# ---------------------------------------------------------------------------------------------------------------
# Surface meshing: polygons draped onto a height function, split on a regular grid so they follow the terrain.

def _ccw_up(tri_xy):
    """Orientation test in the y-SOUTH frame: we want triangles CCW when viewed with y pointing north,
    which is CW in raw (x, y_south) coordinates -> signed area < 0."""
    a, b, c = tri_xy
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) < 0


def triangulate_polygon(poly: shapely.Polygon):
    """Earcut a polygon with holes. Returns (vertices (n,2), triangles (m,3)) with consistent 'up' winding."""
    rings = [np.asarray(poly.exterior.coords)[:-1]] + [np.asarray(r.coords)[:-1] for r in poly.interiors]
    verts = np.concatenate(rings)
    ends = np.cumsum([len(r) for r in rings]).astype(np.uint32)
    tris = earcut.triangulate_float64(verts, ends).reshape(-1, 3)
    if len(tris) == 0:
        return verts, tris
    # make winding 'up' (see _ccw_up)
    a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    signed = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
    flip = signed > 0
    tris[flip] = tris[flip][:, [0, 2, 1]]
    return verts, tris


def drape(builder: MeshBuilder, section, geom, height_fn, bounds, cell=2.0, z_offset=0.0, uv_scale=1.0):
    """Clips geom to a grid of `cell`-sized squares within bounds and triangulates each piece, sampling z from
    height_fn(x, y). Full cells become two triangles; partial cells are earcut."""
    if geom.is_empty:
        return
    x0, y0, x1, y1 = bounds
    geom = geom.intersection(shapely.box(x0, y0, x1, y1))
    if geom.is_empty:
        return
    shapely.prepare(geom)
    gx = np.arange(x0, x1, cell)
    gy = np.arange(y0, y1, cell)
    cx, cy = np.meshgrid(gx, gy)
    cx, cy = cx.ravel(), cy.ravel()
    cells = shapely.box(cx, cy, np.minimum(cx + cell, x1), np.minimum(cy + cell, y1))
    touching = shapely.intersects(geom, cells)
    cells, cx, cy = cells[touching], cx[touching], cy[touching]
    full = shapely.contains(geom, cells)

    verts_all, tris_all = [], []
    count = 0
    # full cells: quads
    fx, fy = cx[full], cy[full]
    if len(fx):
        x2 = np.minimum(fx + cell, x1)
        y2 = np.minimum(fy + cell, y1)
        quad = np.stack([np.stack([fx, fy], 1), np.stack([x2, fy], 1), np.stack([x2, y2], 1), np.stack([fx, y2], 1)], 1)
        verts_all.append(quad.reshape(-1, 2))
        base = np.arange(len(fx))[:, None] * 4
        # (0,1,2),(0,2,3) is CCW in raw coords with y south -> flip to (0,2,1),(0,3,2) for 'up'
        tris = np.concatenate([base + [0, 2, 1], base + [0, 3, 2]])
        tris_all.append(tris)
        count = len(fx) * 4
    # partial cells
    pieces = shapely.intersection(cells[~full], geom)
    for piece in pieces:
        for poly in getattr(piece, "geoms", [piece]):
            if not isinstance(poly, shapely.Polygon) or poly.area < 1e-4:
                continue
            v, t = triangulate_polygon(poly)
            if len(t) == 0:
                continue
            verts_all.append(v)
            tris_all.append(t + count)
            count += len(v)
    if not verts_all:
        return
    xy = np.concatenate(verts_all)
    z = height_fn(xy[:, 0], xy[:, 1]) + z_offset
    positions = np.column_stack([xy, z])
    uvs = xy / uv_scale
    builder.add(section, positions, uvs, np.concatenate(tris_all))


def ribbon(builder: MeshBuilder, section, line: shapely.LineString, width, height_fn, z_offset, step=2.0,
           dash=None, uv_scale=1.0):
    """Flat strip of `width` centred on line, following the surface. dash=(on, off) metres for dashed lines."""
    length = line.length
    if length < 0.1:
        return
    if dash:
        on, off = dash
        starts = np.arange(0.0, length, on + off)
        spans = [(s, min(s + on, length)) for s in starts if min(s + on, length) - s > 0.3]
    else:
        spans = [(0.0, length)]
    for s0, s1 in spans:
        n = max(2, int(np.ceil((s1 - s0) / step)) + 1)
        d = np.linspace(s0, s1, n)
        pts = np.array([line.interpolate(v).coords[0] for v in d])
        tangent = np.gradient(pts, axis=0)
        tangent /= np.maximum(np.linalg.norm(tangent, axis=1, keepdims=True), 1e-9)
        normal = np.column_stack([-tangent[:, 1], tangent[:, 0]])
        left = pts + normal * width / 2
        right = pts - normal * width / 2
        xy = np.empty((2 * n, 2))
        xy[0::2] = left
        xy[1::2] = right
        z = height_fn(xy[:, 0], xy[:, 1]) + z_offset
        uv = np.column_stack([np.repeat(d, 2) / uv_scale, np.tile([0.0, 1.0], n)])
        i = np.arange(n - 1) * 2
        tris = np.concatenate([np.stack([i, i + 1, i + 2], 1), np.stack([i + 1, i + 3, i + 2], 1)])
        # ensure 'up' winding
        a, b, c = xy[tris[:, 0]], xy[tris[:, 1]], xy[tris[:, 2]]
        signed = (b[:, 0] - a[:, 0]) * (c[:, 1] - a[:, 1]) - (b[:, 1] - a[:, 1]) * (c[:, 0] - a[:, 0])
        flip = signed > 0
        tris[flip] = tris[flip][:, [0, 2, 1]]
        builder.add(section, np.column_stack([xy, z]), uv, tris)


def walls(builder: MeshBuilder, section, ring_xy, z_top, z_bottom, outward=True, uv_scale=1.0, v_base=0.0,
          color=None):
    """Vertical quads along a closed ring. z_top/z_bottom: arrays per ring vertex or scalars.
    outward=True: faces point away from the ring interior (building walls, kerb sides).
    UV: u = distance along the ring, v = -(z - v_base) (metres / uv_scale), so v_base marks the ground floor."""
    ring = np.asarray(ring_xy)
    z_top = np.asarray(z_top, dtype=np.float64)
    z_bottom = np.asarray(z_bottom, dtype=np.float64)
    if np.allclose(ring[0], ring[-1]):
        ring = ring[:-1]
        z_top = z_top[:-1] if z_top.ndim and len(z_top) == len(ring) + 1 else z_top
        z_bottom = z_bottom[:-1] if z_bottom.ndim and len(z_bottom) == len(ring) + 1 else z_bottom
    n = len(ring)
    if n < 2:
        return
    z_top = np.broadcast_to(z_top, (n,))
    z_bottom = np.broadcast_to(z_bottom, (n,))
    # orientation in y-north sense: signed area with y flipped
    flipped = ring * np.array([1.0, -1.0])
    area = 0.5 * np.sum(flipped[:, 0] * np.roll(flipped[:, 1], -1) - np.roll(flipped[:, 0], -1) * flipped[:, 1])
    ccw = area > 0
    seg_len = np.linalg.norm(np.roll(ring, -1, axis=0) - ring, axis=1)
    u = np.concatenate([[0.0], np.cumsum(seg_len)])
    positions, uvs, tris = [], [], []
    for i in range(n):
        j = (i + 1) % n
        base = len(positions)
        positions += [[*ring[i], z_bottom[i]], [*ring[j], z_bottom[j]], [*ring[j], z_top[j]], [*ring[i], z_top[i]]]
        zb_i, zb_j, zt_i, zt_j = z_bottom[i] - v_base, z_bottom[j] - v_base, z_top[i] - v_base, z_top[j] - v_base
        uvs += [[u[i] / uv_scale, zb_i / uv_scale], [u[i + 1] / uv_scale, zb_j / uv_scale],
                [u[i + 1] / uv_scale, zt_j / uv_scale], [u[i] / uv_scale, zt_i / uv_scale]]
        # For a CCW (y-north) ring, outward faces need the quad wound (0,1,2),(0,2,3) seen from outside.
        if ccw == outward:
            tris += [[base, base + 1, base + 2], [base, base + 2, base + 3]]
        else:
            tris += [[base, base + 2, base + 1], [base, base + 3, base + 2]]
    uvs = np.array(uvs)
    uvs[:, 1] = -uvs[:, 1]  # V grows downward in textures; keep "up" = decreasing V
    colors = None if color is None else np.tile(np.asarray(color, dtype=np.uint8), (len(positions), 1))
    builder.add(section, np.array(positions), uvs, np.array(tris), colors)
