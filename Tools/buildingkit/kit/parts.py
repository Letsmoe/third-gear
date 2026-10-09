"""Reusable building components. Every function returns a geom.Mesh in the bay frame of the README: the wall face on
Y = 0, the wall body toward +Y, X to the right, Z up, the origin at the bottom-left of the wall face."""

import math

from .geom import Mesh, segmental_arch_points, smooth_noise


# ---------------------------------------------------------------------------------------------------------- walls
def arch_points_at(opening_x, head_z, opening_width, rise, segments=14):
    """Intrados points (x, z) of a segmental arch sitting on the head line of an opening."""
    return [(opening_x + x, head_z + z) for x, z in segmental_arch_points(opening_width, rise, segments)]


def wall_bay(width, height, thickness, material, opening=None, bottom_closed=False):
    """A wall segment, optionally with a through opening that has real reveal faces.

    opening: dict with x, z, width, height (rectangular part) and rise (segmental arched head, 0 for flat).
    Open at the top, bottom and sides: neighbouring pieces abut and the next storey sits on top."""
    mesh = Mesh()
    skip_sides = "LRTD"
    if opening is None:
        mesh.box(0, width, 0, thickness, 0, height, material, skip=skip_sides)
        return mesh
    opening_x, opening_z = opening["x"], opening["z"]
    opening_width, opening_height = opening["width"], opening["height"]
    rise = opening.get("rise", 0.0)
    head_z = opening_z + opening_height
    if opening_z > 1e-9:
        mesh.box(0, width, 0, thickness, 0, opening_z, material, skip=skip_sides)
        mesh.add_face(
            [(opening_x, 0, opening_z), (opening_x + opening_width, 0, opening_z),
             (opening_x + opening_width, thickness, opening_z), (opening_x, thickness, opening_z)],
            material, desired_normal=(0, 0, 1),
        )
    mesh.box(0, opening_x, 0, thickness, opening_z, head_z, material, skip="LTD")
    mesh.box(opening_x + opening_width, width, 0, thickness, opening_z, head_z, material, skip="RTD")
    arc = arch_points_at(opening_x, head_z, opening_width, rise) if rise > 0 else [(opening_x, head_z), (opening_x + opening_width, head_z)]
    polygon = [(0, head_z)] + arc + [(width, head_z), (width, height), (0, height)]
    mesh.prism(polygon, "xz", 0.0, thickness, material, caps="LH", sides=False)
    for index in range(len(arc) - 1):
        start, end = arc[index], arc[index + 1]
        mid_x = (start[0] + end[0]) / 2.0
        inward = (opening_x + opening_width / 2.0 - mid_x, 0.0, -1.0 if rise <= 0 else (opening_z + opening_height * 0.5) - (start[1] + end[1]) / 2.0)
        if rise <= 0:
            inward = (0.0, 0.0, -1.0)
        mesh.add_face(
            [(start[0], 0, start[1]), (end[0], 0, end[1]), (end[0], thickness, end[1]), (start[0], thickness, start[1])],
            material, smooth=rise > 0, desired_normal=inward,
        )
    return mesh


def wall_corner(thickness, height, material):
    """Outer corner post whose front face is on Y = 0 for x in [0, thickness] and whose side face is on X = 0."""
    mesh = Mesh()
    mesh.box(0, thickness, 0, thickness, 0, height, material, skip="RTD")
    return mesh


# ------------------------------------------------------------------------------------------------ window and door
def _arch_ring_profile(opening_width, rise, ring_width, segments=14):
    """Inner and outer arc points of an arch band, both relative to the chord's left end at z = 0."""
    radius = (opening_width * opening_width / 4.0 + rise * rise) / (2.0 * rise)
    center_x = opening_width / 2.0
    center_z = rise - radius
    start_angle = math.atan2(-center_z, -center_x)
    end_angle = math.atan2(-center_z, opening_width - center_x)
    inner = []
    outer = []
    for index in range(segments + 1):
        angle = start_angle + (end_angle - start_angle) * index / segments
        inner.append((center_x + radius * math.cos(angle), center_z + radius * math.sin(angle)))
        outer.append((center_x + (radius + ring_width) * math.cos(angle), center_z + (radius + ring_width) * math.sin(angle)))
    return inner, outer


