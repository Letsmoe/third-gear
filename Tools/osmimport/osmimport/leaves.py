"""Where fallen leaves collect: a deterministic settling simulation on the region's 1 m vertex grid.

The result is the depth of the leaf layer at the peak of leaf fall, in half centimetres (u8, 255 is reserved for "no
field": surfaces without data keep the material's old noise pattern). At runtime the material scales it by today's
fallen-leaf amount from the weather, so this is computed once per world and the season only changes a factor.

Model, per step on a raster of mass (cm of loose leaf layer, area weighted):
  1. Sources: every deciduous tree and hedge puts its crown's share down under the crown, heaviest below the middle.
  2. Wind: a share of each cell's mass moves downwind (west-southwest wind, same direction as the snow drift in
     WorldSnow.cpp), by how loose the surface is: grass holds leaves, pavement lets them skate, tarmac is fastest.
  3. Traffic: on the carriageway, mass moves towards the nearest kerb, hard in the lane and not in the gutter.
  4. Barriers: buildings, hedges and water take no inflow, so mass stops against them. A kerb stops mass coming from
     the road; the other way round is open.
Afterwards the wheel tracks of the carriageway are cleared (a car passes every few minutes), mass is lightly blurred
and converted to depth.
"""
import numpy as np
import shapely
from rasterio import features as rfeatures
from rasterio.transform import from_origin
from scipy import ndimage

STEPS = 70
WIND_TOWARDS = np.array([0.94, -0.34])  # unit vector x east, y south; blowing east-northeast (from west-southwest)
LEAF_DEPTH_UNIT_CM = 0.5                # u8 step of the stored depth
MAX_CODE = 254
# Mean layer depth in cm under a crown at the peak of leaf fall (2 to 3 leaves thick), per model.
CROWN_DEPTH_CM = {"broadleaf": 1.4, "shrub": 0.5}
HEDGE_HALF_WIDTH = 0.5
PILE_MAX_CM = 12.0  # a heap deeper than this slumps and compacts; mass above it is dropped
MOBILITY = {"lawn": 0.05, "paved": 0.35, "road": 0.5}
SWEEP_IN_LANE = 0.30
SWEEP_IN_GUTTER_METRES = 0.8
TRACK_CLEARED = 0.95
TRACK_CLEAR_FROM, TRACK_CLEAR_TO = 0.8, 1.8


def _raster(geometries, x_origin, y_origin, shape):
    """Boolean raster of geometries, one cell per metre, cell (row, column) centred on (x_origin + column, y_origin + row)."""
    geometries = [g for g in geometries if g is not None and not g.is_empty]
    if not geometries:
        return np.zeros(shape, dtype=bool)
    transform = from_origin(x_origin - 0.5, -(y_origin - 0.5), 1.0, 1.0)
    flipped = [shapely.transform(g, lambda c: c * np.array([1.0, -1.0])) for g in geometries]
    return rfeatures.rasterize([(g, 1) for g in flipped], out_shape=shape, transform=transform, fill=0,
                               dtype="uint8").astype(bool)


