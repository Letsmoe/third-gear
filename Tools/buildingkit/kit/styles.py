"""The four building styles. Each builder returns (pieces, spec): an ordered dict of piece name to geom.Mesh, and a dict
with the dimensions the assembly script and the README need (module, storey height, wall thickness, roof pitch ...)."""

import math
from collections import OrderedDict

from . import parts
from .geom import Mesh

MODULE = 2.0


def _place(mesh, x=0.0, y=0.0, z=0.0):
    return mesh.translated(x, y, z)


def _tilted(mesh, pitch_degrees):
    """Tilts a flat roof piece about X so its surface follows the roof pitch (rises toward +Y)."""
    return mesh.rotated_x(pitch_degrees)


def _tile_roof_set(prefix, pitch_degrees, module, wave, step):
    """Tile roof family: eave row with fascia board, plain rows, a half-width row, ridge cap."""
    pieces = OrderedDict()
    plane = _tilted(parts.tile_plane(module, 1.0, wave=wave, step=step), pitch_degrees)
    eave = plane.copy()
    eave.box(0.0, module, -0.022, 0.0, -0.17, 0.03, "Frame", skip="LRBT")
    pieces[prefix + "_Eave"] = eave
    pieces[prefix + "_Plane"] = plane
    pieces[prefix + "_Plane_Half"] = _tilted(parts.tile_plane(module / 2.0, 1.0, wave=wave, step=step), pitch_degrees)
    pieces[prefix + "_Ridge"] = parts.ridge_cap_tile(module, pitch_degrees)
    return pieces


# ============================================================================================ red clinker town house
def build_brick():
    storey = 3.25
    thickness = 0.36
    reveal = 0.20
    pitch = 45.0
    pieces = OrderedDict()
    spec = {"name": "brick", "module": MODULE, "storey": storey, "thickness": thickness, "roof_pitch": pitch,
            "reveal": reveal, "wall_material": "Brick"}

    window_opening = {"x": 0.5, "z": 0.85, "width": 1.0, "height": 1.8, "rise": 0.10}
    door_opening = {"x": 0.45, "z": 0.0, "width": 1.1, "height": 2.28, "rise": 0.12}
    pieces["Wall_Solid"] = parts.wall_bay(MODULE, storey, thickness, "Brick")
    pieces["Wall_Half"] = parts.wall_bay(MODULE / 2.0, storey, thickness, "Brick")
    pieces["Wall_Window"] = parts.wall_bay(MODULE, storey, thickness, "Brick", window_opening)
    pieces["Wall_Door"] = parts.wall_bay(MODULE, storey, thickness, "Brick", door_opening)
    pieces["Corner_L"] = parts.wall_corner(thickness, storey, "Brick")
    pieces["Corner_R"] = pieces["Corner_L"].mirrored_x(thickness)

    window = parts.window_unit(1.0, 1.9, frame_depth=0.08, frame_width=0.06, sash_width=0.045, columns=2,
                               transom_height=0.38, bars_across=1, bars_up=2, arch_rise=0.10)
    pieces["Window"] = _place(window, 0.5, reveal, 0.85)
    door = parts.door_unit(1.1, 2.4, frame_depth=0.09, frame_width=0.07, leaf_panels=(2, 3), arch_rise=0.12,
                           fanlight_height=0.22)
    pieces["Door"] = _place(door, 0.45, reveal, 0.0)
    pieces["Sill"] = parts.sill_piece(0.5, 0.85, 1.0, projection=0.07, overhang=0.06, reveal_depth=reveal + 0.02)
    pieces["Lintel_Arch_Window"] = parts.arch_relief(0.5, 2.65, 1.0, 0.10, 0.11, 0.035)
    pieces["Lintel_Arch_Door"] = parts.arch_relief(0.45, 2.28, 1.1, 0.12, 0.12, 0.035)
    step = Mesh()
    step.box(0.35, 1.65, -0.38, 0.1, -0.16, 0.0, "Sill")
    step.box(0.30, 1.70, -0.62, -0.30, -0.32, -0.16, "Sill", skip="")
    pieces["Door_Step"] = step

    pieces.update(parts.band_pieces(
        "Plinth", [(0, 0), (0.05, 0), (0.05, 0.55), (0.02, 0.62), (0, 0.62)], "Sill", MODULE, thickness))
    pieces.update(parts.band_pieces(
        "StringCourse",
        [(0, storey - 0.22), (0.05, storey - 0.22), (0.05, storey - 0.19), (0.075, storey - 0.19),
         (0.075, storey - 0.06), (0.05, storey - 0.04), (0.05, storey), (0, storey)], "Sill", MODULE, thickness))
    corbels = [(0, storey - 0.50)]
    for step_index in range(4):
        offset = 0.06 + step_index * 0.065
        bottom = storey - 0.50 + step_index * 0.07
        corbels += [(offset, bottom), (offset, bottom + 0.07)]
    corbels += [(0.32, storey - 0.18), (0.32, storey + 0.0), (0.28, storey + 0.07), (0, storey + 0.07)]
    pieces.update(parts.band_pieces("Cornice", corbels, "Brick", MODULE, thickness))

    pieces.update(parts.gutter_pieces("Gutter", MODULE, thickness, 0.0, hang=0.30))
    pieces["Downpipe"] = parts.downpipe_piece(storey)
    pieces.update(_tile_roof_set("Roof_Tile", pitch, MODULE, 0.016, 0.028))
    dormer_window = {"x": 0.3, "z": 0.25, "width": 0.8, "height": 0.9}
    dormer = parts.dormer_gabled(1.4, 1.45, pitch, 38.0, dormer_window, "Brick")
    dormer_unit = parts.window_unit(0.8, 0.9, 0.07, 0.05, 0.04, columns=2, bars_up=2)
    dormer.add(dormer_unit, 0.3, 0.12, 0.25)
    pieces["Dormer_Gable"] = dormer
    pieces["Chimney"] = parts.chimney(0.6, 0.9, 1.6, 1.6)
    spec["dormer_width"] = 1.4
    return pieces, spec