def window_unit(width, height, frame_depth, frame_width, sash_width, columns=2, transom_height=0.0, bars_across=1,
                bars_up=1, bar_width=0.02, arch_rise=0.0, frame_material="Frame"):
    """A window made of an outer frame, sashes with glazing bars and glass panes, filling an opening exactly.

    Local frame: x in [0, width], z in [0, height] (including the arched head), the front on y = 0 and the glass
    near the middle of frame_depth. columns sashes side by side; transom_height > 0 adds a fixed light at the top."""
    mesh = Mesh()
    rect_height = height - arch_rise
    glass_y = frame_depth * 0.45
    bottom_rail = frame_width * 1.4
    mesh.box(0, frame_width, 0, frame_depth, 0, rect_height, frame_material, skip="LTD")
    mesh.box(width - frame_width, width, 0, frame_depth, 0, rect_height, frame_material, skip="RTD")
    mesh.box(frame_width, width - frame_width, 0, frame_depth, 0, bottom_rail, frame_material, skip="D")
    inner_x0, inner_x1 = frame_width, width - frame_width
    inner_z0, inner_z1 = bottom_rail, rect_height - frame_width
    if arch_rise > 0:
        mesh.box(frame_width, width - frame_width, 0, frame_depth, inner_z1, rect_height, frame_material, skip="D")
        inner, outer = _arch_ring_profile(width, arch_rise, 0.0)
        inner_ring, _ = _arch_ring_profile(width - 2 * frame_width, max(arch_rise - frame_width, 0.015), 0.0)
        arc_outer = [(x, rect_height + z) for x, z in inner]
        arc_inner = [(x + frame_width, rect_height + z) for x, z in inner_ring]
        polygon = arc_outer + list(reversed(arc_inner))
        mesh.prism(polygon, "xz", 0.0, frame_depth, frame_material, caps="LH", sides=True, smooth_sides=False)
        glass_polygon = [(x, rect_height + z) for x, z in inner_ring]
        glass_polygon = [(x + frame_width, z) for x, z in glass_polygon]
        mesh.quad_double([(x, glass_y, z) for x, z in reversed(glass_polygon)], "Glass")
    else:
        mesh.box(frame_width, width - frame_width, 0, frame_depth, rect_height - frame_width, rect_height, frame_material, skip="D")
    sash_bottom = inner_z0
    sash_top = inner_z1
    if transom_height > 0:
        transom_bottom = inner_z1 - transom_height - frame_width * 0.8
        mesh.box(inner_x0, inner_x1, 0, frame_depth, transom_bottom, transom_bottom + frame_width * 0.8, frame_material, skip="LRD")
        mesh.add(_glazed_region(inner_x0, inner_x1, transom_bottom + frame_width * 0.8, inner_z1, glass_y, frame_depth,
                                 max(columns, 1), 1, bar_width, frame_material))
        sash_top = transom_bottom
    mullion_width = sash_width * 1.2
    sash_columns = max(columns, 1)
    sash_span = (inner_x1 - inner_x0 - (sash_columns - 1) * mullion_width) / sash_columns
    for column in range(sash_columns):
        sash_x0 = inner_x0 + column * (sash_span + mullion_width)
        sash_x1 = sash_x0 + sash_span
        if column > 0:
            mesh.box(sash_x0 - mullion_width, sash_x0, 0, frame_depth, sash_bottom, sash_top, frame_material, skip="TD")
        mesh.add(_sash(sash_x0, sash_x1, sash_bottom, sash_top, sash_width, glass_y, frame_depth, bars_across, bars_up,
                       bar_width, frame_material))
    return mesh