class LeafField:
    """Settled leaf depth over a region; see the module docstring."""

    def __init__(self, area, net, surfaces, building_union, plants):
        self.x_min, self.y_min = area.x_min, area.y_min
        nx = int(round(area.x_max - area.x_min)) + 1
        ny = int(round(area.y_max - area.y_min)) + 1
        self.shape = (ny, nx)
        self._classify(net, surfaces, building_union, plants)
        self._distance_to_kerb()
        self.mass = self._sources(plants)

    def _classify(self, net, surfaces, building_union, plants):
        """Surface class rasters: carriageway, pavement and paths, buildings, hedges and water."""
        raster = lambda geoms: _raster(geoms, self.x_min, self.y_min, self.shape)  # noqa: E731
        roadway = shapely.union_all(list(net.surfaces.values())).difference(net.pavement)
        self.road = raster([roadway])
        self.paved = raster([net.pavement, surfaces.paved]) & ~self.road
        self.building = raster([building_union])
        hedges = [shapely.Point(p.x, p.y).buffer(HEDGE_HALF_WIDTH) for p in plants if p.source == "hedge"]
        self.hedge = raster(hedges) & ~self.building
        self.water = raster([wb.polygon for wb in surfaces.water])
        self.solid = self.building | self.hedge | self.water
        self.mobility = np.where(self.road, MOBILITY["road"], np.where(self.paved, MOBILITY["paved"], MOBILITY["lawn"]))
        self.mobility[self.solid] = 0.0

    def _distance_to_kerb(self):
        """Metres from every carriageway cell to the nearest cell that is not carriageway, and the direction there."""
        self.kerb_distance = ndimage.distance_transform_edt(self.road)
        gradient_y, gradient_x = np.gradient(ndimage.gaussian_filter(self.kerb_distance, 1.0))
        length = np.maximum(np.hypot(gradient_x, gradient_y), 1e-6)
        self.towards_kerb_x = -gradient_x / length
        self.towards_kerb_y = -gradient_y / length

    def _sources(self, plants):
        """Leaves dropped under each deciduous crown: a Gaussian blob out to 1.3 crown radii with the crown's total."""
        mass = np.zeros(self.shape)
        for plant in plants:
            depth = CROWN_DEPTH_CM.get(plant.model)
            if depth is None or plant.crown < 0.5:
                continue
            self._drop(mass, plant, depth)
        mass[self.solid] = 0.0
        return mass

    def _drop(self, mass, plant, depth_cm):
        """Adds one crown's leaves to the mass raster."""
        radius = plant.crown / 2.0
        reach = radius * 1.3
        column = plant.x - self.x_min
        row = plant.y - self.y_min
        column_low, column_high = int(max(column - reach, 0)), int(min(column + reach + 1, self.shape[1] - 1))
        row_low, row_high = int(max(row - reach, 0)), int(min(row + reach + 1, self.shape[0] - 1))
        if column_high <= column_low or row_high <= row_low:
            return
        columns, rows = np.meshgrid(np.arange(column_low, column_high + 1), np.arange(row_low, row_high + 1))
        squared = ((columns - column) ** 2 + (rows - row) ** 2) / (0.55 * radius) ** 2
        weight = np.exp(-squared) * (squared <= (reach / (0.55 * radius)) ** 2)
        total = depth_cm * np.pi * radius ** 2
        if weight.sum() > 0:
            mass[row_low:row_high + 1, column_low:column_high + 1] += weight * (total / weight.sum())

    def _flux(self, mass):
        """Mass leaving each cell this step as (to_east, to_west, to_south, to_north) shares of the cell's mass."""
        sweep = np.where(self.road, np.where(self.kerb_distance > SWEEP_IN_GUTTER_METRES, SWEEP_IN_LANE, 0.0), 0.0)
        velocity_x = WIND_TOWARDS[0] * self.mobility + self.towards_kerb_x * sweep
        velocity_y = WIND_TOWARDS[1] * self.mobility + self.towards_kerb_y * sweep
        return velocity_x, velocity_y

    def _blocked(self, destination_solid, destination_paved, source_road):
        """Moves that cannot happen: into a solid cell, or from the carriageway up onto the pavement."""
        return destination_solid | (destination_paved & source_road)

    def settle(self):
        """Runs the steps; returns the settled mass raster."""
        mass = self.mass.copy()
        velocity_x, velocity_y = self._flux(mass)
        share_east, share_west = np.clip(velocity_x, 0, 1), np.clip(-velocity_x, 0, 1)
        share_south, share_north = np.clip(velocity_y, 0, 1), np.clip(-velocity_y, 0, 1)
        moves = []
        for share, shift in ((share_east, (0, 1)), (share_west, (0, -1)), (share_south, (1, 0)), (share_north, (-1, 0))):
            destination_solid = np.roll(self.solid, (-shift[0], -shift[1]), (0, 1))
            destination_paved = np.roll(self.paved, (-shift[0], -shift[1]), (0, 1))
            allowed = ~self._blocked(destination_solid, destination_paved, self.road)
            moves.append((np.where(allowed, share, 0.0), shift))
        for _ in range(STEPS):
            leaving = np.zeros(self.shape)
            arriving = np.zeros(self.shape)
            for share, shift in moves:
                moved = mass * share
                leaving += moved
                arriving += np.roll(moved, shift, (0, 1))
            mass = mass - leaving + arriving
            mass[self.water] = 0.0
            np.minimum(mass, PILE_MAX_CM, out=mass)
        return mass

    def depth_codes(self):
        """The settled field as u8 half centimetres of depth, after clearing the wheel tracks and a light blur."""
        mass = self.settle()
        tracks = np.clip((self.kerb_distance - TRACK_CLEAR_FROM) / (TRACK_CLEAR_TO - TRACK_CLEAR_FROM), 0, 1)
        mass = mass * np.where(self.road, 1.0 - TRACK_CLEARED * tracks, 1.0)
        mass = ndimage.gaussian_filter(mass, 0.6)
        mass[self.solid] = 0.0
        self.depth_cm = mass
        return np.clip(np.round(mass / LEAF_DEPTH_UNIT_CM), 0, MAX_CODE).astype(np.uint8)

    def tile_codes(self, x0, y0, nx, ny):
        """Slice of the u8 field for a tile vertex grid starting at world (x0, y0)."""
        column = int(round(x0 - self.x_min))
        row = int(round(y0 - self.y_min))
        return self.codes[row:row + ny, column:column + nx]

    def compute(self):
        """Settles the leaves and keeps the u8 field for tile_codes."""
        self.codes = self.depth_codes()
        return self.codes

    def save_debug(self, path, plants):
        """Writes the field and the masks it was made from for preview_leaves.py (not read by the game)."""
        trees = np.array([[p.x, p.y, p.crown, p.model == "shrub"] for p in plants if p.model in CROWN_DEPTH_CM])
        np.savez_compressed(path, depth_cm=self.depth_cm.astype(np.float16), road=self.road, paved=self.paved,
                            building=self.building, hedge=self.hedge, trees=trees,
                            origin=np.array([self.x_min, self.y_min]))