# ================================================================================================ plastered 1950s house
def build_plaster():
    storey = 2.75
    thickness = 0.30
    reveal = 0.15
    pitch = 35.0
    pieces = OrderedDict()
    spec = {"name": "plaster", "module": MODULE, "storey": storey, "thickness": thickness, "roof_pitch": pitch,
            "reveal": reveal, "wall_material": "Plaster"}

    window_opening = {"x": 0.4, "z": 0.95, "width": 1.2, "height": 1.4, "rise": 0.0}
    door_opening = {"x": 0.5, "z": 0.0, "width": 1.0, "height": 2.1, "rise": 0.0}
    pieces["Wall_Solid"] = parts.wall_bay(MODULE, storey, thickness, "Plaster")
    pieces["Wall_Half"] = parts.wall_bay(MODULE / 2.0, storey, thickness, "Plaster")
    pieces["Wall_Window"] = parts.wall_bay(MODULE, storey, thickness, "Plaster", window_opening)
    pieces["Wall_Door"] = parts.wall_bay(MODULE, storey, thickness, "Plaster", door_opening)
    pieces["Corner_L"] = parts.wall_corner(thickness, storey, "Plaster")
    pieces["Corner_R"] = pieces["Corner_L"].mirrored_x(thickness)

    window = parts.window_unit(1.2, 1.4, frame_depth=0.07, frame_width=0.055, sash_width=0.055, columns=2,
                               transom_height=0.0, bars_across=1, bars_up=1)
    pieces["Window"] = _place(window, 0.4, reveal, 0.95)
    door = parts.door_unit(1.0, 2.1, frame_depth=0.08, frame_width=0.07, leaf_panels=(1, 2), glazed=False)
    pieces["Door"] = _place(door, 0.5, reveal, 0.0)
    pieces["Sill"] = parts.sill_piece(0.4, 0.95, 1.2, projection=0.04, overhang=0.03, reveal_depth=reveal + 0.02,
                                      material="Concrete", thickness=0.05)
    canopy = Mesh()
    canopy.box(0.2, 1.8, -0.9, 0.05, 2.25, 2.33, "Concrete")
    canopy.tube((0.25, -0.85, 2.25), (0.25, 0.0, 2.55), 0.012, "Metal", segments=6)
    canopy.tube((1.75, -0.85, 2.25), (1.75, 0.0, 2.55), 0.012, "Metal", segments=6)
    pieces["Canopy"] = canopy

    pieces.update(parts.band_pieces(
        "Plinth", [(0, 0), (0.025, 0), (0.025, 0.45), (0, 0.45)], "Concrete", MODULE, thickness))
    pieces.update(parts.band_pieces(
        "StringCourse", [(0, storey - 0.16), (0.02, storey - 0.16), (0.02, storey), (0, storey)], "Plaster", MODULE,
        thickness))
    pieces.update(parts.band_pieces(
        "Cornice", [(0, storey - 0.22), (0.16, storey - 0.22), (0.16, storey - 0.05), (0.19, storey - 0.02),
                    (0.19, storey + 0.06), (0, storey + 0.06)], "Plaster", MODULE, thickness))
    pieces.update(parts.gutter_pieces("Gutter", MODULE, thickness, 0.0, radius=0.06, hang=0.19))
    pieces["Downpipe"] = parts.downpipe_piece(storey, radius=0.038)
    pieces.update(_tile_roof_set("Roof_Tile", pitch, MODULE, 0.010, 0.02))
    dormer_window = {"x": 0.3, "z": 0.3, "width": 1.2, "height": 0.8}
    dormer = parts.dormer_shed(1.8, 1.25, pitch, dormer_window, "Plaster", roof_pitch_degrees=8.0)
    dormer.add(parts.window_unit(1.2, 0.8, 0.07, 0.05, 0.045, columns=2), 0.3, 0.12, 0.3)
    pieces["Dormer_Shed"] = dormer
    pieces["Chimney"] = parts.chimney(0.55, 0.55, 1.2, 1.2)
    return pieces, spec


