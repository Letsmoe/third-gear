"""Street furniture models for the streamed world, built with Blender (headless) and exported as FBX.

  blender -b --factory-startup -P Tools/furniture/make_models.py -- <out dir>

Conventions: metres, Z up, X is the direction of travel of the road user the object faces (its lenses, sign face and
luminaire arm point along -X for signals and signs, +X for lamps), origin at the foot of the pole. Blender's Y is
mirrored by the Unreal importer, which is harmless because the models are symmetric except for the cable door.
Materials are only slot names here; Scripts/create_furniture_assets.py gives them their looks.
"""
import math
import os
import sys

import bpy
import bmesh
from mathutils import Matrix, Vector

SEGMENTS = 24


def new_object(name, bm, materials):
    """Turns a bmesh into an object; each face's material_index refers to the materials list."""
    mesh = bpy.data.meshes.new(name)
    bm.to_mesh(mesh)
    bm.free()
    for material_name in materials:
        material = bpy.data.materials.get(material_name) or bpy.data.materials.new(material_name)
        mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


class Part:
    """Accumulates geometry in one bmesh with a slot per material name."""

    def __init__(self):
        self.bm = bmesh.new()
        self.slots = []

    def slot(self, material_name):
        if material_name not in self.slots:
            self.slots.append(material_name)
        return self.slots.index(material_name)

    def finish(self, name, smooth_angle=40.0):
        """Creates the object; edges sharper than smooth_angle stay hard so boxes are crisp and tubes round."""
        for face in self.bm.faces:
            face.smooth = True
        for edge in self.bm.edges:
            if len(edge.link_faces) == 2:
                a, b = edge.link_faces
                degenerate = a.normal.length < 1e-6 or b.normal.length < 1e-6
                edge.smooth = degenerate or a.normal.angle(b.normal) <= math.radians(smooth_angle)
        obj = new_object(name, self.bm, self.slots)
        self.bm = None
        return obj

    def add(self, geometry_fn, material, matrix=Matrix.Identity(4), **kwargs):
        before = set(self.bm.faces)
        geometry_fn(self.bm, **kwargs)
        index = self.slot(material)
        new_faces = [f for f in self.bm.faces if f not in before]
        verts = {v for f in new_faces for v in f.verts}
        bmesh.ops.transform(self.bm, matrix=matrix, verts=list(verts))
        for face in new_faces:
            face.material_index = index

    # --- primitives -------------------------------------------------------------------------------------------

    def cylinder(self, material, radius, length, center=(0, 0, 0), axis="Z", radius_top=None, segments=SEGMENTS, caps=True):
        radius_top = radius if radius_top is None else radius_top

        def build(bm):
            bmesh.ops.create_cone(bm, cap_ends=caps, cap_tris=False, segments=segments, radius1=radius,
                                  radius2=radius_top, depth=length)
        self.add(build, material, Matrix.Translation(center) @ _axis_matrix(axis))

    def box(self, material, size, center=(0, 0, 0), bevel=0.0):
        def build(bm):
            created = bmesh.ops.create_cube(bm, size=1.0)
            bmesh.ops.scale(bm, vec=size, verts=created["verts"])
        before = len(self.bm.faces)
        self.add(build, material, Matrix.Translation(center))
        if bevel > 0:
            self._bevel_new_faces(before, bevel)

    def _bevel_new_faces(self, before_count, width):
        faces = list(self.bm.faces)[before_count:]
        edges = list({e for f in faces for e in f.edges})
        bmesh.ops.bevel(self.bm, geom=edges, offset=width, segments=2, profile=0.6, affect="EDGES")

    def lathe(self, material, profile, segments=SEGMENTS, center=(0, 0, 0)):
        """Surface of revolution around Z from (radius, z) points."""
        rings = []
        for radius, z in profile:
            ring = []
            for i in range(segments):
                angle = 2 * math.pi * i / segments
                ring.append(self.bm.verts.new((radius * math.cos(angle) + center[0], radius * math.sin(angle) + center[1], z + center[2])))
            rings.append(ring)
        index = self.slot(material)
        for a, b in zip(rings[:-1], rings[1:]):
            for i in range(segments):
                j = (i + 1) % segments
                face = self.bm.faces.new((a[i], a[j], b[j], b[i]))
                face.material_index = index
        for ring, flip in ((rings[0], True), (rings[-1], False)):
            if _ring_radius(ring, center) < 1e-6:
                continue
            face = self.bm.faces.new(ring[::-1] if flip else ring)
            face.material_index = index
        bmesh.ops.recalc_face_normals(self.bm, faces=list(self.bm.faces))

    def tube_along(self, material, points, radius, radius_end=None, segments=12):
        """Round tube following a polyline (corners are not mitred, so use many points on curves)."""
        radius_end = radius if radius_end is None else radius_end
        rings = []
        for k, point in enumerate(points):
            tangent = (points[min(k + 1, len(points) - 1)] - points[max(k - 1, 0)]).normalized()
            side = tangent.cross(Vector((0, 0, 1)))
            if side.length < 1e-4:
                side = tangent.cross(Vector((0, 1, 0)))
            side.normalize()
            up = side.cross(tangent).normalized()
            r = radius + (radius_end - radius) * k / max(len(points) - 1, 1)
            ring = [self.bm.verts.new(point + (side * math.cos(2 * math.pi * i / segments) + up * math.sin(2 * math.pi * i / segments)) * r)
                    for i in range(segments)]
            rings.append(ring)
        index = self.slot(material)
        for a, b in zip(rings[:-1], rings[1:]):
            for i in range(segments):
                j = (i + 1) % segments
                self.bm.faces.new((a[i], b[i], b[j], a[j])).material_index = index
        for ring, flip in ((rings[0], True), (rings[-1], False)):
            self.bm.faces.new(ring[::-1] if flip else ring).material_index = index
        bmesh.ops.recalc_face_normals(self.bm, faces=list(self.bm.faces))


