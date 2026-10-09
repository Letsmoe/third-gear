"""What the game generates for the streets, for the street viewer: road surfaces, pavements, markings with their dashes,
zebra bars, stop lines, and the signal poles, signs and lamps, made by the same code build_world.py runs.

roads.build gets slow on big areas (57 s for 1.5 km), so the viewer generates 500 m tiles, each from its OSM data with
100 m of context around it, and keeps only what lies inside the tile. Near tile edges this can differ slightly from
a whole-region build: a junction cut by the context edge sees fewer arms.
"""
from dataclasses import dataclass, field

import shapely
from shapely import ops

from build_area import MARKING_STYLE
from build_world import STOP_LINE_WIDTH, ZEBRA_BAR_WIDTH, stop_line_geometry, zebra_bars, zebra_carriageway
from osmimport import furniture, roads, street_layers

TILE_SIZE = 500.0
CONTEXT_MARGIN = 100.0
SURFACE_STYLES = {"asphalt": "game_asphalt", "pavers": "game_pavers", "cobble": "game_cobble"}


@dataclass
class GameStreets:
    """The game's street geometry inside one rectangle (world metres)."""
    surfaces: list = field(default_factory=list)   # (kind, polygon): asphalt, pavers, cobble on the ground
    bridges: list = field(default_factory=list)    # (way centre line, deck polygon)
    pavement: object = None                        # polygon of the pavements beside the roads
    markings: list = field(default_factory=list)   # (kind, polygon) painted white: dashes, edges, zebra bars, stop lines
    poles: list = field(default_factory=list)      # {"kind", "x", "y", "label"}: signal heads, signs, lamps

    def extend(self, other: "GameStreets"):
        """Adds another tile's geometry."""
        self.surfaces += other.surfaces
        self.bridges += other.bridges
        self.markings += other.markings
        self.poles += other.poles
        if other.pavement is not None and not other.pavement.is_empty:
            self.pavement = other.pavement if self.pavement is None else shapely.union(self.pavement, other.pavement)


def dash_pieces(line: shapely.LineString, dash):
    """The painted pieces of a marking line: the whole line, or (on, off) dashes along it."""
    if dash is None:
        return [line]
    on, off = dash
    pieces = []
    start = 0.0
    while start < line.length:
        piece = ops.substring(line, start, min(start + on, line.length))
        if isinstance(piece, shapely.LineString) and piece.length > 0.05:
            pieces.append(piece)
        start += on + off
    return pieces


def marking_polygons(net, zebra_cutout):
    """(kind, polygon) for every painted piece of the lane and edge lines, with the zebra crossings left free."""
    result = []
    for kind, line in net.markings:
        width, dash = MARKING_STYLE[kind]
        remaining = line if zebra_cutout is None else line.difference(zebra_cutout)
        for part in getattr(remaining, "geoms", [remaining]):
            if not isinstance(part, shapely.LineString) or part.is_empty:
                continue
            for piece in dash_pieces(part, dash):
                result.append((kind, shapely.buffer(piece, width / 2, cap_style="flat")))
    return result


def zebra_and_stop_polygons(builder, traffic, carriageway):
    """(kind, polygon) for the zebra bars and the stop lines at signals."""
    result = []
    for zebra in builder.zebras:
        bars, _, _ = zebra_bars(zebra, carriageway)
        result += [("zebra", shapely.buffer(bar, ZEBRA_BAR_WIDTH / 2, cap_style="flat")) for bar in bars]
    for junction in traffic["junctions"]:
        for approach in junction["approaches"]:
            line = stop_line_geometry(approach)
            if line is not None:
                result.append(("stop_line", shapely.buffer(line, STOP_LINE_WIDTH / 2, cap_style="flat")))
    return result


def furniture_poles(builder):
    """The generated signal heads, signs and lamps as {"kind", "x", "y", "label"}."""
    poles = [{"kind": "signal_head", "x": head["x"], "y": head["y"], "label": f"signal ({head['side']})"}
             for head in builder.heads]
    poles += [{"kind": "sign", "x": sign["x"], "y": sign["y"], "label": " + ".join(sign["names"])}
              for sign in builder.signs]
    poles += [{"kind": "lamp", "x": lamp["x"], "y": lamp["y"], "label": f"lamp ({lamp['source']})"}
              for lamp in builder.lamps]
    return poles


def _clipped(geometry, box):
    """The part of a geometry inside the box, or None when nothing is left."""
    clipped = shapely.intersection(geometry, box)
    if clipped.is_empty:
        return None
    return clipped


def generate(data, heights, core_box) -> GameStreets:
    """The game's streets inside core_box, from OSM data and terrain that reach CONTEXT_MARGIN beyond it."""
    building_union = shapely.union_all([geometry for _, _, geometry in data.buildings]) if data.buildings else None
    net = roads.build(data, heights, building_union)
    builder = furniture.FurnitureBuilder(data, net, building_union)
    traffic = builder.build()
    carriageway = zebra_carriageway(net)
    zebra_cutout = None
    if builder.zebras:
        zebra_cutout = shapely.union_all([zebra_bars(zebra, carriageway)[1] for zebra in builder.zebras])

    result = GameStreets()
    for kind, polygon in net.surfaces.items():
        clipped = _clipped(polygon, core_box)
        if clipped is not None:
            result.surfaces.append((kind, clipped))
    for way, polygon in net.bridges:
        clipped = _clipped(polygon, core_box)
        if clipped is not None:
            result.bridges.append((shapely.LineString(way.xy), clipped))
    result.pavement = _clipped(net.pavement, core_box)
    for kind, polygon in marking_polygons(net, zebra_cutout) + zebra_and_stop_polygons(builder, traffic, carriageway):
        clipped = _clipped(polygon, core_box)
        if clipped is not None:
            result.markings.append((kind, clipped))
    result.poles = [pole for pole in furniture_poles(builder) if core_box.contains(shapely.Point(pole["x"], pole["y"]))]
    return result


def map_layers(streets: GameStreets) -> street_layers.Layers:
    """The game's streets as viewer layers."""
    layers = street_layers.Layers()
    for kind, polygon in streets.surfaces:
        layers.add("Game road surface", polygon, SURFACE_STYLES[kind], {"kind": f"game {kind} surface", "osm_id": "",
                                                                        "tags": {}, "derived": {}})
    for line, polygon in streets.bridges:
        layers.add("Game road surface", polygon, "game_bridge", {"kind": "game bridge deck", "osm_id": "", "tags": {},
                                                                 "derived": {"length_m": round(line.length, 1)}})
    if streets.pavement is not None:
        layers.add("Game road surface", streets.pavement, "game_pavement",
                   {"kind": "game pavement", "osm_id": "", "tags": {}, "derived": {}})
    for kind, polygon in streets.markings:
        layers.add("Game markings", polygon, "game_marking", {"kind": f"game marking: {kind}", "osm_id": "",
                                                              "tags": {}, "derived": {}})
    for pole in streets.poles:
        layers.add("Game furniture", shapely.Point(pole["x"], pole["y"]), f"game_{pole['kind']}",
                   {"kind": f"game {pole['kind']}", "osm_id": "", "tags": {}, "derived": {"what": pole["label"]},
                    "label": pole["label"] if pole["kind"] == "sign" else None})
    return layers
