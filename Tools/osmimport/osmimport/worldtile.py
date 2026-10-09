"""World data tiles (.tgtile): the compact per-tile data the game turns into meshes and instances at runtime.

Read by Plugins/MapRuntime (WorldTileData.cpp); keep both in sync and bump VERSION on any change.

File (little-endian):
  char[4] "TGT1", u32 version, f64 x0, f64 y0 (tile corner, world metres), f32 size_x, f32 size_y, u32 section_count
  per section: char[4] tag, u32 raw_size, u32 compressed_size, zlib data

Positions inside a tile are f32 metres relative to (x0, y0); heights are absolute metres above NHN.
World frame as in geo.py: x east, y south.

Sections:
  NAME  string table: u32 count, per string u16 length + utf-8. Records refer to material and model names by index.
  GRID  u32 nx, u32 ny, f32 cell, f32 base_z; u16 terrain[ny*nx], u16 road[ny*nx] (cm above base_z, row-major,
        row 0 at y0; terrain 0xFFFF marks a hole: no ground in any cell touching that point, used by horizon tiles
        around the region), u8 cover[ny*nx*3] (landcover blend weights: meadow, field, forest)
  SURF  u32 count; per polygon: u16 material, u8 height_mode, u8 pad, f32 params[6], u32 ring_count,
        per ring u32 n + f32 xy[n*2] (ring 0 is the outline, the rest are holes; no repeated end point)
  MARK  u32 count; per line: u16 material, u8 style, u8 pad, f32 width, f32 dash_on, f32 dash_off, f32 phase,
        u32 n, f32 xy[n*2]
  BLDG  u32 count; per building: u64 osm_id, u16 facade, u16 roof, u8 roof_shape, u8 tint, u8 variation, u8 pad,
        f32 base_z, f32 eave_height, f32 roof_rectangle[8] (gabled roofs: the footprint's minimum rotated rectangle,
        4 corners), u32 ring_count, rings as in SURF
  VEGE  u32 count; per plant: u16 model, u16 pad, f32 x, y, z, yaw, crown, height, trunk
  POIS  street furniture, u32 count; per object (36 bytes): u8 kind (POI_*), u8 flags, u16 variant, u16 variant2,
        u16 pad, f32 x, y (tile-relative), z (absolute, at the foot), yaw (degrees, see below), param0, param1,
        u32 link. Yaw is the direction of travel the object faces: a sign or signal head is read by drivers moving
        that way (its face looks back against it); for a lamp it is the direction from the pole to the road.
        LAMP: variant 0, param0 mast height m, param1 arm length m, flags 1 = lamp placed from OSM (else from lit=yes).
        SIGNAL_HEAD: link = approach id in traffic.json, param0 pole height m, flags 1 = pole on the left.
        SIGN: variant = name of the graphic (Zeichen_274-30), variant2 = name of an additional sign below it or 0xFFFF.
        Signal junctions, phases and the speed limit ways are in traffic.json next to world.json.
  LEAF  optional, settled fallen leaves (leaves.py) on the GRID vertex grid: u32 nx, u32 ny, u8 depth[ny*nx] in half
        centimetres at the peak of leaf fall (0..254; 255 is never written). Readers that don't know it skip it.
"""
import struct
import zlib

import numpy as np
import shapely

VERSION = 2

POI_LAMP = 0
POI_SIGNAL_HEAD = 1
POI_SIGN = 2
NO_VARIANT = 0xFFFF

# GRID terrain value of a hole
HOLE = 0xFFFF

# SURF height modes: how the runtime gets z for a vertex
HEIGHT_ROAD = 0        # road height grid + params[0]
HEIGHT_TERRAIN = 1     # terrain grid + params[0]
HEIGHT_CONSTANT = 2    # params[0]
HEIGHT_RAMP = 3        # params[0] at (params[2], params[3]) to params[1] at (params[4], params[5]), linear along the axis

ROOF_FLAT = 0
ROOF_GABLED = 1


class NameTable:
    """Interns strings (material and model names) to u16 indices."""

    def __init__(self):
        self.names = []
        self.index = {}

    def __call__(self, name):
        if name not in self.index:
            self.index[name] = len(self.names)
            self.names.append(name)
        return self.index[name]

    def pack(self):
        out = [struct.pack("<I", len(self.names))]
        for name in self.names:
            data = name.encode("utf-8")
            out.append(struct.pack("<H", len(data)) + data)
        return b"".join(out)


def _polygons(geom):
    """Non-empty polygons of any geometry."""
    if geom is None or geom.is_empty:
        return []
    if isinstance(geom, shapely.Polygon):
        return [geom]
    return [g for part in getattr(geom, "geoms", []) for g in _polygons(part)]


def _pack_rings(polygon, origin):
    """Ring count and rings of a polygon, relative to origin, without the repeated end point."""
    rings = [polygon.exterior, *polygon.interiors]
    out = [struct.pack("<I", len(rings))]
    for ring in rings:
        xy = np.asarray(ring.coords, dtype=np.float64)[:-1] - origin
        out.append(struct.pack("<I", len(xy)) + xy.astype("<f4").tobytes())
    return b"".join(out)


