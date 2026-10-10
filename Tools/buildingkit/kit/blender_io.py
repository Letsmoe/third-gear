"""Blender side of the kit: turns geom.Mesh objects into Blender meshes with UVs and material slots, exports GLB and
sets up preview rendering. Only import this inside Blender."""

import math
import os

import bmesh
import bpy
from mathutils import Vector

# Preview colours: one distinct hue per material slot so mistakes in slot assignment are visible.
PREVIEW_COLOURS = {
    "Brick": (0.62, 0.16, 0.10, 1.0),
    "Plaster": (0.90, 0.85, 0.70, 1.0),
    "Timber": (0.28, 0.15, 0.07, 1.0),
    "Frame": (0.97, 0.97, 0.95, 1.0),
    "Glass": (0.35, 0.65, 0.90, 0.45),
    "Sill": (0.72, 0.60, 0.45, 1.0),
    "RoofTile": (0.20, 0.27, 0.45, 1.0),
    "Thatch": (0.80, 0.68, 0.25, 1.0),
    "Metal": (0.30, 0.32, 0.34, 1.0),
    "Concrete": (0.42, 0.52, 0.44, 1.0),
    "Paint": (0.80, 0.81, 0.80, 1.0),
    "PaintRed": (0.65, 0.04, 0.03, 1.0),
    "PaintGreen": (0.12, 0.38, 0.18, 1.0),
    "Beacon": (1.0, 0.05, 0.02, 1.0),
    "Lattice": (0.30, 0.33, 0.31, 1.0),
    "Insulator": (0.55, 0.57, 0.60, 1.0),
    "PowderCoat": (0.10, 0.11, 0.12, 1.0),
    "AdPanel": (0.85, 0.80, 0.65, 1.0),
    "Timetable": (0.95, 0.95, 0.92, 1.0),
    "StopSignFace": (0.97, 0.97, 0.95, 1.0),
    "BinSticker": (0.97, 0.97, 0.97, 1.0),
    "Litter": (0.86, 0.82, 0.72, 1.0),
    "BinBag": (0.03, 0.03, 0.035, 1.0),
    "GreenhouseGlass": (0.80, 0.88, 0.88, 0.30),
    "Foil": (0.92, 0.93, 0.90, 0.55),
    "Aluminium": (0.72, 0.74, 0.76, 1.0),
    "BrandPaint": (0.10, 0.25, 0.60, 1.0),
    "BrandLogo": (0.90, 0.90, 0.95, 1.0),
    "PriceBoard": (0.10, 0.10, 0.12, 1.0),
    "PumpFace": (0.85, 0.86, 0.87, 1.0),
    "WashSign": (0.90, 0.90, 0.95, 1.0),
    "CanopyLight": (1.0, 0.98, 0.92, 1.0),
    "SafetyYellow": (0.95, 0.70, 0.02, 1.0),
    "WashBrush": (0.10, 0.35, 0.80, 1.0),
    "Asphalt": (0.12, 0.12, 0.13, 1.0),
    "Ground": (0.30, 0.38, 0.22, 1.0),
    "Water": (0.10, 0.25, 0.32, 1.0),
    "FacadeGlass": (0.28, 0.38, 0.48, 1.0),
    "RoofSequins": (0.88, 0.88, 0.86, 1.0),
}
TRANSPARENT = ("Glass", "GreenhouseGlass", "Foil")

# Back-lit faces: they glow in their own colour or image.
LIT = ("AdPanel", "BrandLogo", "PriceBoard", "PumpFace", "CanopyLight")

# Slot name -> image file; when set, the preview material shows the image through the piece's UVs (posters, signs).
PREVIEW_TEXTURES = {}

# Enercon's tower foot: five green bands from dark at the ground to pale, 3 m each (V of the metre UVs is the height).
GREEN_BANDS = [(0.03, 0.17, 0.07), (0.07, 0.27, 0.11), (0.15, 0.40, 0.17), (0.30, 0.53, 0.26), (0.50, 0.66, 0.40)]
GREEN_BAND_HEIGHT = 3.0


def reset_scene():
    """Empties the scene so every run starts clean."""
    bpy.ops.wm.read_factory_settings(use_empty=True)