def _sash(x0, x1, z0, z1, sash_width, glass_y, frame_depth, bars_across, bars_up, bar_width, frame_material):
    """One casement sash: a ring of four members, glazing bars and a pane of glass."""
    mesh = Mesh()
    front, back = frame_depth * 0.08, frame_depth * 0.78
    mesh.box(x0, x0 + sash_width, front, back, z0, z1, frame_material, skip="D")
    mesh.box(x1 - sash_width, x1, front, back, z0, z1, frame_material, skip="D")
    mesh.box(x0 + sash_width, x1 - sash_width, front, back, z0, z0 + sash_width * 1.3, frame_material, skip="LRD")
    mesh.box(x0 + sash_width, x1 - sash_width, front, back, z1 - sash_width, z1, frame_material, skip="LRT")
    mesh.add(_glazed_region(x0 + sash_width, x1 - sash_width, z0 + sash_width * 1.3, z1 - sash_width, glass_y, frame_depth,
                            bars_across, bars_up, bar_width, frame_material))
    return mesh


def _glazed_region(x0, x1, z0, z1, glass_y, frame_depth, bars_across, bars_up, bar_width, frame_material):
    """A pane of glass split by thin glazing bars into bars_across by bars_up lights."""
    mesh = Mesh()
    mesh.quad_double([(x0, glass_y, z0), (x1, glass_y, z0), (x1, glass_y, z1), (x0, glass_y, z1)], "Glass")
    bar_front, bar_back = glass_y - frame_depth * 0.12, glass_y + frame_depth * 0.12
    for index in range(1, bars_across):
        bar_x = x0 + (x1 - x0) * index / bars_across
        mesh.box(bar_x - bar_width / 2, bar_x + bar_width / 2, bar_front, bar_back, z0, z1, frame_material, skip="")
    for index in range(1, bars_up):
        bar_z = z0 + (z1 - z0) * index / bars_up
        mesh.box(x0, x1, bar_front, bar_back, bar_z - bar_width / 2, bar_z + bar_width / 2, frame_material, skip="LR")
    return mesh