class TileWriter:
    """Collects one tile's records and writes the .tgtile file."""

    def __init__(self, bounds):
        self.x0, self.y0, self.x1, self.y1 = bounds
        self.origin = np.array([self.x0, self.y0])
        self.box = shapely.box(*bounds)
        self.names = NameTable()
        self.grid = None
        self.surfaces = []
        self.markings = []
        self.buildings = []
        self.plants = []
        self.pois = []
        self.leaf = None

    def set_grid(self, terrain, road, cover, cell, holes=None):
        """terrain, road: (ny, nx) heights in metres at the tile's vertex grid; cover: (ny, nx, 3) weights 0..1;
        holes: optional (ny, nx) bool mask of grid points without ground."""
        base = float(np.floor(min(terrain.min(), road.min())))
        to_cm = lambda z: np.clip(np.round((z - base) * 100.0), 0, HOLE - 1).astype("<u2")  # noqa: E731
        ny, nx = terrain.shape
        self.grid_nx, self.grid_ny = nx, ny
        terrain_cm = to_cm(terrain)
        if holes is not None:
            terrain_cm[holes] = HOLE
        self.grid = b"".join([
            struct.pack("<IIff", nx, ny, cell, base),
            terrain_cm.tobytes(), to_cm(road).tobytes(),
            np.clip(np.round(cover * 255.0), 0, 255).astype(np.uint8).tobytes(),
        ])

    def add_surface(self, material, geom, height_mode, params=()):
        """Adds the part of geom inside the tile as draped surface polygons."""
        params = (list(params) + [0.0] * 6)[:6]
        for polygon in _polygons(geom.intersection(self.box)):
            if polygon.area < 0.05:
                continue
            self.surfaces.append(struct.pack("<HBx6f", self.names(material), height_mode, *params)
                                 + _pack_rings(polygon, self.origin))

    def add_marking(self, material, style, line, width, dash):
        """Adds the part of a marking line inside the tile; the phase keeps dashes continuous across tiles."""
        on, off = dash if dash else (0.0, 0.0)
        clipped = line.intersection(self.box)
        for part in getattr(clipped, "geoms", [clipped]):
            if not isinstance(part, shapely.LineString) or part.length < 0.3:
                continue
            phase = float(line.project(shapely.Point(part.coords[0])))
            xy = np.asarray(part.coords, dtype=np.float64) - self.origin
            self.markings.append(struct.pack("<HBx4fI", self.names(material), style, width, on, off, phase, len(xy))
                                 + xy.astype("<f4").tobytes())

    def add_building(self, osm_id, facade, roof, roof_shape, tint, variation, base_z, eave_height, footprint,
                     roof_rectangle=None):
        """Adds a whole building (it belongs to the tile its representative point lies in).
        roof_rectangle: (4, 2) world corners of the gabled roof's rectangle, or None."""
        corners = np.zeros((4, 2))
        if roof_rectangle is not None:
            corners = np.asarray(roof_rectangle, dtype=np.float64)[:4] - self.origin
        self.buildings.append(struct.pack("<QHHBBBxff", osm_id & 0xFFFFFFFFFFFFFFFF, self.names(facade),
                                          self.names(roof), roof_shape, tint, variation, base_z, eave_height)
                              + corners.astype("<f4").tobytes() + _pack_rings(footprint, self.origin))

    def add_plant(self, model, x, y, z, yaw, crown, height, trunk):
        """Adds one tree or shrub at world position (x, y, z)."""
        self.plants.append(struct.pack("<H2x7f", self.names(model), x - self.x0, y - self.y0, z, yaw, crown,
                                       height, trunk))

    def add_poi(self, kind, x, y, z, yaw, variant=0, variant2=NO_VARIANT, param0=0.0, param1=0.0, link=0, flags=0):
        """Adds one piece of street furniture at world position (x, y), foot height z; see the POIS section."""
        self.pois.append(struct.pack("<BBHHxx6fI", kind, flags, variant, variant2,
                                     x - self.x0, y - self.y0, z, yaw, param0, param1, link))

    def set_leaf_field(self, depth):
        """depth: (ny, nx) u8 settled leaf depth on the grid's vertices."""
        ny, nx = depth.shape
        self.leaf = struct.pack("<II", nx, ny) + np.ascontiguousarray(depth, dtype=np.uint8).tobytes()

    def is_empty(self):
        return self.grid is None

    def write(self, path):
        sections = [(b"NAME", self.names.pack()), (b"GRID", self.grid)]
        for tag, records in ((b"SURF", self.surfaces), (b"MARK", self.markings), (b"BLDG", self.buildings),
                             (b"VEGE", self.plants), (b"POIS", self.pois)):
            sections.append((tag, struct.pack("<I", len(records)) + b"".join(records)))
        if self.leaf is not None:
            sections.append((b"LEAF", self.leaf))
        with open(path, "wb") as f:
            f.write(b"TGT1" + struct.pack("<Iddff", VERSION, self.x0, self.y0, self.x1 - self.x0, self.y1 - self.y0))
            f.write(struct.pack("<I", len(sections)))
            for tag, raw in sections:
                packed = zlib.compress(raw, 6)
                f.write(tag + struct.pack("<II", len(raw), len(packed)))
                f.write(packed)
