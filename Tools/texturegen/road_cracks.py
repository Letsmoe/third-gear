"""Generates the road crack atlas: 4 x 4 tiles of 2 m x 2 m road surface defects, which the road material scatters
over plain asphalt by a cell hash (see ROAD_CRACKS_HLSL in Scripts/create_materials.py), so they never repeat.

Tiles: meandering open cracks with branches, tar-sealed crack lines, alligator cracking, block cracking, a transverse
crack, hairline networks, and patch repairs: trench strips, clusters of lobed pothole patches in light and dark tones
with loose rims, and patches laid into fatigue cracking. Every defect stays inside its tile's margin, so cells can be
rotated and mirrored freely without cut-off cracks at the borders.

Output in <data root>/texturegen/road_cracks/:
  T_RoadCracks_Mask.png    R open crack (1 = crack), G tar sealant, B patch repair, A patch tone (0 weathered light, 1 fresh black)
  T_RoadCracks_Normal.png  DirectX tangent-space normal from the height
Usage: <osmimport venv python> -I Tools/texturegen/road_cracks.py [--preview out.jpg]
"""
import argparse
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage
from scipy.spatial import Voronoi

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402

TILE_PIXELS = 1024
TILES_PER_SIDE = 4
TILE_METRES = 2.0
# Drawing happens at twice the resolution and is downsampled, for anti-aliased lines.
SUPERSAMPLE = 2
PIXELS_PER_METRE = TILE_PIXELS / TILE_METRES
MARGIN = 0.12  # share of the tile kept free of defects at each border
# Depths in metres; the normal map is computed from them.
CRACK_DEPTH = 0.006
SEALANT_RAISE = 0.0015
PATCH_STEP = 0.003


class TileCanvas:
    """The supersampled layers of one tile, in tile metres with (0, 0) at the top left."""

    def __init__(self):
        size = TILE_PIXELS * SUPERSAMPLE
        self.crack = Image.new("L", (size, size), 0)
        self.sealant = Image.new("L", (size, size), 0)
        self.patch = Image.new("L", (size, size), 0)
        # Tone of each patch (0 weathered light grey, 1 fresh black), and the gap left where a patch came loose,
        # drawn after the patches so they don't cover it.
        self.shade = Image.new("L", (size, size), 128)
        self.rim = Image.new("L", (size, size), 0)
        self.scale = PIXELS_PER_METRE * SUPERSAMPLE

    def pixels(self, points):
        """Tile metres to supersampled pixels."""
        return [(x * self.scale, y * self.scale) for x, y in points]

    def draw_line(self, layer, points, width_metres):
        """Draws a polyline with round joints into a layer."""
        width = max(1, int(round(width_metres * self.scale)))
        draw = ImageDraw.Draw(layer)
        pixel_points = self.pixels(points)
        draw.line(pixel_points, fill=255, width=width, joint="curve")
        radius = width / 2
        for x, y in pixel_points:
            draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=255)


def meander(rng, start, heading, length, step=0.008, wiggle=0.35, drift=0.0, jag=0.55):
    """A crack path: a random walk whose direction changes smoothly, kept inside the tile margin. Each small step
    also zigzags off that direction by `jag` radians, as cracks run around the stones of the asphalt."""
    points = [start]
    x, y = start
    turn = 0.0
    lower, upper = MARGIN * TILE_METRES, (1 - MARGIN) * TILE_METRES
    for _ in range(int(length / step)):
        turn = turn * 0.93 + rng.normal(0, wiggle) * 0.07 + drift
        heading += turn * step * 10
        if rng.random() < 0.01:
            heading += rng.normal(0, 0.5)  # an occasional kink
        direction = heading + rng.normal(0, jag)
        x += math.cos(direction) * step
        y += math.sin(direction) * step
        if not (lower < x < upper and lower < y < upper):
            break
        points.append((x, y))
    return points


def tapered_crack(canvas, rng, points, width):
    """Draws a crack that narrows towards both ends, as real cracks do."""
    count = len(points)
    if count < 2:
        return
    pieces = 6
    for piece in range(pieces):
        first = piece * (count - 1) // pieces
        last = (piece + 1) * (count - 1) // pieces + 1
        middle = (piece + 0.5) / pieces
        taper = 0.35 + 0.65 * math.sin(math.pi * middle)
        canvas.draw_line(canvas.crack, points[first:last], width * taper * rng.uniform(0.8, 1.2))