def door_unit(width, height, frame_depth, frame_width, leaf_panels=(2, 3), arch_rise=0.0, fanlight_height=0.0,
              glazed=False, frame_material="Frame", leaf_inset=0.03, aluminium=False):
    """A door with frame, panelled or glazed leaf, and an optional fanlight above it.

    Local frame like window_unit: x in [0, width], z in [0, height], front on y = 0, the floor on z = 0."""
    mesh = Mesh()
    rect_height = height - arch_rise
    leaf_top = rect_height - frame_width - fanlight_height
    mesh.box(0, frame_width, 0, frame_depth, 0, rect_height, frame_material, skip="LTD")
    mesh.box(width - frame_width, width, 0, frame_depth, 0, rect_height, frame_material, skip="RTD")
    mesh.box(frame_width, width - frame_width, 0, frame_depth, rect_height - frame_width, rect_height, frame_material, skip="D")
    leaf_x0, leaf_x1 = frame_width, width - frame_width
    leaf_front, leaf_back = leaf_inset, leaf_inset + 0.045
    if glazed:
        member = frame_width * 0.9
        mesh.box(leaf_x0, leaf_x0 + member, leaf_front, leaf_back, 0, leaf_top, frame_material, skip="D")
        mesh.box(leaf_x1 - member, leaf_x1, leaf_front, leaf_back, 0, leaf_top, frame_material, skip="D")
        mesh.box(leaf_x0 + member, leaf_x1 - member, leaf_front, leaf_back, 0, member * 1.8, frame_material, skip="LRD")
        mesh.box(leaf_x0 + member, leaf_x1 - member, leaf_front, leaf_back, leaf_top - member, leaf_top, frame_material, skip="LRT")
        glass_y = (leaf_front + leaf_back) / 2.0
        mesh.quad_double([(leaf_x0 + member, glass_y, member * 1.8), (leaf_x1 - member, glass_y, member * 1.8),
                          (leaf_x1 - member, glass_y, leaf_top - member), (leaf_x0 + member, glass_y, leaf_top - member)], "Glass")
    else:
        mesh.box(leaf_x0, leaf_x1, leaf_front, leaf_back, 0, leaf_top, frame_material, skip="LRTD")
        panels_across, panels_up = leaf_panels
        margin = 0.1
        gap = 0.07
        panel_width = (leaf_x1 - leaf_x0 - 2 * margin - (panels_across - 1) * gap) / panels_across
        panel_height = (leaf_top - 2 * margin - (panels_up - 1) * gap) / panels_up
        for column in range(panels_across):
            for row in range(panels_up):
                panel_x = leaf_x0 + margin + column * (panel_width + gap)
                panel_z = margin + row * (panel_height + gap)
                mesh.box(panel_x, panel_x + panel_width, leaf_front - 0.012, leaf_front, panel_z, panel_z + panel_height, frame_material, skip="D")
    if fanlight_height > 0:
        fan_bottom = leaf_top
        mesh.box(leaf_x0, leaf_x1, 0, frame_depth, fan_bottom, fan_bottom + frame_width * 0.8, frame_material, skip="LRD")
        glass_y = frame_depth * 0.45
        top = rect_height - frame_width
        mesh.quad_double([(leaf_x0, glass_y, fan_bottom + frame_width * 0.8), (leaf_x1, glass_y, fan_bottom + frame_width * 0.8),
                          (leaf_x1, glass_y, top), (leaf_x0, glass_y, top)], "Glass")
        if width > 0.9:
            middle = width / 2.0
            mesh.box(middle - 0.01, middle + 0.01, glass_y - 0.02, glass_y + 0.02, fan_bottom + frame_width * 0.8, top, frame_material, skip="")
    if arch_rise > 0:
        inner, _ = _arch_ring_profile(width, arch_rise, 0.0)
        inner_small, _ = _arch_ring_profile(width - 2 * frame_width, max(arch_rise - frame_width, 0.015), 0.0)
        polygon = [(x, rect_height + z) for x, z in inner] + [(x + frame_width, rect_height + z) for x, z in reversed(inner_small)]
        mesh.prism(polygon, "xz", 0.0, frame_depth, frame_material, caps="LH", sides=True)
        glass_polygon = [(x + frame_width, rect_height + z) for x, z in inner_small]
        mesh.quad_double([(x, frame_depth * 0.45, z) for x, z in reversed(glass_polygon)], "Glass")
    return mesh


def sill_piece(opening_x, opening_z, opening_width, projection, overhang, reveal_depth, material="Sill", thickness=0.07):
    """A stone or concrete sill under an opening: slopes outward, sticks out of the wall face by projection."""
    mesh = Mesh()
    x0, x1 = opening_x - overhang, opening_x + opening_width + overhang
    top_inner = opening_z + 0.003
    top_outer = opening_z - 0.025
    bottom = opening_z - thickness
    front = -projection
    back = reveal_depth
    # Profile in (y, z), counter-clockwise seen from +X: bottom front, bottom back, top back, top front.
    polygon = [(front, bottom), (back, bottom), (back, top_inner), (0.0, top_inner), (front, top_outer)]
    mesh.prism(polygon, "yz", x0, x1, material, caps="LH")
    return mesh


def arch_relief(opening_x, head_z, opening_width, rise, ring_width, relief, ring_material="Sill", stone_material="Sill"):
    """A raised segmental arch band over a window head with a keystone and skewback blocks."""
    mesh = Mesh()
    inner, outer = _arch_ring_profile(opening_width, rise, ring_width)
    inner = [(opening_x + x, head_z + z) for x, z in inner]
    outer = [(opening_x + x, head_z + z) for x, z in outer]
    polygon = inner + list(reversed(outer))
    mesh.prism(polygon, "xz", -relief, 0.0, ring_material, caps="L", sides=True)
    middle = len(inner) // 2
    keystone_half = max(0.05, opening_width * 0.06)
    key_inner = inner[middle]
    key_outer = (key_inner[0], key_inner[1] + ring_width + 0.045)
    mesh.box(key_inner[0] - keystone_half, key_inner[0] + keystone_half, -relief - 0.02, 0.0, key_inner[1] - 0.005, key_outer[1], stone_material)
    skew_width = ring_width * 1.1
    skew_height = ring_width * 0.9
    for side_x in (inner[0][0] - skew_width * 0.55, inner[-1][0] - skew_width * 0.45):
        mesh.box(side_x, side_x + skew_width, -relief - 0.01, 0.0, head_z - skew_height * 0.9, head_z + skew_height * 0.1, stone_material)
    return mesh


