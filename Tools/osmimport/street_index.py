"""The whole OSM extract with spatial indexes, for the street viewer and the survey comparison: cuts out the roads,
paths, areas, points and buildings of any rectangle in world metres (x east, y south)."""
import os

import shapely

import data_root
from build_world import building_footprints
from osmimport import building_types, geo, osm

# The world frame of every region (same origin), wide enough to take the whole extract.
WHOLE_EXTRACT = geo.Area("whole_extract", geo.ORIGIN_E, geo.ORIGIN_N, -100000, 100000, -100000, 100000)


class ViewportIndex:
    """The loaded OSM data and its buildings with spatial indexes. Buildings are (osm_id, tags, footprint, type);
    the type is building_types' classification, or None when typing was skipped."""

    def __init__(self, data: osm.OsmData, type_buildings: bool = True):
        self.data = data
        footprints = list(building_footprints(data))
        types = [None] * len(footprints)
        if type_buildings:
            typer = building_types.BuildingTyper(footprints, data.roads, data.footways, data.areas)
            types = [typer.classify(index) for index in range(len(footprints))]
        self.buildings = [(osm_id, tags, footprint, building_type)
                          for (osm_id, tags, footprint), building_type in zip(footprints, types)]
        self.building_tree = shapely.STRtree([footprint for _, _, footprint in footprints])
        self.way_lists = {"roads": data.roads, "footways": data.footways, "railways": data.railways,
                          "waterways": data.waterways}
        self.way_trees = {name: shapely.STRtree([shapely.LineString(way.xy) for way in ways])
                          for name, ways in self.way_lists.items()}
        self.area_tree = shapely.STRtree([geometry for _, _, geometry in data.areas])
        self.point_tree = shapely.STRtree([shapely.Point(point.x, point.y) for point in data.points])

    def subset(self, box) -> osm.OsmData:
        """The roads, paths, railways, waterways, areas and points touching the world rectangle."""
        subset = osm.OsmData()
        for name, ways in self.way_lists.items():
            setattr(subset, name, [ways[index] for index in self.way_trees[name].query(box)])
        subset.areas = [self.data.areas[index] for index in self.area_tree.query(box)]
        subset.points = [self.data.points[index] for index in self.point_tree.query(box)]
        return subset

    def buildings_in(self, box) -> list:
        """The buildings touching the world rectangle."""
        return [self.buildings[index] for index in self.building_tree.query(box)]


def load_whole_extract(type_buildings: bool = True) -> ViewportIndex:
    """The index over the whole OSM extract (about 10 s, plus 15 s for typing the buildings)."""
    data = osm.load(os.path.join(data_root.geodata_dir(), "osm", "bergedorf.osm.pbf"), WHOLE_EXTRACT, margin=0.0)
    return ViewportIndex(data, type_buildings)
