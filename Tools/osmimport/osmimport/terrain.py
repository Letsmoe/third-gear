"""Terrain heights conformed to roads and pavements, and terrain mesh tiles."""
import numpy as np
import shapely
from rasterio import features as rfeatures
from rasterio.transform import from_origin
from scipy import ndimage

from .dem import HeightGrid
from .mesh import MeshBuilder
from .roads import KERB_HEIGHT, RoadNetwork

UNDER_ROAD = 0.10      # terrain sits this far below the road surface under roads
EDGE_DROP = 0.03       # at the road/pavement edge, terrain meets the surface just below it
BLEND_DISTANCE = 5.0   # metres over which terrain returns to the natural DEM


def _rasterize(dem: HeightGrid, geom):
    h, w = dem.z.shape
    if geom.is_empty:
        return np.zeros((h, w), dtype=bool)
    transform = from_origin(dem.x0, -dem.y0, dem.res, dem.res)
    flipped = shapely.transform(geom, lambda c: c * np.array([1.0, -1.0]))
    return rfeatures.rasterize([(flipped, 1)], out_shape=(h, w), transform=transform, fill=0, dtype="uint8",
                               all_touched=True).astype(bool)


def conform(dem: HeightGrid, roads: RoadNetwork, water=()) -> HeightGrid:
    """Natural DEM, pressed under roads and raised under pavements, blended over BLEND_DISTANCE;
    water bodies (paths.WaterBody) get a bed that deepens away from the bank."""
    natural = ndimage.gaussian_filter(dem.z.astype(np.float64), 0.7)  # take the edge off 1 m laser noise
    for body in water:
        inside = _rasterize(dem, body.polygon)
        if not inside.any():
            continue
        dist = ndimage.distance_transform_edt(inside, sampling=dem.res)
        depth = np.clip(0.3 + dist * 0.4, 0.3, 2.0)
        natural = np.where(inside, np.minimum(natural, body.level - depth), natural)
    road_z = roads.height.z.astype(np.float64)

    road_mask = _rasterize(dem, roads.ground)
    pave_mask = _rasterize(dem, roads.pavement) & ~road_mask
    target = np.full(dem.z.shape, np.nan)
    target[road_mask] = road_z[road_mask] - UNDER_ROAD
    target[pave_mask] = road_z[pave_mask] + KERB_HEIGHT - UNDER_ROAD
    surfaced = road_mask | pave_mask
    if not surfaced.any():
        return HeightGrid(natural.astype(np.float32), dem.x0, dem.y0, dem.res)

    # Outside: blend from the nearest surfaced edge height to natural ground.
    dist, (ri, ci) = ndimage.distance_transform_edt(~surfaced, sampling=dem.res, return_indices=True)
    edge_z = np.where(road_mask[ri, ci], road_z[ri, ci] - EDGE_DROP, road_z[ri, ci] + KERB_HEIGHT - EDGE_DROP)
    t = np.clip(dist / BLEND_DISTANCE, 0.0, 1.0)
    t = t * t * (3 - 2 * t)  # smoothstep
    out = edge_z * (1 - t) + natural * t
    out[surfaced] = target[surfaced]
    return HeightGrid(out.astype(np.float32), dem.x0, dem.y0, dem.res)


def build_tile(builder: MeshBuilder, terrain: HeightGrid, bounds, cell=1.0, cover=None, canopy=None,
               section="Terrain_Grass"):
    """Regular grid mesh over bounds (shares edge vertices with neighbouring tiles).
    cover: landcover.LandCover -> vertex colour blend weights; canopy: HeightGrid of vegetation height."""
    x0, y0, x1, y1 = bounds
    xs = np.arange(x0, x1 + 1e-6, cell)
    ys = np.arange(y0, y1 + 1e-6, cell)
    gx, gy = np.meshgrid(xs, ys)
    z = terrain.sample(gx.ravel(), gy.ravel())
    positions = np.column_stack([gx.ravel(), gy.ravel(), z])
    nx, ny = len(xs), len(ys)
    r, c = np.meshgrid(np.arange(ny - 1), np.arange(nx - 1), indexing="ij")
    a = (r * nx + c).ravel()
    b = a + 1
    d = a + nx
    e = d + 1
    # (a, b, e) in raw coords is CCW (y grows south) -> 'up' winding is (a, e, b) & (a, d, e)... see mesh._ccw_up
    tris = np.concatenate([np.stack([a, e, b], 1), np.stack([a, d, e], 1)])
    uvs = positions[:, :2].copy()
    colors = None
    if cover is not None:
        from .landcover import to_colors
        colors = to_colors(cover.weights(xs, ys, cell, canopy))
    builder.add(section, positions, uvs, tris, colors)
