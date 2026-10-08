"""Top-down 2D preview of an area's road/pavement/building geometry (debug aid).
  .venv/bin/python -I preview_2d.py <area> <out.png>"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import shapely
from shapely.plotting import plot_polygon, plot_line
from osmimport import dem, geo, osm, roads

area = geo.AREAS[sys.argv[1]]
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "GeoData")
data = osm.load(os.path.join(root, "osm", "bergedorf.osm.pbf"), area)
heights = dem.build_mosaic(area, root)
bu = shapely.union_all([g for _, _, g in data.buildings])
net = roads.build(data, heights, bu)
fig, ax = plt.subplots(figsize=(14, 14), dpi=110)
ax.imshow(heights.z, extent=(heights.x0, heights.x0 + heights.z.shape[1], heights.y0 + heights.z.shape[0], heights.y0), cmap="terrain", alpha=0.5)
colors = {"asphalt": "#444", "pavers": "#a66", "cobble": "#963"}
for kind, poly in net.surfaces.items():
    plot_polygon(poly, ax=ax, add_points=False, facecolor=colors[kind], edgecolor="none")
plot_polygon(net.pavement, ax=ax, add_points=False, facecolor="#bbb", edgecolor="#777", linewidth=0.3)
plot_polygon(bu, ax=ax, add_points=False, facecolor="#c84", edgecolor="#622", linewidth=0.3)
for _, poly in net.bridges:
    plot_polygon(poly, ax=ax, add_points=False, facecolor="#06c", edgecolor="none")
for kind, line in net.markings:
    plot_line(line, ax=ax, add_points=False, color="white", linewidth=0.6)
s = __import__("build_area").find_start(net)
ax.plot([s["x"]], [s["y"]], "r*", markersize=14)
ax.set_xlim(area.x_min, area.x_max); ax.set_ylim(area.y_max, area.y_min); ax.set_aspect("equal")
ax.set_title(f"{area.name}: x east, y south (m); red star = start")
fig.savefig(sys.argv[2], bbox_inches="tight")