# ==================================================================================================== postwar block
def _balcony(panel_style):
    """A concrete balcony slab for one module, 1.4 m deep. The slab top is at z = 0.02, the floor of the storey."""
    mesh = Mesh()
    depth = 1.4
    mesh.box(0.0, MODULE, -depth, 0.15, -0.16, 0.02, "Concrete")
    mesh.box(0.0, 0.07, -depth, 0.0, 0.02, 1.0, "Concrete", skip="")
    mesh.box(MODULE - 0.07, MODULE, -depth, 0.0, 0.02, 1.0, "Concrete", skip="")
    if panel_style == "panel":
        mesh.box(0.07, MODULE - 0.07, -depth, -depth + 0.08, 0.02, 0.98, "Concrete")
        mesh.box(0.0, MODULE, -depth - 0.015, -depth + 0.095, 0.98, 1.04, "Metal")
        return mesh
    mesh.box(0.0, MODULE, -depth - 0.015, -depth + 0.05, 0.98, 1.04, "Metal")
    mesh.box(0.0, MODULE, -depth - 0.015, -depth + 0.05, 0.02, 0.09, "Metal")
    bars = 15
    for index in range(bars):
        x = 0.1 + (MODULE - 0.2) * index / (bars - 1)
        mesh.box(x - 0.012, x + 0.012, -depth, -depth + 0.025, 0.09, 0.98, "Metal")
    return mesh