def _axis_matrix(axis):
    if axis == "X":
        return Matrix.Rotation(math.radians(90), 4, "Y")
    if axis == "Y":
        return Matrix.Rotation(math.radians(90), 4, "X")
    return Matrix.Identity(4)


def _ring_radius(ring, center):
    return max(math.hypot(v.co.x - center[0], v.co.y - center[1]) for v in ring)


def smoothstep(edge0, edge1, x):
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3 - 2 * t)


# --------------------------------------------------------------------------------------------- signal head

HEAD_CENTER_Z = 3.0
CELL_PITCH = 0.305
HOUSING_FRONT_X = -0.335
VISOR_LENGTH = 0.31
VISOR_RADIUS = 0.132


def visor(part, center_z, material):
    """Tunnel visor: a shell around the lens that is long at the top and cut away below, seen on German heads."""
    steps_angle, steps_length = 40, 6
    angle_start, angle_end = math.radians(-35), math.radians(215)
    grid = []
    for i in range(steps_angle + 1):
        angle = angle_start + (angle_end - angle_start) * i / steps_angle
        # length along X: full over the top half, shorter toward the open bottom
        height_factor = smoothstep(-0.55, 0.35, math.sin(angle))
        length = VISOR_LENGTH * (0.22 + 0.78 * height_factor)
        row = []
        for j in range(steps_length + 1):
            x = HOUSING_FRONT_X - length * j / steps_length
            row.append(part.bm.verts.new((x, VISOR_RADIUS * math.cos(angle), center_z + VISOR_RADIUS * math.sin(angle))))
        grid.append(row)
    index = part.slot(material)
    faces = []
    for i in range(steps_angle):
        for j in range(steps_length):
            faces.append(part.bm.faces.new((grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1])))
    for face in faces:
        face.material_index = index
    # thickness: second surface slightly inside, joined at the free edges
    inner = []
    for i in range(steps_angle + 1):
        row = []
        for j in range(steps_length + 1):
            v = grid[i][j].co
            row.append(part.bm.verts.new((v.x, v.y * 0.965, center_z + (v.z - center_z) * 0.965)))
        inner.append(row)
    for i in range(steps_angle):
        for j in range(steps_length):
            part.bm.faces.new((inner[i][j], inner[i][j + 1], inner[i + 1][j + 1], inner[i + 1][j])).material_index = index
    for j in range(steps_length):
        for edge_i in (0, steps_angle):
            part.bm.faces.new((grid[edge_i][j], grid[edge_i][j + 1], inner[edge_i][j + 1], inner[edge_i][j])).material_index = index
    for i in range(steps_angle):
        part.bm.faces.new((grid[i][steps_length], inner[i][steps_length], inner[i + 1][steps_length], grid[i + 1][steps_length])).material_index = index
    bmesh.ops.recalc_face_normals(part.bm, faces=list(part.bm.faces))