# ---------------------------------------------------------------------------------------------------- horizontal bands
def band_pieces(prefix, profile, material, module, thickness, half=True):
    """Straight, half and corner versions of a profile that runs along the facade (cornice, string course, plinth).

    The straight piece covers one module along +X. The left corner piece covers the corner post (x in [0, thickness],
    y in [0, thickness]) plus the projection around it and returns along the side wall; the right corner is its
    mirror image on [0, thickness]. All use mitred joints so the band is continuous around the building."""
    pieces = {}
    straight = Mesh()
    straight.sweep(profile, [(0.0, 0.0), (module, 0.0)], material, closed_ends=False)
    pieces[prefix + "_M"] = straight
    if half:
        half_piece = Mesh()
        half_piece.sweep(profile, [(0.0, 0.0), (module / 2.0, 0.0)], material, closed_ends=False)
        pieces[prefix + "_Half"] = half_piece
    corner = Mesh()
    corner.sweep(profile, [(0.0, thickness), (0.0, 0.0), (thickness, 0.0)], material, closed_ends=False)
    pieces[prefix + "_CornerL"] = corner
    pieces[prefix + "_CornerR"] = corner.mirrored_x(thickness)
    return pieces


def gutter_profile(radius, hang):
    """Outer and inner profile lines of a half-round gutter whose rim is at z = 0 and centre at d = hang + radius."""
    outer = []
    inner = []
    segments = 10
    for index in range(segments + 1):
        angle = math.pi * index / segments
        outer.append((hang + radius - radius * math.cos(angle), -radius * math.sin(angle)))
    inner_radius = radius - 0.004
    for index in range(segments + 1):
        angle = math.pi * (segments - index) / segments
        inner.append((hang + radius - inner_radius * math.cos(angle), -inner_radius * math.sin(angle)))
    return outer, inner


def gutter_pieces(prefix, module, thickness, z_rim, radius=0.065, hang=0.04):
    """Half-round gutter: straight run and mitred outer corners. z_rim is the height of the gutter's rim."""
    outer, inner = gutter_profile(radius, hang)
    pieces = {}
    for suffix, path in (
        ("_M", [(0.0, 0.0), (module, 0.0)]),
        ("_CornerL", [(0.0, thickness), (0.0, 0.0), (thickness, 0.0)]),
    ):
        mesh = Mesh()
        mesh.sweep(outer, path, "Metal", closed_ends=True, smooth=True, base_z=z_rim)
        mesh.sweep(list(reversed(inner)), path, "Metal", closed_ends=False, smooth=True, base_z=z_rim)
        pieces[prefix + suffix] = mesh
    pieces[prefix + "_CornerR"] = pieces[prefix + "_CornerL"].mirrored_x(thickness)
    return pieces


def downpipe_piece(height, radius=0.04, standoff=0.05, bracket_spacing=1.0):
    """Vertical round downpipe centred at x = 0, hung in front of the wall with brackets; z in [0, height]."""
    mesh = Mesh()
    center_y = -(standoff + radius)
    mesh.cylinder(0.0, center_y, 0.0, height, radius, "Metal", segments=10, caps="LH")
    bracket_height = 0.03
    z = bracket_spacing * 0.5
    while z < height:
        mesh.box(-0.012, 0.012, -standoff - radius, 0.0, z, z + bracket_height, "Metal")
        mesh.cylinder(0.0, center_y, z, z + bracket_height, radius + 0.008, "Metal", segments=10, caps="")
        z += bracket_spacing
    return mesh


