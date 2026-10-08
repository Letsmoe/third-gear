# Geodata report — Hamburg-Bergedorf / Vierlande

Compiled 2026-10-08 by a data-fetch agent. Re-run scripts in `Tools/geodata/` (uv venv, `.venv/bin/python -I`).

**Bbox (WGS84)**: lon 10.05–10.33, lat 53.40–53.53 (~18.8 × 14.8 km, 269 km²).
EPSG:25832: E 569599–588427, N 5917282–5932054.

## Files
| Path | Size | Content |
|---|---|---|
| `GeoData/osm/bergedorf.osm.pbf` | 6.7 MB | Clipped OSM (Hamburg + Schleswig-Holstein + Niedersachsen merged), 602k nodes, 102k ways, 764 relations (only multipolygon + restriction relations kept) |
| `GeoData/osm_stats.json` | 36 KB | All OSM statistics below |
| `GeoData/raw/osm_*/` | 52 / 152 / 484 MB | Geofabrik extracts HH / SH / NI (snapshot 2026-10-07) |
| `GeoData/raw/dgm1_hamburg/dgm1_hh_2022-04-30.zip`, `..._extracted/` | 1.3 GB each | Hamburg DGM1 2022: 880 GeoTIFF tiles 1×1 km, 1 m, float32, EPSG:25832, heights m NHN (DHHN2016); 212 tiles intersect the bbox. **NoData is -9999** (header claims -3.4e38) → filter `v > -1000` |
| `GeoData/raw/dgm1_niedersachsen/` | 518 MB | LGLN DGM1 COGs (via STAC `dgm.stac.lgln.niedersachsen.de`), 77 positions × vintages 2015/2025 → use `_2025`. NoData -9999. Some tiles overlap Hamburg: Hamburg takes priority |
| `GeoData/raw/copernicus_glo30/Copernicus_DSM_COG_10_N53_00_E010_00_DEM.tif` | 32 MB | GLO-30 fallback, EPSG:4326, ~30 m, EGM2008, **DSM** (trees/buildings bias high) |

## Coverage problem
The bbox is not all Hamburg. The north-east (Reinbek, Wentorf, Börnsen, Escheburg, Aumühle, Geesthacht; ~lon 10.17–10.33,
lat 53.46–53.53) is **Schleswig-Holstein**; the south-west strip south of the Elbe is **Niedersachsen**.
- 1 m terrain covers 79% of the bbox (Hamburg 56% + NI 23%). The SH part (21%, ~57 km²) only has GLO-30.
- Better SH source: LVermGeo SH DGM1 (1 m XYZ, `dgm1_32_<E>_<N>_1_sh`), only via interactive portal
  https://geodaten.schleswig-holstein.de/gaialight-sh/_apps/dladownload/dl-dgm1.html (~55–60 tiles, license reportedly CC BY 4.0, unverified).
- Heights: −3.2 … 52.9 m (mean 3.8). Vierlande marsh −1…+3 m; Bergedorf/Lohbrügge Geest 40–50 m; Sachsenwald (SH) up to 94 m.
- Dike break lines: WFS `geodienste.hamburg.de/wfs_gelaendemodelle_hamburg_bruchkanten` (or 25 MB GeoJSON).

## Optional datasets (not downloaded)
- **LoD2 buildings (Hamburg only)**: https://daten-hamburg.de/opendata/3d_stadtmodell_lod2/LoD2-DE_HH_2026-04-28.zip (660 MB). LoD1 2023: 306 MB.
- **DOP20 aerial imagery (20 cm, 2022 RGBI)**: per district, e.g.
  https://daten-hamburg.de/geographie_geologie_geobasisdaten/digitale_orthophotos/DOP20/DOP20_HH-Bergedorf_fruehjahrsbefliegung_2022_rgbi.zip (5.1 GB).
  Same pattern for Harburg (4.3 GB), Hamburg-Mitte (3.8 GB), Wandsbek (5.1 GB). WMS at geodienste.hamburg.de.

## Licences / attribution (must appear in credits)
- Hamburg data (DGM, LoD2, DOP): dl-de/by-2.0 — "Freie und Hansestadt Hamburg, Landesbetrieb Geoinformation und Vermessung (LGV), <year>"; note modifications.
- Niedersachsen DGM1: CC BY 4.0 — "LGLN (2025), CC BY 4.0".
- Copernicus GLO-30: "Contains modified Copernicus data. © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018, provided under COPERNICUS by the European Union and ESA; all rights reserved".
- OSM: ODbL 1.0 — "© OpenStreetMap contributors" (derived databases stay ODbL).