def build_block():
    storey = 2.75
    thickness = 0.30
    reveal = 0.15
    pieces = OrderedDict()
    spec = {"name": "block", "module": MODULE, "storey": storey, "thickness": thickness, "roof_pitch": 0.0,
            "reveal": reveal, "wall_material": "Plaster"}

    window_opening = {"x": 0.2, "z": 0.85, "width": 1.6, "height": 1.3, "rise": 0.0}
    balcony_opening = {"x": 0.2, "z": 0.0, "width": 1.6, "height": 2.15, "rise": 0.0}
    door_opening = {"x": 0.4, "z": 0.0, "width": 1.2, "height": 2.2, "rise": 0.0}
    stair_opening = {"x": 0.5, "z": 0.45, "width": 1.0, "height": 1.9, "rise": 0.0}
    pieces["Wall_Solid"] = parts.wall_bay(MODULE, storey, thickness, "Plaster")
    pieces["Wall_Half"] = parts.wall_bay(MODULE / 2.0, storey, thickness, "Plaster")
    pieces["Wall_Window"] = parts.wall_bay(MODULE, storey, thickness, "Plaster", window_opening)
    pieces["Wall_Balcony"] = parts.wall_bay(MODULE, storey, thickness, "Plaster", balcony_opening)
    pieces["Wall_Door"] = parts.wall_bay(MODULE, storey, thickness, "Plaster", door_opening)
    pieces["Wall_Stair"] = parts.wall_bay(MODULE, storey, thickness, "Plaster", stair_opening)
    pieces["Corner_L"] = parts.wall_corner(thickness, storey, "Plaster")
    pieces["Corner_R"] = pieces["Corner_L"].mirrored_x(thickness)

    window = parts.window_unit(1.6, 1.3, frame_depth=0.07, frame_width=0.04, sash_width=0.035, columns=3,
                               bars_across=1, bars_up=1)
    pieces["Window"] = _place(window, 0.2, reveal, 0.85)
    balcony_window = parts.window_unit(1.6, 2.15, frame_depth=0.07, frame_width=0.04, sash_width=0.035, columns=2,
                                       transom_height=0.45)
    pieces["Window_Balcony"] = _place(balcony_window, 0.2, reveal, 0.0)
    stair_window = parts.window_unit(1.0, 1.9, frame_depth=0.07, frame_width=0.04, sash_width=0.035, columns=1,
                                     bars_across=3, bars_up=6, bar_width=0.025)
    pieces["Window_Stair"] = _place(stair_window, 0.5, reveal, 0.45)
    door = parts.door_unit(1.2, 2.2, frame_depth=0.08, frame_width=0.05, glazed=True, fanlight_height=0.35)
    pieces["Door_Glazed"] = _place(door, 0.4, reveal, 0.0)
    pieces["Sill"] = parts.sill_piece(0.2, 0.85, 1.6, projection=0.035, overhang=0.02, reveal_depth=reveal + 0.02,
                                      material="Concrete", thickness=0.04)
    pieces["Balcony_Panel"] = _balcony("panel")
    pieces["Balcony_Rail"] = _balcony("rail")
    canopy = Mesh()
    canopy.box(0.0, MODULE, -1.2, 0.05, 2.35, 2.5, "Concrete")
    canopy.tube((0.1, -1.1, 2.35), (0.1, 0.0, 2.9), 0.012, "Metal", segments=6)
    canopy.tube((MODULE - 0.1, -1.1, 2.35), (MODULE - 0.1, 0.0, 2.9), 0.012, "Metal", segments=6)
    pieces["Canopy"] = canopy

    pieces.update(parts.band_pieces(
        "Plinth", [(0, 0), (0.04, 0), (0.04, 0.45), (0, 0.45)], "Concrete", MODULE, thickness))
    pieces.update(parts.band_pieces(
        "FloorBand", [(0, storey - 0.24), (0.02, storey - 0.24), (0.02, storey), (0, storey)], "Concrete", MODULE,
        thickness))
    pieces.update(parts.band_pieces(
        "RoofEdge", [(0, storey - 0.3), (0.05, storey - 0.3), (0.05, storey + 0.36), (0.13, storey + 0.36),
                     (0.13, storey + 0.42), (0, storey + 0.42)], "Concrete", MODULE, thickness))
    roof = Mesh()
    roof.box(0.0, MODULE, 0.0, MODULE, -0.1, 0.0, "Concrete", skip="LRBF")
    roof.heightfield(0.0, MODULE, 0.0, MODULE, 8, 8, lambda x, y: 0.0, "Concrete")
    pieces["Roof_Flat"] = roof
    pieces["Downpipe"] = parts.downpipe_piece(storey, radius=0.045)
    return pieces, spec


# ================================================================================================= Vierlande farmhouse
def _brace(mesh, start, end, width, depth, y_front=0.0):
    """A diagonal timber between two points in the wall plane (x, z)."""
    dx, dz = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dz)
    normal = (-dz / length * width / 2.0, dx / length * width / 2.0)
    polygon = [(start[0] - normal[0], start[1] - normal[1]), (end[0] - normal[0], end[1] - normal[1]),
               (end[0] + normal[0], end[1] + normal[1]), (start[0] + normal[0], start[1] + normal[1])]
    mesh.prism(polygon, "xz", y_front, y_front + depth, "Timber", caps="LH")