# -------------------------------------------------------------------------------------------------------- roofs
def tile_plane(width, slope_length, thickness=0.06, course_length=0.33, tile_period=0.22, wave=0.016, step=0.028):
    """A pantile roof section in its flat frame: x along the eaves, y up the slope, the visible surface near z = 0.

    The caller tilts it by the roof pitch (rotated_x); the origin is the lower-left corner of the surface."""
    mesh = Mesh()
    periods = max(1, int(round(width / tile_period)))
    tile_period = width / periods
    column_count = periods * 5
    xs = [width * index / column_count for index in range(column_count + 1)]
    ys = []
    courses = max(1, int(round(slope_length / course_length)))
    course = slope_length / courses
    for course_index in range(courses):
        base = course_index * course
        for fraction in (0.0, 0.3, 0.6, 0.93):
            ys.append(base + course * fraction)
        ys.append(base + course * 0.975)
    ys.append(slope_length)

    def height(x, y):
        local = (y % course) / course if y < slope_length - 1e-9 else 1.0
        ramp = step * (1.0 - min(local / 0.95, 1.0))
        return wave * math.sin(2 * math.pi * x / tile_period) + ramp

    mesh.heightfield_nonuniform(xs, ys, height, "RoofTile", skirt_depth=thickness)
    return mesh


def ridge_cap_tile(width, pitch_degrees, cap_half_width=0.2, thickness=0.03):
    """Inverted-V ridge cap over two tile slopes meeting at the apex line y = 0, z = 0; runs along +X."""
    mesh = Mesh()
    drop = cap_half_width * math.tan(math.radians(pitch_degrees)) + 0.02
    apex = 0.045
    polygon = [
        (-cap_half_width, -drop), (-cap_half_width, -drop - thickness * 0.6),
        (0.0, apex - thickness), (cap_half_width, -drop - thickness * 0.6),
        (cap_half_width, -drop), (0.0, apex),
    ]
    mesh.prism(polygon, "yz", 0.0, width, "RoofTile", caps="LH")
    return mesh


def thatch_plane(width, slope_length, thickness, seed=1, rough=1.0):
    """A thatch roof section in its flat frame: a thick lumpy slab with straw streaks running down the slope."""
    mesh = Mesh()
    columns = max(6, int(round(width / 0.05)))
    rows = max(4, int(round(slope_length / 0.07)))

    def height(x, y):
        border = min(x, width - x, y, slope_length - y)
        fade = min(1.0, border / 0.12)
        fade = fade * fade * (3 - 2 * fade)
        streaks = 0.016 * smooth_noise(x * 22.0, y * 2.5, seed, 1.0)
        tufts = 0.03 * smooth_noise(x * 3.2, y * 1.6, seed + 7, 1.0)
        bands = 0.02 * math.sin(y * math.pi * 2.0 / 0.5)
        return fade * rough * (streaks + tufts) + bands

    mesh.heightfield(0.0, width, 0.0, slope_length, columns, rows, height, "Thatch", skirt_depth=thickness)
    return mesh


def thatch_ridge(width, pitch_degrees, half_width=0.34, height=0.22, seed=3):
    """A rolled thatch ridge: a rounded saddle with a scalloped edge, apex line at y = 0, z = 0."""
    mesh = Mesh()
    steps_along = max(6, int(width / 0.08))
    segments_across = 10
    previous_ring = None
    for step_index in range(steps_along + 1):
        x = width * step_index / steps_along
        wobble = 1.0 + 0.1 * smooth_noise(x, 0.0, seed, 5.0)
        ring = []
        for index in range(segments_across + 1):
            angle = math.pi * index / segments_across
            across = -half_width * math.cos(angle)
            drop = abs(across) * math.tan(math.radians(pitch_degrees)) * 0.55
            ring.append((x, across, height * wobble * math.sin(angle) - drop + 0.0))
        if previous_ring is not None:
            for index in range(segments_across):
                points = [previous_ring[index], ring[index], ring[index + 1], previous_ring[index + 1]]
                mesh.add_face(points, "Thatch", smooth=True, desired_normal=(0.0, 0.0, 1.0 if 0 < index < segments_across - 1 else 0.3))
        previous_ring = ring
    return mesh