def backplate_with_holes(part, lens_zs):
    """Black plate in front of the housing with a round hole per lens, framed by a retroreflective border."""
    width, height, thickness = 0.50, 1.16, 0.012
    x = HOUSING_FRONT_X + 0.004
    center_z = HEAD_CENTER_Z
    hole_radius = VISOR_RADIUS + 0.004
    index_plate = part.slot("SignalBackplate")
    index_border = part.slot("SignalBackplateBorder")
    bm = part.bm
    # plate as a grid-less polygon with holes: build with bmesh.ops.triangulate on a face with hole loops
    outer = [bm.verts.new((x, -width / 2, center_z - height / 2)), bm.verts.new((x, width / 2, center_z - height / 2)),
             bm.verts.new((x, width / 2, center_z + height / 2)), bm.verts.new((x, -width / 2, center_z + height / 2))]
    hole_rings = []
    for z in lens_zs:
        hole_rings.append([bm.verts.new((x, hole_radius * math.cos(2 * math.pi * i / 32), z + hole_radius * math.sin(2 * math.pi * i / 32)))
                           for i in range(32)])
    edges = []
    for ring in [outer] + hole_rings:
        for k in range(len(ring)):
            edges.append(bm.edges.new((ring[k], ring[(k + 1) % len(ring)])))
    result = bmesh.ops.triangle_fill(bm, edges=edges, use_beauty=True)
    for face in result["geom"]:
        if isinstance(face, bmesh.types.BMFace):
            face.material_index = index_plate
            if face.normal.x > 0:
                face.normal_flip()
    # Border: a frame slightly in front of the plate, 18 mm wide.
    border = 0.026
    x_front = x - 0.002
    frame = [((-width / 2, -width / 2 + border), (center_z - height / 2, center_z + height / 2)),
             ((width / 2 - border, width / 2), (center_z - height / 2, center_z + height / 2)),
             ((-width / 2 + border, width / 2 - border), (center_z - height / 2, center_z - height / 2 + border)),
             ((-width / 2 + border, width / 2 - border), (center_z + height / 2 - border, center_z + height / 2))]
    for (y0, y1), (z0, z1) in frame:
        verts = [bm.verts.new((x_front, y0, z0)), bm.verts.new((x_front, y1, z0)), bm.verts.new((x_front, y1, z1)),
                 bm.verts.new((x_front, y0, z1))]
        face = bm.faces.new(verts[::-1])
        face.material_index = index_border
    # plate thickness: the same outline pushed back
    back = [bm.verts.new((x + thickness, v.co.y, v.co.z)) for v in outer]
    for k in range(4):
        a, b = outer[k], outer[(k + 1) % 4]
        face = bm.faces.new((a, b, back[(k + 1) % 4], back[k]))
        face.material_index = index_plate
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))


def build_signal_pole():
    part = Part()
    height = 3.6
    # tube with a slightly thicker foot, a base flange and a cap
    part.lathe("SignalPole", [(0.095, 0.0), (0.095, 0.02), (0.060, 0.03), (0.058, 0.60), (0.0545, 0.62), (0.0545, height - 0.03),
                              (0.056, height - 0.02), (0.056, height), (0.03, height + 0.03), (0.0, height + 0.04)])
    part.lathe("SignalPole", [(0.13, 0.0), (0.13, 0.014), (0.0, 0.014)])
    # cable access door on the +Y(Blender) side: shallow plate with two bolts
    part.box("SignalPole", (0.07, 0.012, 0.32), center=(0.0, 0.058, 0.62), bevel=0.003)
    for dz in (-0.12, 0.12):
        part.cylinder("SignalPole", 0.008, 0.012, center=(0.0, 0.066, 0.62 + dz), axis="Y", segments=8)
    # clamps for the head brackets (collars on the pole, arms reach to the housing)
    for dz in (-0.42, 0.42):
        z = HEAD_CENTER_Z + dz
        part.cylinder("SignalPole", 0.064, 0.05, center=(0, 0, z))
        part.box("SignalPole", (0.07, 0.035, 0.03), center=(-0.075, 0, z), bevel=0.004)
    return part.finish("SM_SignalPole")


def build_signal_head():
    part = Part()
    lens_zs = [HEAD_CENTER_Z + CELL_PITCH, HEAD_CENTER_Z, HEAD_CENTER_Z - CELL_PITCH]
    names = ["SignalLensRed", "SignalLensAmber", "SignalLensGreen"]
    depth = 0.235
    x_back = HOUSING_FRONT_X + depth
    for z, name in zip(lens_zs, names):
        part.box("SignalHousing", (depth, 0.285, CELL_PITCH - 0.004), center=(HOUSING_FRONT_X + depth / 2, 0, z), bevel=0.012)
        part.cylinder("SignalHousing", 0.110, 0.014, center=(HOUSING_FRONT_X + 0.004, 0, z), axis="X", segments=32)
        # lens cover: shallow dome, front at -X
        profile = [(0.0, 0.020), (0.040, 0.0185), (0.085, 0.012), (0.098, 0.004), (0.098, -0.020)]
        _lens_dome(part, name, z, profile)
        visor(part, z, "SignalHousing")
    # mounting lugs on the back where the brackets reach the housing
    for dz in (-0.42, 0.42):
        part.box("SignalHousing", (0.045, 0.06, 0.04), center=(x_back + 0.02, 0, HEAD_CENTER_Z + dz), bevel=0.006)
    backplate_with_holes(part, lens_zs)
    # head bracket tubes, from the housing back to the pole axis
    for dz in (-0.42, 0.42):
        part.cylinder("SignalPole", 0.013, x_back + 0.0 - (-0.0), center=(x_back / 2, 0, HEAD_CENTER_Z + dz), axis="X", segments=10)
    return part.finish("SM_SignalHead")