def _timber_frame(width, height, timber_depth, opening, post_width=0.14, rail=0.18):
    """Timber members of one Fachwerk bay: half posts at both ends, sill beam, top plate, a mid rail and braces. With
    an opening, studs line it instead of a mid post."""
    mesh = Mesh()
    half = post_width / 2.0
    mesh.box(0.0, half, 0.0, timber_depth, 0.0, height, "Timber", skip="LD")
    mesh.box(width - half, width, 0.0, timber_depth, 0.0, height, "Timber", skip="RD")
    mesh.box(half, width - half, 0.0, timber_depth, 0.0, rail, "Timber", skip="LRD")
    mesh.box(half, width - half, 0.0, timber_depth, height - rail, height, "Timber", skip="LRT")
    if opening is None:
        mesh.box(width / 2.0 - 0.06, width / 2.0 + 0.06, 0.0, timber_depth, rail, height - rail, "Timber", skip="")
        mesh.box(half, width - half, 0.0, timber_depth * 0.9, 1.0, 1.12, "Timber", skip="LR")
        for side in (-1.0, 1.0):
            outer_x = half if side < 0 else width - half
            inner_x = width / 2.0 - 0.06 if side < 0 else width / 2.0 + 0.06
            _brace(mesh, (outer_x, rail), (inner_x, 1.0), 0.09, timber_depth * 0.85)
            _brace(mesh, (outer_x, height - rail), (inner_x, 1.12), 0.09, timber_depth * 0.85)
        return mesh
    stud = 0.09
    left_stud = opening["x"] - stud
    right_stud = opening["x"] + opening["width"]
    head = opening["z"] + opening["height"]
    bottom = opening["z"]
    top_of_studs = height - rail
    mesh.box(left_stud, left_stud + stud, 0.0, timber_depth, rail, top_of_studs, "Timber", skip="")
    mesh.box(right_stud, right_stud + stud, 0.0, timber_depth, rail, top_of_studs, "Timber", skip="")
    if bottom > rail + 0.05:
        mesh.box(left_stud + stud, right_stud, 0.0, timber_depth, bottom - 0.12, bottom, "Timber", skip="LR")
    mesh.box(left_stud + stud, right_stud, 0.0, timber_depth, head, head + 0.13, "Timber", skip="LR")
    _brace(mesh, (half, rail), (left_stud, head), 0.09, timber_depth * 0.85)
    _brace(mesh, (width - half, rail), (right_stud + stud, head), 0.09, timber_depth * 0.85)
    return mesh


def _timber_bay(width, height, thickness, opening, timber_depth=0.17, infill_front=0.04):
    """A Fachwerk wall bay: brick infill set back from the timber, timber frame in front."""
    mesh = parts.wall_bay(width, height, thickness - infill_front, "Brick", opening).translated(0.0, infill_front, 0.0)
    mesh.add(_timber_frame(width, height, timber_depth, opening))
    return mesh


def _gate(width, height):
    """Two boarded gate leaves with diagonal braces, filling an opening of this size."""
    mesh = Mesh()
    leaf = width / 2.0 - 0.01
    for index in range(2):
        x0 = index * (leaf + 0.02)
        mesh.box(x0, x0 + leaf, 0.0, 0.05, 0.0, height, "Timber", skip="D")
        for board in range(1, 6):
            board_x = x0 + leaf * board / 6.0
            mesh.box(board_x - 0.004, board_x + 0.004, -0.008, 0.0, 0.0, height, "Timber", skip="")
        mesh.box(x0, x0 + leaf, -0.025, 0.0, height * 0.12, height * 0.12 + 0.12, "Timber", skip="")
        mesh.box(x0, x0 + leaf, -0.025, 0.0, height * 0.82, height * 0.82 + 0.12, "Timber", skip="")
    return mesh