def random_start(rng):
    """A point well inside the tile."""
    return (rng.uniform(0.3, 0.7) * TILE_METRES, rng.uniform(0.3, 0.7) * TILE_METRES)


def branched_crack(canvas, rng, length, width, branches):
    """A main meandering crack from the middle outwards in both directions, with side branches."""
    start = random_start(rng)
    heading = rng.uniform(0, 2 * math.pi)
    forward = meander(rng, start, heading, length / 2)
    backward = meander(rng, start, heading + math.pi, length / 2)
    main = list(reversed(backward)) + forward[1:]
    tapered_crack(canvas, rng, main, width)
    for _ in range(branches):
        if len(main) < 10:
            break
        origin = main[rng.integers(len(main) // 5, len(main) * 4 // 5)]
        branch = meander(rng, origin, heading + rng.choice([-1, 1]) * rng.uniform(0.6, 1.3), rng.uniform(0.15, 0.6))
        tapered_crack(canvas, rng, branch, width * 0.6)
    return main


def sealed_crack(canvas, rng, length):
    """A crack sealed with a band of tar, the open crack partly visible again inside it."""
    start = random_start(rng)
    heading = rng.uniform(0, 2 * math.pi)
    path = list(reversed(meander(rng, start, heading + math.pi, length / 2, wiggle=0.25)))
    path += meander(rng, start, heading, length / 2, wiggle=0.25)[1:]
    width = rng.uniform(0.03, 0.06)
    for first in range(0, len(path) - 1, 12):
        canvas.draw_line(canvas.sealant, path[first:first + 13], width * rng.uniform(0.7, 1.3))
    for _ in range(int(rng.integers(2, 6))):
        x, y = path[rng.integers(0, len(path))]
        radius = rng.uniform(0.02, 0.05)
        blob = [(x + math.cos(a) * radius * rng.uniform(0.6, 1.2), y + math.sin(a) * radius * rng.uniform(0.6, 1.2))
                for a in np.linspace(0, 2 * math.pi, 9)[:-1]]
        ImageDraw.Draw(canvas.sealant).polygon(canvas.pixels(blob), fill=255)
    reopened = path[len(path) // 4: len(path) * 3 // 4]
    tapered_crack(canvas, rng, reopened, 0.0025)


def alligator(canvas, rng, radius_x, radius_y, cell):
    """Fatigue cracking: a patch of small connected polygons inside an ellipse. Returns its centre."""
    centre = random_start(rng)
    count = int(4 * radius_x * radius_y / (cell * cell))
    seeds = np.column_stack([rng.uniform(-radius_x, radius_x, count), rng.uniform(-radius_y, radius_y, count)])
    diagram = Voronoi(seeds)
    for first, second in diagram.ridge_vertices:
        if first < 0 or second < 0:
            continue
        a, b = diagram.vertices[first], diagram.vertices[second]
        mid = (a + b) / 2
        inside = (mid[0] / radius_x) ** 2 + (mid[1] / radius_y) ** 2
        if inside > 1 or rng.random() < inside * 0.5:
            continue
        bend = (a + b) / 2 + rng.normal(0, cell * 0.08, 2)
        points = [tuple(a + centre), tuple(bend + centre), tuple(b + centre)]
        canvas.draw_line(canvas.crack, points, rng.uniform(0.003, 0.007) * (1.2 - inside))
    return centre


def patch_shade(rng):
    """Tone of one patch: most cold-mix patches weather lighter than the road around them, some stay darker."""
    if rng.random() < 0.65:
        return int(rng.uniform(0.0, 0.35) * 255)
    return int(rng.uniform(0.65, 1.0) * 255)


def fill_patch(canvas, rng, outline):
    """Fills a patch outline in the patch and shade layers."""
    pixels = canvas.pixels(outline)
    ImageDraw.Draw(canvas.patch).polygon(pixels, fill=255)
    ImageDraw.Draw(canvas.shade).polygon(pixels, fill=patch_shade(rng))


def patch_repair(canvas, rng, width=None, height=None, centre=None):
    """A cut-out rectangular patch of newer asphalt with sealed joints, slightly rotated. Returns its corners."""
    width = width or rng.uniform(0.5, 1.4)
    height = height or rng.uniform(0.4, 1.2)
    cx, cy = centre or random_start(rng)
    angle = rng.normal(0, 0.06)
    corners = []
    for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
        x, y = sx * width / 2, sy * height / 2
        corners.append((cx + x * math.cos(angle) - y * math.sin(angle), cy + x * math.sin(angle) + y * math.cos(angle)))
    fill_patch(canvas, rng, ragged_outline(rng, corners))
    sealed_joints(canvas, rng, corners)
    return corners


def ragged_outline(rng, corners, step=0.03, roughness=0.004):
    """A cut edge is never quite straight: the polygon's sides subdivided and jittered by a few millimetres."""
    outline = []
    for index, start in enumerate(corners):
        end = corners[(index + 1) % len(corners)]
        length = math.dist(start, end)
        for i in range(max(1, int(length / step))):
            t = i * step / length
            outline.append((start[0] + (end[0] - start[0]) * t + rng.normal(0, roughness),
                            start[1] + (end[1] - start[1]) * t + rng.normal(0, roughness)))
    return outline


def sealed_joints(canvas, rng, corners):
    """Bitumen along some stretches of a patch's joints; the rest of the seal has worn away."""
    for index, start in enumerate(corners):
        if rng.random() < 0.35:
            continue
        end = corners[(index + 1) % len(corners)]
        first, last = sorted(rng.uniform(0, 1, 2))
        last = max(last, first + 0.3)
        a = (start[0] + (end[0] - start[0]) * first, start[1] + (end[1] - start[1]) * first)
        b = (start[0] + (end[0] - start[0]) * min(last, 1), start[1] + (end[1] - start[1]) * min(last, 1))
        canvas.draw_line(canvas.sealant, [a, b], rng.uniform(0.006, 0.014))


def trench_strip(canvas, rng):
    """The long narrow patch over a utility trench, with a crack along one joint where it settled."""
    centre = (TILE_METRES / 2, rng.uniform(0.35, 0.65) * TILE_METRES)
    corners = patch_repair(canvas, rng, width=rng.uniform(1.3, 1.45), height=rng.uniform(0.3, 0.6), centre=centre)
    edge = [corners[0], corners[1]] if rng.random() < 0.5 else [corners[3], corners[2]]
    steps = 40
    path = [(edge[0][0] + (edge[1][0] - edge[0][0]) * i / steps, edge[0][1] + (edge[1][1] - edge[0][1]) * i / steps)
            for i in range(int(steps * rng.uniform(0.4, 1.0)))]
    tapered_crack(canvas, rng, path, 0.004)


def blob_outline(rng, centre, radius):
    """A lobed pothole outline: a circle whose radius wanders smoothly with a few bulges, plus millimetre jitter."""
    count = 72
    angles = np.linspace(0, 2 * math.pi, count, endpoint=False)
    reach = np.ones(count)
    for lobes in (2, 3, 5):
        reach += rng.uniform(0.06, 0.18) * np.cos(lobes * angles + rng.uniform(0, 2 * math.pi))
    stretch = rng.uniform(1.0, 1.6)
    turn = rng.uniform(0, math.pi)
    outline = []
    for angle, scale in zip(angles, reach):
        x, y = math.cos(angle) * radius * scale * stretch, math.sin(angle) * radius * scale
        outline.append((centre[0] + x * math.cos(turn) - y * math.sin(turn) + rng.normal(0, 0.003),
                        centre[1] + x * math.sin(turn) + y * math.cos(turn) + rng.normal(0, 0.003)))
    return outline


def loose_rim(canvas, rng, outline):
    """Dark gaps along parts of a patch's edge, where it has shrunk away from the road around it."""
    count = len(outline)
    for _ in range(int(rng.integers(1, 4))):
        first = int(rng.integers(0, count))
        length = int(rng.integers(count // 8, count // 3))
        stretch = [outline[(first + i) % count] for i in range(length)]
        tapered_crack(canvas, rng, stretch, rng.uniform(0.006, 0.012))
    # tapered_crack drew into the crack layer; move it to the rim layer so the patch doesn't erase it
    canvas.rim = Image.fromarray(np.maximum(np.asarray(canvas.rim), np.asarray(canvas.crack)))
    canvas.crack = Image.new("L", canvas.crack.size, 0)


def pothole_patches(canvas, rng, count):
    """A cluster of overlapping pothole patches along a wheel path, each its own tone, some with loose rims."""
    start = random_start(rng)
    heading = rng.uniform(0, 2 * math.pi)
    centre = start
    for _ in range(count):
        radius = rng.uniform(0.14, 0.36)
        outline = blob_outline(rng, centre, radius)
        fill_patch(canvas, rng, outline)
        if rng.random() < 0.6:
            existing = canvas.crack.copy()
            canvas.crack = Image.new("L", canvas.crack.size, 0)
            loose_rim(canvas, rng, outline)
            canvas.crack = existing
        step = radius * rng.uniform(1.0, 2.0)
        heading += rng.normal(0, 0.5)
        lower, upper = MARGIN * TILE_METRES + 0.3, (1 - MARGIN) * TILE_METRES - 0.3
        centre = (min(max(centre[0] + math.cos(heading) * step, lower), upper),
                  min(max(centre[1] + math.sin(heading) * step, lower), upper))


def patch_over_cracking(canvas, rng):
    """Pothole patches laid into an area of fatigue cracking, the cracking still showing around them."""
    centre = alligator(canvas, rng, rng.uniform(0.55, 0.72), rng.uniform(0.4, 0.55), rng.uniform(0.12, 0.22))
    for _ in range(int(rng.integers(1, 3))):
        spot = (centre[0] + rng.uniform(-0.25, 0.25), centre[1] + rng.uniform(-0.15, 0.15))
        outline = blob_outline(rng, spot, rng.uniform(0.12, 0.25))
        fill_patch(canvas, rng, outline)
        existing = canvas.crack.copy()
        canvas.crack = Image.new("L", canvas.crack.size, 0)
        loose_rim(canvas, rng, outline)
        canvas.crack = existing


def block_cracking(canvas, rng):
    """A few long cracks meeting at angles, outlining irregular blocks."""
    paths = [branched_crack(canvas, rng, rng.uniform(1.0, 1.5), 0.005, 0)]
    for _ in range(2):
        origin = paths[0][rng.integers(len(paths[0]) // 4, max(len(paths[0]) * 3 // 4, len(paths[0]) // 4 + 1))]
        heading = rng.uniform(0, 2 * math.pi)
        side = meander(rng, origin, heading, rng.uniform(0.5, 1.0), wiggle=0.2)
        tapered_crack(canvas, rng, side, 0.004)
        paths.append(side)


def hairlines(canvas, rng):
    """A fine network of short shallow cracks."""
    for _ in range(rng.integers(5, 9)):
        start = (rng.uniform(0.2, 0.8) * TILE_METRES, rng.uniform(0.2, 0.8) * TILE_METRES)
        tapered_crack(canvas, rng, meander(rng, start, rng.uniform(0, 2 * math.pi), rng.uniform(0.1, 0.35), step=0.01), 0.0018)


def transverse(canvas, rng):
    """A crack across the lane, fairly straight, with short spurs."""
    start = (MARGIN * TILE_METRES + 0.02, rng.uniform(0.4, 0.6) * TILE_METRES)
    path = meander(rng, start, rng.normal(0, 0.1), TILE_METRES, wiggle=0.15)
    tapered_crack(canvas, rng, path, 0.010)
    for _ in range(3):
        origin = path[rng.integers(0, len(path))]
        tapered_crack(canvas, rng, meander(rng, origin, rng.uniform(0, 2 * math.pi), rng.uniform(0.05, 0.2)), 0.003)


def make_tile(index, rng):
    """Draws tile number `index` of the atlas."""
    canvas = TileCanvas()
    if index < 5:
        branched_crack(canvas, rng, rng.uniform(0.8, 1.6), rng.uniform(0.007, 0.013), int(rng.integers(1, 4)))
    elif index < 7:
        sealed_crack(canvas, rng, rng.uniform(1.0, 1.6))
    elif index == 7:
        alligator(canvas, rng, rng.uniform(0.5, 0.72), rng.uniform(0.3, 0.55), rng.uniform(0.12, 0.22))
    elif index == 8:
        trench_strip(canvas, rng)
    elif index < 11:
        pothole_patches(canvas, rng, int(rng.integers(2, 6)))
    elif index < 13:
        patch_over_cracking(canvas, rng)
    elif index == 13:
        block_cracking(canvas, rng)
    elif index == 14:
        transverse(canvas, rng)
    else:
        hairlines(canvas, rng)
    return canvas


def downsample(layer):
    """Supersampled layer to tile resolution, 0 to 1."""
    return np.asarray(layer.resize((TILE_PIXELS, TILE_PIXELS), Image.LANCZOS), dtype=np.float32) / 255.0


def height_of(crack, sealant, patch):
    """Surface height in metres: cracks sink in (deeper at their middle), sealant sits proud, patches step."""
    crack_depth = ndimage.gaussian_filter(crack, 0.8) * CRACK_DEPTH
    patch_soft = ndimage.gaussian_filter(patch, 1.5)
    return -crack_depth + ndimage.gaussian_filter(sealant, 1.2) * SEALANT_RAISE - patch_soft * PATCH_STEP


def normal_from_height(height):
    """DirectX tangent-space normal (green down) from a height map in metres."""
    pixel = 1.0 / PIXELS_PER_METRE
    gy, gx = np.gradient(height, pixel)
    normal = np.dstack([-gx, gy, np.ones_like(height)])
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    return ((normal * 0.5 + 0.5) * 255).round().astype(np.uint8)


def build_atlas(seed=7):
    """Draws all tiles and returns the mask (RGBA) and normal (RGB) atlases as uint8 arrays."""
    rng = np.random.default_rng(seed)
    side = TILE_PIXELS * TILES_PER_SIDE
    mask = np.zeros((side, side, 4), np.uint8)
    normal = np.zeros((side, side, 3), np.uint8)
    for index in range(TILES_PER_SIDE * TILES_PER_SIDE):
        canvas = make_tile(index, rng)
        crack, sealant, patch = downsample(canvas.crack), downsample(canvas.sealant), downsample(canvas.patch)
        shade = downsample(canvas.shade)
        crack *= 1.0 - sealant * 0.85  # the tar covers most of a sealed crack
        crack *= 1.0 - np.clip(patch * 4.0, 0.0, 1.0)  # a patch covers the cracking it was laid over
        crack = np.maximum(crack, downsample(canvas.rim))
        # Dirt and water staining darken the asphalt a couple of centimetres either side of a crack.
        crack = np.maximum(crack, np.clip(ndimage.gaussian_filter(crack, 6.0) * 1.2, 0, 0.3))
        height = height_of(crack, sealant, patch)
        row, column = divmod(index, TILES_PER_SIDE)
        area = (slice(row * TILE_PIXELS, (row + 1) * TILE_PIXELS), slice(column * TILE_PIXELS, (column + 1) * TILE_PIXELS))
        mask[area] = np.dstack([
            crack, sealant, patch, shade
        ]).__mul__(255).round().astype(np.uint8)
        normal[area] = normal_from_height(height)
    return mask, normal


def preview(mask, path):
    """A quick look: asphalt grey with the defects drawn in, one atlas tile per cell."""
    crack, sealant, patch = (mask[..., channel] / 255.0 for channel in range(3))
    grey = 0.45 * (1 - patch * 0.2) * (1 - crack * 0.75) * (1 - sealant * 0.8)
    Image.fromarray((grey * 255).astype(np.uint8)).resize((1024, 1024), Image.LANCZOS).save(path, quality=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", help="also write a small greyscale preview of the atlas here")
    args = parser.parse_args()
    out_dir = os.path.join(data_root.data_root(), "texturegen", "road_cracks")
    os.makedirs(out_dir, exist_ok=True)
    mask, normal = build_atlas()
    Image.fromarray(mask, "RGBA").save(os.path.join(out_dir, "T_RoadCracks_Mask.png"))
    Image.fromarray(normal, "RGB").save(os.path.join(out_dir, "T_RoadCracks_Normal.png"))
    if args.preview:
        preview(mask, args.preview)
    print(f"wrote {out_dir}")


if __name__ == "__main__":
    main()