def get_material(full_name):
    """Returns the named preview material, creating it on first use. A name like "BrandPaint@hopp" is the slot's
    preview for one variant: its colour and image come from the full name, everything else from the slot."""
    material = bpy.data.materials.get(full_name)
    if material is not None:
        return material
    material = bpy.data.materials.new(full_name)
    name = full_name.split("@")[0]
    colour = PREVIEW_COLOURS.get(full_name, PREVIEW_COLOURS[name])
    material.diffuse_color = colour
    material.use_nodes = True
    shader = material.node_tree.nodes["Principled BSDF"]
    shader.inputs["Base Color"].default_value = colour
    shader.inputs["Roughness"].default_value = 0.6
    if name in TRANSPARENT:
        shader.inputs["Alpha"].default_value = colour[3]
        shader.inputs["Roughness"].default_value = 0.05 if name != "Foil" else 0.4
        material.surface_render_method = "BLENDED"
    if name == "PaintGreen":
        add_band_ramp(material, shader)
    image = PREVIEW_TEXTURES.get(full_name, PREVIEW_TEXTURES.get(name))
    if image is not None and os.path.exists(image):
        add_preview_texture(material, shader, image, emissive=name in LIT)
    if name in LIT:
        shader.inputs["Emission Color"].default_value = colour
        shader.inputs["Emission Strength"].default_value = 0.6
    if name == "Beacon":
        shader.inputs["Emission Color"].default_value = colour
        shader.inputs["Emission Strength"].default_value = 4.0
    if name == "FacadeGlass":
        shader.inputs["Metallic"].default_value = 0.5
        shader.inputs["Roughness"].default_value = 0.15
    if name == "Metal":
        shader.inputs["Metallic"].default_value = 0.6
        shader.inputs["Roughness"].default_value = 0.4
    return material


def add_preview_texture(material, shader, path, emissive):
    """Feeds an image through the UVs into the base colour (and the emission, for back-lit posters)."""
    nodes = material.node_tree.nodes
    image_node = nodes.new("ShaderNodeTexImage")
    image_node.image = bpy.data.images.load(path, check_existing=True)
    material.node_tree.links.new(image_node.outputs["Color"], shader.inputs["Base Color"])
    if emissive:
        material.node_tree.links.new(image_node.outputs["Color"], shader.inputs["Emission Color"])


def add_band_ramp(material, shader):
    """Drives the base colour from the height (UV V) through a constant colour ramp of the green bands."""
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    texture_coordinate = nodes.new("ShaderNodeUVMap")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    scale = nodes.new("ShaderNodeMath")
    scale.operation = "DIVIDE"
    scale.inputs[1].default_value = GREEN_BAND_HEIGHT * len(GREEN_BANDS)
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.interpolation = "CONSTANT"
    elements = ramp.color_ramp.elements
    while len(elements) < len(GREEN_BANDS):
        elements.new(0.0)
    for index, colour in enumerate(GREEN_BANDS):
        elements[index].position = index / len(GREEN_BANDS)
        elements[index].color = (*colour, 1.0)
    links.new(texture_coordinate.outputs["UV"], separate.inputs[0])
    links.new(separate.outputs["Y"], scale.inputs[0])
    links.new(scale.outputs[0], ramp.inputs["Fac"])
    links.new(ramp.outputs["Color"], shader.inputs["Base Color"])


def build_mesh_data(name, kit_mesh):
    """Creates a Blender mesh from a geom.Mesh: welded vertices, planar metre UVs, one slot per used material."""
    vertices = []
    polygons = []
    for face in kit_mesh.faces:
        start = len(vertices)
        vertices.extend(face["points"])
        polygons.append(list(range(start, start + len(face["points"]))))
    mesh_data = bpy.data.meshes.new(name)
    mesh_data.from_pydata(vertices, [], polygons)
    slot_names = kit_mesh.materials_used()
    for slot_name in slot_names:
        mesh_data.materials.append(get_material(slot_name))
    uv_layer = mesh_data.uv_layers.new(name="UVMap")
    loop_index = 0
    for polygon, face in zip(mesh_data.polygons, kit_mesh.faces):
        polygon.material_index = slot_names.index(face["material"])
        polygon.use_smooth = face["smooth"]
        for uv in face["uvs"]:
            uv_layer.data[loop_index].uv = uv
            loop_index += 1
    mesh_data.update()
    weld_vertices(mesh_data)
    return mesh_data