def build_farm():
    storey = 2.25
    thickness = 0.25
    reveal = 0.15
    pitch = 50.0
    pieces = OrderedDict()
    spec = {"name": "farm", "module": MODULE, "storey": storey, "thickness": thickness, "roof_pitch": pitch,
            "reveal": reveal, "wall_material": "Brick"}

    window_opening = {"x": 0.55, "z": 0.80, "width": 0.9, "height": 1.0, "rise": 0.0}
    door_opening = {"x": 0.5, "z": 0.0, "width": 1.0, "height": 1.85, "rise": 0.0}
    gate_opening = {"x": 0.2, "z": 0.0, "width": 1.6, "height": 1.9, "rise": 0.0}
    pieces["Wall_Frame"] = _timber_bay(MODULE, storey, thickness, None)
    pieces["Wall_Frame_Half"] = _half_frame(storey, thickness)
    pieces["Wall_Frame_Window"] = _timber_bay(MODULE, storey, thickness, window_opening)
    pieces["Wall_Frame_Door"] = _timber_bay(MODULE, storey, thickness, door_opening)
    pieces["Wall_Frame_Gate"] = _timber_bay(MODULE, storey, thickness, gate_opening)
    corner = Mesh()
    corner.box(0, thickness, 0, thickness, 0, storey, "Timber", skip="RTD")
    pieces["Corner_L"] = corner
    pieces["Corner_R"] = corner.mirrored_x(thickness)

    window = parts.window_unit(0.9, 1.0, frame_depth=0.07, frame_width=0.05, sash_width=0.04, columns=2,
                               bars_across=2, bars_up=3, bar_width=0.018)
    pieces["Window"] = _place(window, 0.55, reveal, 0.80)
    door = parts.door_unit(1.0, 1.85, frame_depth=0.08, frame_width=0.07, leaf_panels=(1, 3))
    pieces["Door"] = _place(door, 0.5, reveal, 0.0)
    pieces["Gate"] = _place(_gate(1.6, 1.9), 0.2, reveal, 0.0)

    pieces.update(parts.band_pieces(
        "Plinth", [(0, -0.4), (0.03, -0.4), (0.03, -0.02), (0, -0.0)], "Brick", MODULE, thickness))
    pieces.update(_thatch_set(pitch, MODULE))
    eyebrow = parts.dormer_eyebrow(1.3, pitch, 0.6, 0.45)
    pieces["Dormer_Eyebrow"] = eyebrow
    pieces["Chimney"] = parts.chimney(0.7, 0.9, 1.4, 1.8)
    return pieces, spec


def _half_frame(height, thickness):
    mesh = parts.wall_bay(MODULE / 2.0, height, thickness - 0.04, "Brick").translated(0.0, 0.04, 0.0)
    half_width = MODULE / 2.0
    mesh.box(0.0, 0.07, 0.0, 0.17, 0.0, height, "Timber", skip="LD")
    mesh.box(half_width - 0.07, half_width, 0.0, 0.17, 0.0, height, "Timber", skip="RD")
    mesh.box(0.07, half_width - 0.07, 0.0, 0.17, 0.0, 0.18, "Timber", skip="LRD")
    mesh.box(0.07, half_width - 0.07, 0.0, 0.17, height - 0.18, height, "Timber", skip="LRT")
    mesh.box(0.07, half_width - 0.07, 0.0, 0.15, 1.0, 1.12, "Timber", skip="LR")
    _brace(mesh, (0.07, 0.18), (half_width - 0.07, 1.0), 0.09, 0.14)
    _brace(mesh, (half_width - 0.07, height - 0.18), (0.07, 1.12), 0.09, 0.14)
    return mesh


def _thatch_set(pitch_degrees, module):
    """Thatch roof family: thick eave with a cut edge, plain rows, half row, rolled ridge."""
    pieces = OrderedDict()
    eave = _tilted(parts.thatch_plane(module, 1.0, 0.48, seed=2), pitch_degrees)
    pieces["Roof_Thatch_Eave"] = eave
    pieces["Roof_Thatch_Plane"] = _tilted(parts.thatch_plane(module, 1.0, 0.36, seed=5), pitch_degrees)
    pieces["Roof_Thatch_Plane_Half"] = _tilted(parts.thatch_plane(module / 2.0, 1.0, 0.36, seed=8), pitch_degrees)
    pieces["Roof_Thatch_Ridge"] = parts.thatch_ridge(module, pitch_degrees)
    return pieces


STYLES = OrderedDict([
    ("brick", build_brick),
    ("plaster", build_plaster),
    ("block", build_block),
    ("farm", build_farm),
])