## OSM analysis
### Road network
Drivable (motorway…living_street + service + links, excl. track): 13,375 ways / 1,468 km, of which service 615 km →
**public network ≈ 853 km**. Tracks +416 km (Vierlande farm roads, often access-restricted).

| highway | ways | km | | highway | ways | km |
|---|---|---|---|---|---|---|
| service | 8335 | 615 | | secondary | 616 | 69 |
| residential | 2125 | 381 | | living_street | 463 | 47.5 |
| track | 1667 | 416 | | unclassified | 257 | 80 |
| tertiary | 903 | 183 | | primary | 247 | 22 |
| motorway | 151 | 49.7 | | motorway_link | 184 | 15.8 |

Roundabouts: 62 ways + 2 mini-roundabouts.

Tag coverage (% of ways):
| class | maxspeed | lanes | width | oneway | surface | sidewalk | turn:lanes | lit |
|---|---|---|---|---|---|---|---|---|
| motorway | 99 | 100 | 0 | 100 | 100 | 0 | 36 | 99 |
| primary | 100 | 97 | 0 | 62 | 100 | 60 | 35 | 88 |
| secondary | 95 | 90 | 2 | 57 | 100 | 47 | 29 | 72 |
| tertiary | 95 | 69 | 2 | 32 | 100 | 52 | 12 | 52 |
| unclassified | 54 | 30 | 6 | 24 | 84 | 27 | 3 | 32 |
| residential | 63 | 12 | 15 | 18 | 83 | 35 | 1 | 52 |
| living_street | 2 | 0 | 8 | 5 | 71 | 2 | 0 | 38 |

- **width is practically absent → defaults per road class.** Lanes on residential mostly missing → default.
- maxspeed values: 50 (1740 ways / 282 km), 30 (1297 / 257 km), 60, 70, 20, 10, 120 (39 km), 80, 100, none.
  `maxspeed:type`/`source:maxspeed`: DE:urban, DE:zone30, sign, DE:rural → fill gaps with urban 50 / zone 30 / rural 100 defaults.
- surface: asphalt 4544, paving_stones 1479, sett 120, cobblestone 77, concrete 79, compacted 97.
- Sidewalks are mostly separate footway ways (7007 ways, 442 km). cycleway: track 110 (+318 via :left/:right), lane 30.
- lane_markings=no on 638 ways (narrow rural roads).

### Point features (inside bbox)
traffic_signals 284 (50% with `traffic_signals:direction`), stop 57, give_way 83, crossings 816
(uncontrolled 283, traffic_signals 254, zebra 29, …), street_lamp 620 (sparse; mostly `lit=yes` on ways),
bus_stop 1080, turning_circle 391, railway level_crossing 188, traffic_calming ~76, gates 1185, bollards 265,
fire hydrants 2414, city_limit signs 67, maxspeed sign nodes 42, natural=tree 29,177, tree_row 178 ways (19 km), hedges 1215.

Bridges: ~230 non-track highway ways (+95 track, +353 foot/path, 105 rail). Road tunnels ~70 (mostly service/garages), 857 culverts.

Turn restrictions: 372 (only_straight_on 133, no_u_turn 126, only_right 34, no_left 34, only_left 20, no_right 20, no_straight 5).

### Buildings
53,866 polygons, footprint 11.1 km², mean 207 m². building:levels 24%, roof:shape 18%, height 0.1%.
Types: yes 26162, house 7205, detached 4803, allotment_house 3957, apartments 3444, residential 1599, semidetached 1373,
garage 1087, greenhouse 780, shed 636, garages 416, industrial 303, farm_auxiliary 269, terrace 256, carport 222.
→ Heights need defaults by type (LoD2 could fill the Hamburg part).

### Land cover
landuse: farmland 74 km², forest 69, meadow 62, residential 44, industrial 10, commercial 4, allotments 3.6,
greenhouse_horticulture 2.6, orchard 1.6. natural: water 21 km², wood 8, scrub 7.7, wetland 6.5.
Waterways ≈ 1,050 km — drain 748 km, ditch 114, river 100, stream 53, canal 36 → **needs a size/LOD threshold**.
Power lines 99 km, parking areas 1360, silos 121.

## Implications for the importer
- Terrain: mosaic HH DGM1 > NI DGM1 > GLO-30 (blend at borders); consider SH DGM1 manual download later.
- Road geometry: widths/lanes from class defaults; maxspeed defaults from `maxspeed:type`/urban context.
- The ditch network is huge — only model canals/rivers and ditches near roads.
- Building heights mostly default by type.
