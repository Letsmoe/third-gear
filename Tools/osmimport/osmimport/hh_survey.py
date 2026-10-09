"""Hamburg's surveyed street data (Tools/geodata/fetch_hh_street_survey.py) in world metres, the measuring stick for
the street model: it is never an input of the game build (issue #91).

Areas are grouped by what they are for the comparison: the carriageway kerb to kerb (painted cycle and bus lanes
included), parking beside it, cycle tracks, footways, green areas (islands, medians, verges) and shared spaces, where
road and footway are one surface and no kerb can be compared.
"""
import json
import os
from dataclasses import dataclass, field

import numpy as np
import shapely
from pyproj import Transformer

from .geo import Area

AREA_GROUPS = {
    "carriageway": {"Fahrbahn", "Busbuchten", "Radfahrstreifen", "Schutzstreifen", "Bussonderstreifen",
                    "Protected Bike Lane"},
    "parking": {"Parkplätze Kfz"},
    "cycle": {"Radweg", "gemeinsamer Geh- und Radweg", "Radfahrstreifen", "Schutzstreifen", "Protected Bike Lane"},
    "footway": {"Gehweg", "Gehweg, Radverkehr frei", "Fußgängerzone", "Haltestelle", "Treppe", "Überfahrt"},
    "green": {"Grünfläche"},
    "shared": {"Mischverkehrsfläche", "Verkehrsberuhigter Bereich"},
}
LAMP_KINDS = {"Lampe", "Hängelampe"}
SIGNAL_KINDS = {"Verkehrsampel"}
KERB_KINDS = {"Bordstein", "Hochbord", "Tiefbord"}
# Sign numbers that say nothing about driving and are left out: street name signs.
IGNORED_SIGNS = {"437"}

_wgs_to_utm = Transformer.from_crs(4326, 25832, always_xy=True)


@dataclass
class Survey:
    """The survey in world metres. areas: (group, usage, surface, polygon); points: (kind, x, y);
    signs: (sign number, x, y, facing azimuth in degrees from north, pole id); kerbs: lines."""
    areas: list = field(default_factory=list)
    kerbs: list = field(default_factory=list)
    points: list = field(default_factory=list)
    signs: list = field(default_factory=list)
    area_tree: object = None
    kerb_tree: object = None
    point_tree: object = None
    sign_tree: object = None

    def build_indexes(self):
        """Spatial indexes for the viewport queries."""
        self.area_tree = shapely.STRtree([polygon for *_, polygon in self.areas])
        self.kerb_tree = shapely.STRtree(self.kerbs)
        self.point_tree = shapely.STRtree([shapely.Point(x, y) for _, x, y in self.points])
        self.sign_tree = shapely.STRtree([shapely.Point(x, y) for _, x, y, _, _ in self.signs])

    def areas_in(self, box, group=None) -> list:
        """The areas touching the box, of one group or all."""
        found = [self.areas[index] for index in self.area_tree.query(box)]
        return [area for area in found if group is None or area[0] == group]

    def group_union(self, box, group):
        """The union of one area group, clipped to the box."""
        polygons = [polygon for *_, polygon in self.areas_in(box, group)]
        if not polygons:
            return shapely.Polygon()
        return shapely.intersection(shapely.union_all(polygons), box)

    def coverage(self, box):
        """Where the survey maps the street space at all, clipped to the box."""
        polygons = [polygon for *_, polygon in self.areas_in(box)]
        if not polygons:
            return shapely.Polygon()
        return shapely.intersection(shapely.union_all(polygons), box)

    def kerbs_in(self, box) -> list:
        return [self.kerbs[index] for index in self.kerb_tree.query(box)]

    def points_in(self, box, kinds) -> list:
        return [self.points[index] for index in self.point_tree.query(box) if self.points[index][0] in kinds]

    def signs_in(self, box) -> list:
        return [self.signs[index] for index in self.sign_tree.query(box)]


def _to_world(area: Area):
    """A shapely transform function from longitude and latitude to world metres."""
    def convert(coordinates):
        east, north = _wgs_to_utm.transform(coordinates[:, 0], coordinates[:, 1])
        return np.column_stack([east - area.origin_e, area.origin_n - north])
    return convert


def _features(path: str) -> list:
    with open(path) as source:
        return json.load(source)["features"]


def _area_groups(usage: str) -> list:
    """The comparison groups a usage belongs to (painted cycle lanes are both carriageway and cycle)."""
    return [group for group, usages in AREA_GROUPS.items() if usage in usages]


def load(geodata_dir: str, area: Area) -> Survey:
    """The whole downloaded survey in the area's world frame, indexed; None when it was not downloaded."""
    folder = os.path.join(geodata_dir, "hh_street_survey")
    if not os.path.isdir(folder):
        return None
    convert = _to_world(area)
    survey = Survey()
    for feature in _features(os.path.join(folder, "street_areas.geojson")):
        properties = feature["properties"]
        if properties.get("ebene_nr", 0) != 0:
            continue  # bridge decks and underpasses above or below the ground level
        polygon = shapely.make_valid(shapely.transform(shapely.geometry.shape(feature["geometry"]), convert))
        for group in _area_groups(properties.get("nutzung")):
            survey.areas.append((group, properties.get("nutzung"), properties.get("inhalt"), polygon))
    for feature in _features(os.path.join(folder, "street_lines.geojson")):
        if feature["properties"].get("inhalt") in KERB_KINDS:
            survey.kerbs.append(shapely.transform(shapely.geometry.shape(feature["geometry"]), convert))
    for feature in _features(os.path.join(folder, "street_points.geojson")):
        kind = feature["properties"].get("inhalt")
        if kind in LAMP_KINDS | SIGNAL_KINDS:
            x, y = convert(np.array([feature["geometry"]["coordinates"][:2]]))[0]
            survey.points.append(("lamp" if kind in LAMP_KINDS else "signal", float(x), float(y)))
    for feature in _features(os.path.join(folder, "traffic_signs.geojson")):
        properties = feature["properties"]
        number = str(properties.get("vz_nr"))
        if number in IGNORED_SIGNS:
            continue
        x, y = convert(np.array([feature["geometry"]["coordinates"][:2]]))[0]
        survey.signs.append((number, float(x), float(y), properties.get("azimut"), properties.get("aufstell_id")))
    survey.build_indexes()
    return survey