def _lens_dome(part, material, z, profile):
    """Lens around the X axis at height z: profile is (radius, depth along -X from the housing front)."""
    segments = 32
    rings = []
    for radius, depth in profile:
        ring = [part.bm.verts.new((HOUSING_FRONT_X + 0.010 - depth, radius * math.cos(2 * math.pi * i / segments),
                                   z + radius * math.sin(2 * math.pi * i / segments))) for i in range(segments)]
        rings.append(ring)
    index = part.slot(material)
    for a, b in zip(rings[:-1], rings[1:]):
        for i in range(segments):
            j = (i + 1) % segments
            part.bm.faces.new((a[i], a[j], b[j], b[i])).material_index = index
    part.bm.faces.new(rings[0][::-1]).material_index = index
    bmesh.ops.recalc_face_normals(part.bm, faces=list(part.bm.faces))


# ------------------------------------------------------------------------------------------------- signs

def build_sign_plate():
    """Unit square in the Y-Z plane, facing -X, UV 0..1 (u along +Y mirrored so the graphic reads correctly)."""
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    verts = [bm.verts.new((0, -0.5, -0.5)), bm.verts.new((0, 0.5, -0.5)), bm.verts.new((0, 0.5, 0.5)), bm.verts.new((0, -0.5, 0.5))]
    face = bm.faces.new(verts[::-1])
    # viewed from -X looking toward +X, UE's +Y is on the viewer's right
    coords = {id(vert): uv_pair for vert, uv_pair in zip(verts, [(1, 0), (0, 0), (0, 1), (1, 1)])}
    for loop in face.loops:
        loop[uv].uv = coords[id(loop.vert)]
    obj = new_object("SM_SignPlate", bm, ["SignFace"])
    return obj


def build_sign_pole(height):
    part = Part()
    radius = 0.038
    part.lathe("FurnitureMetalGalv", [(radius, 0.0), (radius, height - 0.004), (radius - 0.003, height), (0.0, height)], segments=20)
    # plastic cap
    part.lathe("FurnitureCap", [(radius + 0.002, height - 0.03), (radius + 0.002, height + 0.02), (radius - 0.01, height + 0.032), (0.0, height + 0.034)], segments=20)
    return part.finish(f"SM_SignPole_{int(round(height * 100))}")


def build_sign_clamp():
    """Band clamp: a strap around the pole and a bar to the sign plate behind which the plate sits at x = -0.062."""
    part = Part()
    part.cylinder("FurnitureMetalGalv", 0.0415, 0.036, center=(0, 0, 0), segments=20)
    part.box("FurnitureMetalGalv", (0.030, 0.050, 0.036), center=(-0.047, 0, 0), bevel=0.003)
    part.cylinder("FurnitureMetalGalv", 0.009, 0.018, center=(-0.047, 0, 0), axis="X", segments=8)
    return part.finish("SM_SignClamp")


# ---------------------------------------------------------------------------------------------- street lamp

LAMP_HEIGHT = 7.0
ARM_LENGTH = 1.7


