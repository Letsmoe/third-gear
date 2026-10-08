# Third Gear

A VR driving simulator for everyday driving on real roads, built in Unreal Engine 5.8. The map is Hamburg-Bergedorf and the Vierlande, generated from OpenStreetMap and Hamburg's open geodata: terrain from the 1 m elevation model, roads with junctions and markings, buildings on their real footprints, and every street tree from the city's tree register. The car is a manual with a clutch you can stall, driven with a Logitech G923 wheel, pedals and H-shifter, with force feedback computed from the tyre forces.

It runs on Linux. VR goes through OpenXR and SteamVR, tested with a Quest 3 over Steam Link.

## Setup

Most content isn't in this repository: some is third-party, and most is generated from data that is too big for git. Each piece is fetched or rebuilt as follows.

1. Install Unreal Engine 5.8.1. The scripts assume `/mnt/storage/UnrealEngine/5.8.1`; set `UE` to change that.
2. Run `Scripts/setup_openxr_plugin.sh`. It copies the engine's OpenXR plugin into the project and applies `Patches/openxr-swapchain-flags.patch`, without which VR hangs on the third frame with SteamVR on Linux.
3. Build the editor target, as described in `CLAUDE.md` under "Build & run".
4. Download the textures with the scripts in `Tools/asset_fetch/` (Poly Haven and ambientCG, all CC0) and import them with `Scripts/import_textures.py`.
5. Add the free **City Sample Vehicles** pack to your Fab library, then fetch it with `Tools/asset_fetch/fab_download.py "City Sample Vehicles" <dir>` and move its `Content/CitySampleVehicles` into `Content/`.
6. Bake the trees with `Scripts/bake_vegetation.py`.
7. Fetch the geodata as described in `GeoData/REPORT.md`, then build and import a map with `Tools/osmimport/build_area.py`, `Scripts/create_materials.py` and `Scripts/import_osm_area.py`.

`CLAUDE.md` documents the whole project in detail: the map pipeline, the car physics, the wheel setup and the tools.

## Data

Map data © OpenStreetMap contributors, under the ODbL. Elevation, surface model and street trees © Freie und Hansestadt Hamburg (LGV), under the Datenlizenz Deutschland Namensnennung 2.0. Elevation for Lower Saxony © LGLN. Textures from Poly Haven and ambientCG under CC0.