# ---------------------------------------------------------------------------------------------------- roof extras
def chimney(width, depth, height, base_depth, material="Brick"):
    """A brick chimney stack with corbelled cap and a pot. Origin at the bottom-left of the front face; the stack
    extends base_depth below z = 0 so it can be sunk into the roof."""
    mesh = Mesh()
    mesh.box(0, width, 0, depth, -base_depth, height, material, skip="D")
    for index, grow in enumerate((0.03, 0.06)):
        mesh.box(-grow, width + grow, -grow, depth + grow, height + index * 0.07, height + index * 0.07 + 0.07, material,
                 skip="" if index == 1 else "")
    top = height + 0.14
    mesh.box(-0.06, width + 0.06, -0.06, depth + 0.06, top, top + 0.05, "Concrete")
    center_x, center_y = width / 2.0, depth / 2.0
    mesh.cylinder(center_x, center_y, top + 0.05, top + 0.45, 0.11, "Metal", segments=12, caps="H")
    mesh.cylinder(center_x, center_y, top + 0.43, top + 0.5, 0.135, "Metal", segments=12, caps="H")
    return mesh


def _gable_front(width, wall_height, apex_height, thickness, material, window):
    """Front wall of a gabled dormer: a rectangle with a triangular gable and a window opening with reveals."""
    mesh = Mesh()
    window_x, window_z = window["x"], window["z"]
    window_width, window_height = window["width"], window["height"]
    head = window_z + window_height
    mesh.box(0, width, 0, thickness, 0, window_z, material, skip="LRTD")
    mesh.add_face([(window_x, 0, window_z), (window_x + window_width, 0, window_z),
                   (window_x + window_width, thickness, window_z), (window_x, thickness, window_z)], material,
                  desired_normal=(0, 0, 1))
    mesh.box(0, window_x, 0, thickness, window_z, head, material, skip="LTD")
    mesh.box(window_x + window_width, width, 0, thickness, window_z, head, material, skip="RTD")
    polygon = [(0, head), (window_x, head), (window_x + window_width, head), (width, head), (width, wall_height),
               (width / 2.0, apex_height), (0, wall_height)]
    mesh.prism(polygon, "xz", 0.0, thickness, material, caps="LH", sides=False)
    mesh.add_face([(window_x, 0, head), (window_x, thickness, head), (window_x + window_width, thickness, head),
                   (window_x + window_width, 0, head)], material, desired_normal=(0, 0, -1))
    return mesh


def _cheek(mesh, x_position, outward_x, points_yz, material):
    """One vertical side wall of a dormer; points_yz is the outline in (depth, height)."""
    mesh.add_face([(x_position, y, z) for y, z in points_yz], material, desired_normal=(outward_x, 0.0, 0.0))


def dormer_gabled(width, wall_height, host_pitch_degrees, roof_pitch_degrees, window, wall_material,
                  cheek_material="Metal", roof_material="RoofTile", thickness=0.2):
    """A small gabled dormer. Local frame: x across, y up the host roof slope seen in plan, z up. The origin is the
    bottom-left of the front wall where it meets the roof surface; cheeks and roof run back until they meet the host
    roof, whose pitch decides the depth. window: dict x, z, width, height."""
    mesh = Mesh()
    roof_tan = math.tan(math.radians(roof_pitch_degrees))
    host_tan = math.tan(math.radians(host_pitch_degrees))
    apex_height = wall_height + (width / 2.0) * roof_tan
    mesh.add(_gable_front(width, wall_height, apex_height, thickness, wall_material, window))
    cheek_depth = wall_height / host_tan
    cheek_outline = [(0.0, -0.02), (cheek_depth, wall_height), (0.0, wall_height)]
    _cheek(mesh, 0.0, -1.0, cheek_outline, cheek_material)
    _cheek(mesh, width, 1.0, list(reversed(cheek_outline)), cheek_material)
    overhang = 0.12
    slab = 0.05
    eave = wall_height - overhang * roof_tan
    cross_section = [
        (-overhang, eave - slab), (width / 2.0, apex_height - slab * 1.2), (width + overhang, eave - slab),
        (width + overhang, eave), (width / 2.0, apex_height), (-overhang, eave),
    ]
    mesh.prism(cross_section, "xz", -0.15, apex_height / host_tan + 0.1, roof_material, caps="LH")
    return mesh


