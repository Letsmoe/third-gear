"""Buildings extruded from OSM footprints, with simple gabled or flat roofs."""
import zlib

import numpy as np
import shapely

from .dem import HeightGrid
from .mesh import MeshBuilder, triangulate_polygon, walls

LEVEL_HEIGHT = 3.0
ROOF_PITCH_DEG = 40.0

# type -> (levels, roof) defaults; roof "gabled" only applies to roughly rectangular footprints
TYPE_DEFAULTS = {
    "house": (2, "gabled"), "detached": (2, "gabled"), "semidetached_house": (2, "gabled"), "terrace": (2, "gabled"),
    "bungalow": (1, "gabled"), "farm": (2, "gabled"), "farm_auxiliary": (1, "gabled"), "barn": (1, "gabled"),
    "allotment_house": (1, "gabled"), "cabin": (1, "gabled"), "hut": (1, "gabled"),
    "apartments": (4, "flat"), "residential": (3, "flat"), "dormitory": (4, "flat"),
    "garage": (1, "flat"), "garages": (1, "flat"), "carport": (1, "flat"), "shed": (1, "flat"),
    "roof": (1, "flat"), "greenhouse": (1, "flat"),
    "commercial": (3, "flat"), "retail": (1, "flat"), "office": (4, "flat"), "supermarket": (1, "flat"),
    "industrial": (2, "flat"), "warehouse": (2, "flat"), "manufacture": (2, "flat"),
    "school": (3, "flat"), "university": (4, "flat"), "hospital": (5, "flat"), "public": (3, "flat"),
    "civic": (3, "flat"), "church": (4, "gabled"), "kindergarten": (1, "flat"), "train_station": (2, "flat"),
}
LOW_TYPES = {"garage", "garages", "carport", "shed", "roof", "greenhouse"}


def _float(value):
    if value is None:
        return None
    try:
        return float(str(value).split(";")[0].replace(",", ".").replace("m", "").strip())
    except ValueError:
        return None


def _hash01(osm_id, salt=0):
    return (zlib.crc32(f"{osm_id}:{salt}".encode()) & 0xFFFF) / 0xFFFF


def building_params(osm_id, tags, footprint):
    btype = tags.get("building", "yes")
    area = footprint.area
    levels, roof = TYPE_DEFAULTS.get(btype, (None, None))
    if levels is None:  # building=yes or unknown: guess from size
        if area < 30:
            levels, roof, btype = 1, "flat", "shed"
        elif area < 220:
            levels, roof = 2, "gabled"
        elif area < 1500:
            levels, roof = 3, "flat"
        else:
            levels, roof = 2, "flat"
    tagged_levels = _float(tags.get("building:levels"))
    if tagged_levels:
        levels = max(1, tagged_levels)
    shape = tags.get("roof:shape")
    if shape in {"gabled", "hipped", "half-hipped", "gambrel", "mansard", "saltbox"}:
        roof = "gabled"
    elif shape in {"flat", "skillion"}:
        roof = "flat"
    eave = levels * LEVEL_HEIGHT
    if btype in LOW_TYPES:
        eave = 2.7 if btype != "greenhouse" else 3.2
    elif btype in {"retail", "supermarket", "industrial", "warehouse", "manufacture"}:
        eave = max(eave, 5.0 if levels <= 1 else levels * 4.0)
    height = _float(tags.get("height"))
    if height and 2 <= height <= 150:
        eave = height if roof == "flat" else height * 0.7
    return btype, eave, roof


def facade_style(osm_id, btype):
    r = _hash01(osm_id)
    if btype == "greenhouse":
        return "Facade_Glass", "Roof_Glass"
    if btype in {"industrial", "warehouse", "manufacture"}:
        return ("Facade_Metal" if r < 0.6 else "Facade_Concrete"), "Roof_Flat"
    if btype in {"retail", "supermarket", "commercial", "office", "school", "hospital", "university", "public"}:
        return ("Facade_Plaster" if r < 0.5 else "Facade_Brick" if r < 0.8 else "Facade_Concrete"), "Roof_Flat"
    # Hamburg housing: lots of red/brown Klinker
    return ("Facade_Brick" if r < 0.6 else "Facade_Plaster"), "Roof_Tiles"


def build(builder: MeshBuilder, buildings, ground: HeightGrid):
    """buildings: iterable of (osm_id, tags, footprint polygon) to build into this builder."""
    for osm_id, tags, geom in buildings:
        for footprint in getattr(geom, "geoms", [geom]):
            if not isinstance(footprint, shapely.Polygon) or footprint.area < 4.0:
                continue
            footprint = shapely.make_valid(footprint.simplify(0.15))
            if not isinstance(footprint, shapely.Polygon) or footprint.area < 4.0:
                continue
            _build_one(builder, osm_id, tags, footprint, ground)