def build_street_lamp():
    part = Part()
    # tapered pole with a thicker foot collar and an access door
    part.lathe("FurnitureMetalGalv", [(0.10, 0.0), (0.10, 0.012), (0.085, 0.02), (0.085, 0.65), (0.0805, 0.68), (0.052, LAMP_HEIGHT - 0.15),
                                      (0.048, LAMP_HEIGHT)], segments=20)
    part.lathe("FurnitureMetalGalv", [(0.16, 0.0), (0.16, 0.012), (0.0, 0.012)], segments=20)
    part.box("FurnitureMetalGalv", (0.012, 0.075, 0.42), center=(0.0, 0.0865, 0.42), bevel=0.003)
    # swept arm: rises, curves over and ends horizontal at the luminaire
    top = Vector((0, 0, LAMP_HEIGHT - 0.05))
    points = []
    for k in range(17):
        t = k / 16
        x = ARM_LENGTH * (1 - math.cos(t * math.pi / 2))
        z = LAMP_HEIGHT - 0.05 + 0.55 * math.sin(t * math.pi / 2) - 0.50 * t * t - 0.06 * t
        points.append(Vector((x, 0, z)))
    part.tube_along("FurnitureMetalGalv", points, 0.034, 0.028)
    end = points[-1]
    # luminaire: tapered shell with a flat lens underneath
    length, width, depth = 0.78, 0.30, 0.10
    center = Vector((end.x + 0.12, 0, end.z - 0.045))
    part.box("LampHousing", (length, width, depth), center=center + Vector((0, 0, depth * 0.1)), bevel=0.03)
    part.box("LampHousing", (0.30, 0.20, 0.05), center=center + Vector((-0.2, 0, depth * 0.5 + 0.02)), bevel=0.012)
    part.box("LampLens", (length - 0.12, width - 0.10, 0.006), center=center + Vector((0, 0, -depth * 0.4 - 0.002)), bevel=0.0)
    part.box("LampHousing", (0.10, 0.07, 0.06), center=(end.x - 0.08, 0, end.z + 0.0), bevel=0.01)
    return part.finish("SM_StreetLamp")


def export(obj, out_dir):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.fbx(filepath=os.path.join(out_dir, obj.name + ".fbx"), use_selection=True, object_types={"MESH"},
                             axis_forward="X", axis_up="Z", global_scale=1.0, apply_unit_scale=True,
                             apply_scale_options="FBX_SCALE_ALL", mesh_smooth_type="EDGE", use_mesh_modifiers=True)


PREVIEW_COLORS = {
    "SignalHousing": (0.045, 0.05, 0.05), "SignalPole": (0.55, 0.57, 0.58), "SignalBackplate": (0.01, 0.01, 0.01),
    "SignalBackplateBorder": (0.9, 0.9, 0.8), "SignalLensRed": (0.8, 0.05, 0.03), "SignalLensAmber": (0.9, 0.5, 0.05),
    "SignalLensGreen": (0.05, 0.6, 0.2), "FurnitureMetalGalv": (0.55, 0.57, 0.58), "FurnitureCap": (0.05, 0.05, 0.05),
    "LampHousing": (0.45, 0.47, 0.48), "LampLens": (0.9, 0.9, 0.8), "SignFace": (0.1, 0.3, 0.8),
}


def render_preview(obj, png_path, location, target, ortho_scale=1.6, resolution=(800, 1000)):
    """Workbench render of one object from a camera position, for checking the shapes without Unreal."""
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.render.resolution_x, scene.render.resolution_y = resolution
    for material in bpy.data.materials:
        material.diffuse_color = (*PREVIEW_COLORS.get(material.name, (0.5, 0.5, 0.5)), 1.0)
    camera_data = bpy.data.cameras.new("cam")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = ortho_scale
    camera = bpy.data.objects.new("cam", camera_data)
    bpy.context.collection.objects.link(camera)
    camera.location = location
    direction = Vector(target) - Vector(location)
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    scene.camera = camera
    for other in bpy.data.objects:
        other.hide_render = other is not obj and other is not camera
    scene.render.filepath = png_path
    bpy.ops.render.render(write_still=True)


def main():
    out_dir = sys.argv[sys.argv.index("--") + 1]
    preview_dir = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    os.makedirs(out_dir, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    objects = [build_signal_pole(), build_signal_head(), build_sign_plate(), build_sign_clamp(), build_street_lamp()]
    objects += [build_sign_pole(height) for height in (3.0, 3.5, 4.0)]
    for obj in objects:
        export(obj, out_dir)
        print("exported", obj.name, len(obj.data.polygons), "faces")
    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
        by_name = {obj.name: obj for obj in objects}
        render_preview(by_name["SM_SignalHead"], preview_dir + "/head_front.png", (-5, 0, 3.0), (0, 0, 3.0), 1.5)
        render_preview(by_name["SM_SignalHead"], preview_dir + "/head_side.png", (0, -5, 3.0), (0, 0, 3.0), 1.5)
        render_preview(by_name["SM_SignalHead"], preview_dir + "/head_persp.png", (-3, -3, 3.8), (-0.2, 0, 3.0), 1.5)
        render_preview(by_name["SM_StreetLamp"], preview_dir + "/lamp.png", (0, -12, 4.0), (0.8, 0, 4.0), 9.0, (1000, 900))
        render_preview(by_name["SM_SignalPole"], preview_dir + "/pole.png", (-5, -5, 2.0), (0, 0, 1.8), 4.2)

if __name__ == "__main__":
    main()