def dormer_shed(width, wall_height, host_pitch_degrees, window, wall_material, roof_pitch_degrees=8.0,
                roof_material="Metal", thickness=0.2):
    """A shed dormer: a box with a front window and a low single-pitch roof rising toward the host roof."""
    mesh = Mesh()
    host_tan = math.tan(math.radians(host_pitch_degrees))
    roof_tan = math.tan(math.radians(roof_pitch_degrees))
    mesh.add(wall_bay(width, wall_height, thickness, wall_material, opening={
        "x": window["x"], "z": window["z"], "width": window["width"], "height": window["height"], "rise": 0.0}))
    meeting_depth = wall_height / (host_tan - roof_tan)
    cheek_outline = [(0.0, -0.02), (meeting_depth, meeting_depth * host_tan), (0.0, wall_height)]
    _cheek(mesh, 0.0, -1.0, cheek_outline, "Metal")
    _cheek(mesh, width, 1.0, list(reversed(cheek_outline)), "Metal")
    overhang = 0.15
    slab = 0.06
    end_depth = meeting_depth + 0.1
    top_start = wall_height - overhang * roof_tan
    top_end = wall_height + end_depth * roof_tan
    outline = [(-overhang, top_start - slab), (end_depth, top_end - slab), (end_depth, top_end), (-overhang, top_start)]
    mesh.prism(outline, "yz", -0.15, width + 0.15, roof_material, caps="LH")
    return mesh


def dormer_eyebrow(width, host_pitch_degrees, window_width, window_height, seed=11):
    """An eyebrow (bat) dormer in a thatch roof: a small window under a lens-shaped bulge of thatch.

    Local frame like the other roof pieces: the origin is on the roof surface at the bulge's front-left corner, x
    across, y toward the ridge in plan, z up. The thatch bulge follows the roof pitch; the window stays vertical."""
    mesh = Mesh()
    depth = width * 1.3
    bulge_height = window_height * 1.2

    def bulge(x, y):
        across = (x - width / 2.0) / (width / 2.0)
        along = y / depth
        if abs(across) >= 1.0 or along >= 1.0:
            return 0.0
        lens = math.sqrt(1.0 - across * across)
        return bulge_height * lens * (1.0 - along) ** 0.7 + 0.015 * smooth_noise(x * 12.0, y * 12.0, seed, 1.0)

    flat = Mesh()
    flat.heightfield(0.0, width, 0.0, depth, 24, 12, bulge, "Thatch")
    mesh.add(flat.rotated_x(host_pitch_degrees))
    window_x = (width - window_width) / 2.0
    window_base = 0.08 * math.tan(math.radians(host_pitch_degrees)) + 0.05
    mesh.box(window_x, window_x + window_width, 0.08, 0.15, window_base, window_base + window_height, "Frame", skip="BD")
    glass_y = 0.1
    mesh.quad_double([(window_x + 0.04, glass_y, window_base + 0.04), (window_x + window_width - 0.04, glass_y, window_base + 0.04),
                      (window_x + window_width - 0.04, glass_y, window_base + window_height - 0.04),
                      (window_x + 0.04, glass_y, window_base + window_height - 0.04)], "Glass")
    return mesh