def _build_one(builder, osm_id, tags, footprint, ground):
    btype, eave, roof = building_params(osm_id, tags, footprint)
    facade, roof_section = facade_style(osm_id, btype)
    ring = np.asarray(footprint.exterior.coords)
    base = float(np.min(ground.sample(ring[:, 0], ring[:, 1])))
    z_eave = base + eave
    tint = int(_hash01(osm_id, 1) * 255)
    color = (tint, int(_hash01(osm_id, 2) * 255), 0, 255)

    walls(builder, facade, ring, z_eave, base - 0.6, outward=True, v_base=base, color=color)
    for hole in footprint.interiors:
        walls(builder, facade, np.asarray(hole.coords), z_eave, base - 0.6, outward=False, v_base=base, color=color)

    rect = footprint.minimum_rotated_rectangle
    rectangular = isinstance(rect, shapely.Polygon) and rect.area > 0 and footprint.area / rect.area > 0.85 \
        and not footprint.interiors
    if roof == "gabled" and rectangular:
        _gable_roof(builder, rect, z_eave, base, facade, roof_section, color)
    else:
        verts, tris = triangulate_polygon(footprint)
        if len(tris):
            pos = np.column_stack([verts, np.full(len(verts), z_eave)])
            builder.add("Roof_Flat" if roof_section == "Roof_Tiles" else roof_section, pos, verts, tris)


def _gable_roof(builder, rect, z_eave, base, facade, roof_section, color):
    c = np.asarray(rect.exterior.coords)[:4]
    e0 = np.linalg.norm(c[1] - c[0])
    e1 = np.linalg.norm(c[2] - c[1])
    if e0 < e1:  # make c0->c1 the long side
        c = np.roll(c, -1, axis=0)
        e0, e1 = e1, e0
    span = e1
    rise = min(span / 2 * np.tan(np.radians(ROOF_PITCH_DEG)), 6.0)
    mid_a = (c[0] + c[3]) / 2  # ridge ends on the two short sides
    mid_b = (c[1] + c[2]) / 2
    ridge_z = z_eave + rise
    overhang = 0.4
    # extend eaves slightly beyond the walls
    centre = c.mean(axis=0)
    c_out = centre + (c - centre) * (1 + overhang / max(min(e0, e1) / 2, 1.0))
    ma_out = centre + (mid_a - centre) * (1 + overhang / max(e0 / 2, 1.0))
    mb_out = centre + (mid_b - centre) * (1 + overhang / max(e0 / 2, 1.0))
    z_ov = z_eave - overhang * np.tan(np.radians(ROOF_PITCH_DEG))
    planes = [  # two roof planes as quads: eave edge (c0->c1 / c2->c3) up to ridge
        [c_out[0], c_out[1], mb_out, ma_out],
        [c_out[2], c_out[3], ma_out, mb_out],
    ]
    for quad in planes:
        quad = np.asarray(quad)
        z = np.array([z_ov, z_ov, ridge_z, ridge_z])
        pos = np.column_stack([quad, z])
        # UVs: u along the eave, v up the slope (metres)
        along = quad[1] - quad[0]
        along /= max(np.linalg.norm(along), 1e-6)
        u = (quad - quad[0]) @ along
        slope_len = np.hypot(np.linalg.norm(quad[3] - quad[0] - along * ((quad[3] - quad[0]) @ along)), rise)
        v = np.array([0.0, 0.0, slope_len, slope_len])
        tris = np.array([[0, 1, 2], [0, 2, 3]])
        _orient_up(pos, tris)
        builder.add(roof_section, pos, np.column_stack([u, -v]), tris)
        # underside so the roof isn't see-through from below (overhang)
        builder.add(roof_section, pos, np.column_stack([u, -v]), tris[:, [0, 2, 1]])
    # gable triangles on the short sides (wall material)
    for a, b, m in ((c[3], c[0], mid_a), (c[1], c[2], mid_b)):
        pos = np.array([[*a, z_eave], [*b, z_eave], [*m, ridge_z]])
        width = np.linalg.norm(b - a)
        uv = np.array([[0.0, -(z_eave - base)], [width, -(z_eave - base)], [width / 2, -(ridge_z - base)]])
        tri = np.array([[0, 1, 2]])
        # face outward: normal must point away from the rectangle centre
        n = _face_normal(pos, tri[0])
        if np.dot(n[:2], m - centre) < 0:
            tri = tri[:, [0, 2, 1]]
        colors = np.tile(np.asarray(color, dtype=np.uint8), (3, 1))
        builder.add(facade, pos, uv, tri, colors)


def _face_normal(pos, tri):
    a, b, c = pos[tri[0]], pos[tri[1]], pos[tri[2]]
    return np.cross(c - a, b - a)  # same convention as mesh._vertex_normals


def _orient_up(pos, tris):
    for t in tris:
        if _face_normal(pos, t)[2] < 0:
            t[[1, 2]] = t[[2, 1]]
