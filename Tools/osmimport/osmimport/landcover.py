"""Ground cover per terrain vertex from OSM land use, encoded as vertex colour blend weights for M_TerrainMaster:
R = meadow / rough grass, G = field soil, B = forest floor; all zero = mown lawn. A is unused (255).
"""
import numpy as np
import shapely
from rasterio import features as rfeatures
from rasterio.transform import from_origin
from scipy import ndimage

LAWN, MEADOW, FIELD, FOREST = 0, 1, 2, 3

# (tag key, value) -> class; first match wins, so more specific entries come first
RULES = [
    (("natural", "wood"), FOREST), (("landuse", "forest"), FOREST), (("natural", "scrub"), FOREST),
    (("landuse", "farmland"), FIELD), (("landuse", "greenhouse_horticulture"), FIELD),
    (("landuse", "plant_nursery"), FIELD), (("landuse", "construction"), FIELD), (("landuse", "brownfield"), FIELD),
    (("landuse", "meadow"), MEADOW), (("landuse", "grass"), MEADOW), (("natural", "grassland"), MEADOW),
    (("natural", "wetland"), MEADOW), (("natural", "heath"), MEADOW), (("landuse", "orchard"), MEADOW),
    (("landuse", "farmyard"), MEADOW), (("landuse", "railway"), MEADOW),
]


def classify(tags):
    for (k, v), cls in RULES:
        if tags.get(k) == v:
            return cls
    return None


def _rasterize(geoms, x0, y0, res, shape):
    if not geoms:
        return np.zeros(shape, dtype=bool)
    transform = from_origin(x0, -y0, res, res)  # world y grows south; raster rows too -> flip y
    flipped = [shapely.transform(g, lambda c: c * np.array([1.0, -1.0])) for g in geoms]
    return rfeatures.rasterize([(g, 1) for g in flipped], out_shape=shape, transform=transform, fill=0,
                               dtype="uint8", all_touched=False).astype(bool)


GREEN_CROP_SHARE = 0.6  # early summer: most fields are green (grain, maize, grassland), the rest bare soil


class LandCover:
    def __init__(self, osm_areas):
        self.by_class = {MEADOW: [], FIELD: [], FOREST: []}
        for osm_id, tags, geom in osm_areas:
            cls = classify(tags)
            if cls == FIELD and tags.get("landuse") == "farmland" and (osm_id * 2654435761 % 1000) / 1000 < GREEN_CROP_SHARE:
                cls = MEADOW
            if cls is not None:
                self.by_class[cls].append(geom)

    def weights(self, xs, ys, res, canopy=None, blur_m=3.0):
        """Blend weights (len(ys), len(xs), 3) for a regular vertex grid (xs, ys = vertex coordinates)."""
        shape = (len(ys), len(xs))
        x0, y0 = xs[0] - res / 2, ys[0] - res / 2
        box = shapely.box(xs[0] - res, ys[0] - res, xs[-1] + res, ys[-1] + res)
        out = np.zeros(shape + (3,), dtype=np.float32)
        for i, cls in enumerate((MEADOW, FIELD, FOREST)):
            geoms = [g.intersection(box) for g in self.by_class[cls] if g.intersects(box)]
            out[..., i] = _rasterize([g for g in geoms if not g.is_empty], x0, y0, res, shape)
        if canopy is not None:
            gx, gy = np.meshgrid(xs, ys)
            under_trees = np.clip((canopy.sample(gx, gy) - 2.0) / 4.0, 0, 1) * 0.7  # leaf litter under crowns
            out[..., 2] = np.maximum(out[..., 2], under_trees)
        sigma = blur_m / res
        if sigma > 0.3:
            for i in range(3):
                out[..., i] = ndimage.gaussian_filter(out[..., i], sigma)
        return np.clip(out, 0, 1)


def to_colors(w):
    """(..., 3) weights -> (n, 4) uint8 vertex colours."""
    w = w.reshape(-1, 3)
    return np.column_stack([np.round(w * 255).astype(np.uint8), np.full(len(w), 255, np.uint8)])