def weld_vertices(mesh_data):
    """Merges coincident vertices so smooth-shaded faces share normals; UVs per loop are kept."""
    bm = bmesh.new()
    bm.from_mesh(mesh_data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
    bm.to_mesh(mesh_data)
    bm.free()
    mesh_data.update()


def add_piece_object(collection, name, kit_mesh, location=(0.0, 0.0, 0.0), rotation_z_degrees=0.0, mesh_data=None):
    """Adds an object for a piece; pass mesh_data to share geometry between instances."""
    if mesh_data is None:
        mesh_data = build_mesh_data(name, kit_mesh)
    obj = bpy.data.objects.new(name, mesh_data)
    obj.location = location
    obj.rotation_euler = (0.0, 0.0, math.radians(rotation_z_degrees))
    collection.objects.link(obj)
    return obj


def export_glb(obj, path):
    """Exports one object on its own as a GLB, with its origin at the object's location (which must be zero)."""
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=path, export_format="GLB", use_selection=True, export_apply=True, export_yup=True,
        export_materials="EXPORT", export_image_format="NONE", export_cameras=False, export_lights=False,
    )


# ------------------------------------------------------------------------------------------------------ rendering
def setup_world(sun_strength=3.4, neutral=False):
    """Sky background plus a sun, so the previews have soft ambient light and readable shadows."""
    scene = bpy.context.scene
    world = bpy.data.worlds.new("Sky")
    world.use_nodes = True
    nodes = world.node_tree.nodes
    links = world.node_tree.links
    nodes.clear()
    sky = nodes.new("ShaderNodeTexSky")
    for sky_type in ("MULTIPLE_SCATTERING", "SINGLE_SCATTERING", "NISHITA"):
        try:
            sky.sky_type = sky_type
            break
        except TypeError:
            continue
    sky.sun_elevation = math.radians(38)
    sky.sun_rotation = math.radians(210)
    sky.sun_disc = False
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Strength"].default_value = 0.22
    output = nodes.new("ShaderNodeOutputWorld")
    if neutral:
        background.inputs["Color"].default_value = (0.75, 0.76, 0.78, 1.0)
        background.inputs["Strength"].default_value = 0.55
    else:
        links.new(sky.outputs["Color"], background.inputs["Color"])
    links.new(background.outputs["Background"], output.inputs["Surface"])
    scene.world = world
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = sun_strength
    sun.data.angle = math.radians(2.0)
    sun.rotation_euler = (math.radians(52), math.radians(8), math.radians(-35))
    scene.collection.objects.link(sun)


def add_ground(size=60.0, z=0.0, colour=(0.30, 0.30, 0.29, 1.0)):
    """A large neutral ground plane."""
    mesh_data = bpy.data.meshes.new("Ground")
    half = size / 2.0
    mesh_data.from_pydata([(-half, -half, z), (half, -half, z), (half, half, z), (-half, half, z)], [], [(0, 1, 2, 3)])
    material = bpy.data.materials.new("GroundMaterial")
    material.use_nodes = True
    material.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = colour
    material.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.9
    mesh_data.materials.append(material)
    obj = bpy.data.objects.new("Ground", mesh_data)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def add_label(text, location, size=0.28, rotation=(math.radians(90), 0.0, 0.0)):
    """A flat text label standing upright, readable from -Y."""
    curve = bpy.data.curves.new("label_" + text, "FONT")
    curve.body = text
    curve.size = size
    curve.align_x = "CENTER"
    obj = bpy.data.objects.new("label_" + text, curve)
    obj.location = location
    obj.rotation_euler = rotation
    material = bpy.data.materials.get("LabelMaterial")
    if material is None:
        material = bpy.data.materials.new("LabelMaterial")
        material.use_nodes = True
        material.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.02, 0.02, 0.02, 1.0)
    curve.materials.append(material)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def add_camera(location, target, lens=35.0, orthographic_scale=None, clip_end=500.0):
    """Adds a camera looking at target and makes it the scene camera."""
    camera_data = bpy.data.cameras.new("Camera")
    camera_data.lens = lens
    camera_data.clip_end = clip_end
    if orthographic_scale is not None:
        camera_data.type = "ORTHO"
        camera_data.ortho_scale = orthographic_scale
    camera = bpy.data.objects.new("Camera", camera_data)
    camera.location = location
    direction = Vector(target) - Vector(location)
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(camera)
    bpy.context.scene.camera = camera
    return camera


def render_to(path, width, height, samples=48):
    """Renders the current scene with EEVEE to a PNG."""
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = path
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "None"
    scene.eevee.taa_render_samples = samples
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.render.render(write_still=True)
