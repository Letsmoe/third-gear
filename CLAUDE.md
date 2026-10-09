# Third Gear — realistic VR road-driving sim

Purpose: practice everyday road driving (driving-school style) in VR on real Hamburg roads.
Racing is a secondary goal. Driving feel and visuals are what matter. Crash damage (deformation) is wanted later,
after the graphics are polished. Public repo: `Letsmoe/third-gear` (the Unreal project is still called `DrivingGame`).

## Git

Commit the worktree first if it's dirty, then write your changes. Short, to-the-point commit title; a body explaining the change when the title doesn't carry it; ask if you're unsure what to write. On a feature branch, only commit what that branch is for.

No trailers, ever: no `Co-Authored-By` on a commit, no "Generated with Claude Code" on a PR.

The repo is public. Never commit engine source code (Epic's licence forbids publishing it; our engine fixes live in `Patches/` as small diffs), Fab or Epic sample content, credentials, or large generated data. `.gitignore` lists what stays local, and README.md "Setup" says how each piece is fetched or regenerated.

## Writing

Commits and issues carry only what matters. Say the thing, explain what a reader won't see for themselves, stop. No restating the diff, no summarising what you just said, no section that exists because the format seemed to want one.

Write issues and their comments the way you'd explain it to a colleague, in complete sentences.

- Start with a 2–3 sentence summary: what happened, why, and the fix.
- Use short headings, with short paragraphs of normal prose under them.
- Never join ideas with arrows, slashes, colons or dashes. Write "first X, then Y" instead of "X → Y", and "A and B" instead of "A / B".
- Use at most two code identifiers per sentence. Say what each one does the first time it appears.
- Use bullets only for genuinely separate items, and make each bullet a full sentence.
- Use tables only for numbers or timelines.
- Use one date format everywhere: 2026-10-07 18:28.
- Put error messages, paths and commands in code formatting, and long logs in a collapsed `<details>` block.
- Put side findings in a short "Out of scope" section at the end.

A good bug issue says what you did, what happened, and what you expected, in that order, in a few lines. The screenshot carries the rest. No severity theatre, no restating the obvious, no speculation about the fix unless you read the code and know.

## How we work

One agent at a time, with me giving feedback as it goes. Keep each step small enough for me to read in one sitting, and stop to show me rather than piling up work I then have to catch up on.

`main` is what I've reviewed and what I run the game from. It stays checked out in the repo root: never switch branches there.

Straight to `main` in the root, no branch: typos, one-liners, and anything that only touches how we work rather than the game — this file, `.claude/`, editor config.

Everything else happens on a branch off `main`, in a worktree under `.worktrees/<branch>`, created with `Scripts/new_worktree.sh <branch>`. Plain `git worktree add` isn't enough here: most content is generated or third-party and not in git, and the script links it in from the root. Those links are shared, so regenerating content in a worktree changes it for the root too. Build the worktree before opening the editor there. Name the branch `<issue-number>-<slug>` when there is an issue (`12-lawn-colour`), `<slug>` when there isn't. One branch is one thing; anything found along the way gets written down as an issue and stays off the branch, unless the branch can't finish without it.

Before starting on anything, check whether it is already half-built: `git branch` and `git worktree list` for the feature, and read what is on the branch. Sessions end mid-feature, and a branch is where that work is — starting again writes it a second time and loses whatever the first attempt learned. If a branch for it exists, continue on it.

No pull requests unless I ask for one. Review happens here, on the diff, before the merge.

## Issues

Issues on `Letsmoe/third-gear` carry goals across sessions. A session ends and its context goes with it; an issue is the only thing that carries a goal to the next one. So:

- Work that finishes in this session, with me here, needs no issue.
- Work that won't finish in one session gets one, however loosely defined it still is.
- Something concrete found along the way, that isn't what we're doing now, gets one instead of being done.

Write them as soon as the list exists, not once the work starts. The open issues are the roadmap; the "Roadmap" section below is only the big picture.

Write to GitHub as `Letsmoe` with `gh`; if another account is active, run `gh auth switch --user Letsmoe` first. `git push` always authenticates as Letsmoe, because the remote URL names the user. No "written by Claude" line in the text.

Labels are two axes. Type is GitHub's default `bug` or `enhancement`. Area is exactly one of:

- `area:car` — vehicle physics, drivetrain, the car model and its interior
- `area:input` — wheel, pedals, shifter, force feedback
- `area:world` — the OSM pipeline: terrain, roads, buildings, vegetation, horizon
- `area:rendering` — lighting, sky, materials, post-processing, GPU performance
- `area:vr` — OpenXR, SteamVR, the headset and stereo rendering
- `area:traffic` — signals, signs, rule checking, AI traffic
- `area:tooling` — scripts, asset downloads, repo setup

Two areas is fine when an issue genuinely spans them; three means split it.

## Done means verified

Work is done when it has been shown to work, not when the code is written. Shown means one of:

- screenshots from `Scripts/screenshot.sh` or `-SeatShot`, from after the change and from before it when it's visual;
- `Scripts/drive_test.sh` numbers for anything that touches the car, before and after;
- `Scripts/profile_gpu.sh` timings for anything that might cost frame time.

Then hand it over and stop: what changed in a sentence or two, the evidence, and the branch. When it has an issue, the evidence also goes on the issue as a comment — that comment is what I read, so it's written for someone who wasn't in the session. GitHub has no API for attaching images, so screenshots are committed to the `screenshots` branch under `<issue-number>/` and embedded by their raw URL (`https://raw.githubusercontent.com/Letsmoe/third-gear/screenshots/<issue-number>/<file>.png`).

## Merging

I review the branch's diff, and it merges into `main` when I say so — never before, and never without evidence. Merge in the root with `git merge --no-ff <branch>`, so the branch stays one unit in the history; if it conflicts, resolve it in the merge commit. Push `main`. Then remove the worktree and delete the branch, and close its issue with `gh issue close <n>`.

C++ changes need a rebuild in the root after the merge, with the editor closed; tell me when one does.

## Validation

Test on something tiny: the 500 m region `bergedorf_test` (`-Region=bergedorf_test` on the `Streamed` map, the default), so a test round takes minutes, not an hour. `bergedorf_core` is for final checks and performance measurements.

Every Unreal run and ComfyUI job takes the machine-wide gpu lock and every build the build lock (`Scripts/lock.sh`; the test scripts take it themselves), so parallel worktrees and agents take turns instead of running out of memory.

Never launch or restart my running editor, game or VR session. Check things yourself headless or offscreen instead: `Scripts/screenshot.sh`, `-SeatShot`, `Scripts/drive_test.sh`, `Scripts/profile_gpu.sh`, and the logs under `Saved/Logs/`. Only I can test with the headset and the real wheel; when that's needed, say exactly what to try.

When I tell you what's happening, that's the reproduction: take it as given and go find the cause, don't re-check what I already saw. Reach for screenshots and logs when the code doesn't make the cause clear, or a fix based on reading it didn't work — guessing from source has been wrong often enough that a second guess isn't worth it.

## Code style

Readability and maintainability over cleverness. Match the surrounding code when it conflicts with the rules below; Unreal C++ follows Epic's naming (`PascalCase`, `F`/`U`/`A` prefixes), Python follows PEP 8.

- Give every function a doc comment saying what it does, so it reads clearly at the call site (`/** … */` in C++, a docstring in Python). The signature carries the types — don't restate them.
- Comments are technical, concise, and for the reader who arrives later. Add them where skimming the code isn't enough — not to restate it. Don't document a feature you just removed.
- Descriptive names, never shortened: `WheelRadiusCm`, `crown_diameter`, not `wr`, `cd`.
- Explicit control flow over shorthand: avoid compact conditionals unless they clearly save a lot of repetition.
- More than 2 levels of nesting is too much — use guard clauses, early returns, or extracted helpers.
- Short, focused functions, one responsibility each. Prefer a named helper over long inline logic with a comment above it.
- Don't change behaviour, public APIs, data shapes, validation or side effects unless asked.

## Hardware / environment
- Linux (CachyOS), fish shell. RTX 4080 Super (16 GB).
- Unreal Engine **5.8.1**, native Linux build: `/mnt/storage/UnrealEngine/5.8.1` (`$UE` below).
- VR: Meta Quest 3 streamed via **Steam Link** (SteamVR "vrlink" driver; ALVR also installed) → **SteamVR** (active OpenXR runtime).
  Stream quality is fine; visual issues are ours to solve. SteamVR must be running BEFORE the editor starts
  (OpenXR instance is created only at startup; otherwise VR Preview is greyed out).
- Input: **Logitech G923** wheel + pedals (with clutch) + **Driving Force Shifter** (H-pattern).
  Kernel driver `hid_logitech_new` (new-lg4ff) is loaded → force feedback via Linux evdev FF (`FF_CONSTANT`, etc.).
  Logitech's G SDK is Windows-only; UE has no wheel FFB on Linux, so we read/write evdev directly.

## Decisions (from the user)
- **Visuals are a top priority** (main reason for not using BeamNG): long view distance (≥1 km, not ~100 m), high-res textures,
  good mirrors, real night lighting (street lamps, headlights), rain with wet roads and reflections.
  → Deferred renderer + Lumen + Virtual Shadow Maps, World Partition + HLODs, TSR. **Not** forward/MSAA.
  Mirrors are SceneCaptures with reduced resolution, update rate and show flags.
- **Driving feel**: realistic acceleration and gearing (BeamNG complaint: 50 km/h in 2nd gear, too much acceleration,
  50 km/h doesn't *feel* like 50). Speed feel comes from correct 1:1 scale and eye height, near-field ground detail, audio, FFB.
- **Car**: use a **stock car asset** (Fab / Epic sample content; the user must add it, it needs an Epic login).
  Only the physics values are ours: Golf-class compact (~110 kW, ~1300 kg, 0–100 in ~9 s, 6-speed manual;
  50 km/h ≈ 3rd/4th gear). Manual gearbox with clutch, stalling, H-shifter.
- **Physics**: start with Chaos Vehicles (tuned realistically); replace with a custom tire model (Pacejka) and drivetrain
  if the feel isn't good enough. FFB is derived from tire forces (self-aligning torque).
- **Map**: real roads imported **from OpenStreetMap**, area **Hamburg-Bergedorf + Vierlande** (town + countryside).
  Terrain from Hamburg open data (DGM1). Buildings: **stock assets placed on OSM footprints** for now; real Hamburg
  LoD2 building shapes possibly later. Right-hand traffic, German rules and signs.
- **Traffic + rule checking**: AI traffic (lanes, lights, right of way). The game detects violations
  (red light, speeding; later right of way, stop signs). No "instructor" for mirrors or indicators needed.
- No MCP for Unreal for now: work via C++, config .ini files, and headless editor Python scripts.

## Roadmap
0. Project skeleton, OpenXR, Lumen; measure VR performance through ALVR.
1. Wheel/pedals/shifter input + force feedback (evdev C++ plugin).
2. Stock car with realistic engine/gearbox/weight; manual + clutch.
3. OSM import: Python preprocessing → Unreal C++ builds terrain, roads + junctions, markings, kerbs/pavements,
   buildings on footprints, vegetation (PCG). Start with central Bergedorf, grow outward.
4. Traffic lights, signs, speed limits from OSM; red-light/speeding detection.
5. AI traffic.
6. Atmosphere polish: night lighting, rain/wet roads/wipers, mirrors.
7. Crash damage (deformable car body) — after the graphics polish.

## Layout
- `DrivingGame.uproject`, `Source/DrivingGame/` — main game module (C++).
- `Config/` — engine/game config (renderer settings live in `DefaultEngine.ini`).
- `Plugins/WheelInput/` — evdev wheel/pedals/shifter input + constant-force FFB on a ~1 kHz thread
  (`UWheelInputSubsystem`: `GetState()`, `SetSteeringForce()` +1 = clockwise, `SetSteeringResistance(damping, friction)`
  applied at 1 kHz from the measured wheel speed, `SetWheelRange()`; force fades out if not refreshed for 0.25 s).
  Axis codes, pedal inversion, shifter/button indices and force sign are in `UWheelInputSettings` (DefaultGame.ini),
  measured on the real G923 (wheel 0x00, throttle 0x02, brake 0x05, clutch 0x01, gears 1–6 = buttons 12–17, R = 18,
  force inverted). `BrakeFullTravel` 0.75: the G923 brake's rubber stop is too stiff to press to the end.
- `Scripts/` — headless editor Python scripts (level/asset creation). `create_proving_ground.py` builds `/Game/Maps/ProvingGround`.
- `Tools/wheeltest/` — standalone C diagnostic: `./wheeltest list|monitor|ffb` (axes, button indices, force direction).
- **Data root** `/mnt/storage/third-gear` (`$THIRD_GEAR_DATA` overrides): everything big or third-party that isn't in
  git — `downloads/`, `geodata/` (osm/, raw/), `world/<region>/` (streamed tiles), `raw_assets/`, `venvs/` (osmimport,
  fab; uv, Python 3.13), `unreal/` (CitySampleVehicles, Textures, Vegetation, World content), `ddc/`, `building_kit/`.
  `Tools/bootstrap/data_root.py` gives each checkout ignored links into it (`External`, the `Content/` folders,
  `DerivedDataCache`, `Tools/osmimport/.venv`); `new_worktree.sh` calls it. Python finds paths via `data_root.*_dir()`.
- `Tools/bootstrap/bootstrap.py [--status | <step> ...]` — sets up a fresh machine step by step (links, downloads,
  Python envs, geodata, OpenXR, build, car, textures, materials, trees, world); each step checks its own output.
- `Tools/asset_fetch/` — CC0 photoreal textures (with height maps), models, HDRIs, German sign SVGs into
  `<data root>/raw_assets`; see `Data/raw_assets.md`.
  `Tools/asset_fetch/fab_download.py "<title>" <out> [--list]` downloads UE asset packs from the user's **Fab library**
  (no Epic launcher on Linux): uses the Epic login stored by Heroic (`~/.config/heroic/legendaryConfig/legendary/user.json`,
  expires after ~1 day → log in again in Heroic) and `legendary-gl` (data root venv `fab`). Output = `Content/<Pack>/…`.
- `Tools/geodata/` — OSM + terrain sources for the Bergedorf/Vierlande bbox, see `GeoData/REPORT.md`;
  `Tools/bootstrap/prepare_geodata.py` clips and unpacks them into `<data root>/geodata` (DGM1, bDOM, street trees).
- `Tools/buildingkit/` — building pieces generated in Blender (four styles) + Hunyuan 3D props; output in
  `<data root>/building_kit`, see its README.
- Isobar weather plugin: separate local repo `/home/moritz/Documents/personal/isobar` (shared with massif, private),
  loaded via `AdditionalPluginDirectories`. `UWeatherSubsystem` (`Source/DrivingGame/`) runs it for the map;
  console `Weather.Print`, `Weather.Skip <hours>`.
- `Plugins/MapRuntime/` — the streamed world (see "Map pipeline"), `AVegetationActor` + material shader includes.
- `Source/DrivingGameEditor/` — editor-only C++: `UDgMeshImporter` (.dgmesh → Nanite mesh), `UVegetationAssetTools`.

- `Plugins/OpenXR/` — **patched copy of the engine OpenXR plugin** (project plugins override engine ones), not in git:
  `Scripts/setup_openxr_plugin.sh` creates it from the engine plus `Patches/openxr-swapchain-flags.patch`.
  Fix in `OpenXRHMD.cpp` `AllocateSwapchainTextures_RenderThread` (search `DRIVINGGAME PATCH`): the Vulkan path strips
  `TexCreate_Dynamic`, so the flag check never matched and the swapchain was recreated mid-frame → deadlock on frame 3
  with SteamVR on Linux. Re-run the script if the engine is upgraded.

## Map pipeline (OSM → Unreal)
**Streamed world (current).** `Tools/osmimport/.venv/bin/python -I Tools/osmimport/build_world.py <region> [--reuse]`
compiles OSM + DEM into 250 m `.tgtile` files in `<data root>/world/<region>/` (format: `osmimport/worldtile.py`,
zlib sections NAME, GRID, SURF, MARK, BLDG, VEGE; `world.json` = index + start pose). bergedorf_core: 64 tiles, 9 MB,
~3 min fresh, 22 s with `--reuse`. At runtime `AWorldStreamer` (`Plugins/MapRuntime`, placed in `/Game/Maps/Streamed`
by `Scripts/create_streamed_map.py`) meshes tiles on worker threads (`WorldTileMesher`) into `UDynamicMeshComponent`s
on `AWorldTileActor`s, in three detail levels (near < 400 m 1 m grid, middle < 1.2 km, far < 3 km), spawning in 2 ms
budgeted steps and enabling collision per ground chunk within 150 m. `-Region=<region>` picks the region
(default bergedorf_test). `Scripts/stream_test.sh [region] [route] [kmh]` flies through at driving speed and prints
frame times and per-tile build/spawn times (bergedorf_core at 100 km/h: median 5.8 ms, 99 % < 9.8 ms; the one
remaining ~45 ms hitch is garbage collection in the editor binary). Run: `env MAP=Streamed Scripts/run_desktop.sh
-Region=bergedorf_core`. Gotchas: a triangle (A, B, C) faces along `Cross(C-A, B-A)`; reserve arrays of
`FDynamicMesh3` (TArray relocates bitwise and breaks attribute parent pointers).

**Prebaked maps (older pipeline, still in the repo; materials and vegetation models are shared with the streamed world):**
1. `Tools/osmimport/.venv/bin/python -I Tools/osmimport/build_area.py <area>` (areas in `osmimport/geo.py`:
   `bergedorf_test` 500 m, `bergedorf_core` 2×2 km; world origin fixed at Bergedorf, x east / y south / z NHN metres)
   → `GeoData/build/<area>/tile_*.dgmesh` + `manifest.json` (start pose, points: signals, lamps, trees…).
   Modules: `osm.py` (load), `dem.py` (1 m DGM mosaic HH>NI>GLO-30), `roads.py` (widths, junction-shaped surfaces,
   smoothed road heights, pavements, markings), `terrain.py` (conformed terrain, 1 m mesh), `paths.py` (footpaths,
   pedestrian zones, water), `buildings.py` (extrusion, gabled/flat roofs), `mesh.py` (draping + .dgmesh format).
   `preview_2d.py <area> out.png` renders a top-down debug image.
2. `Scripts/create_materials.py` — `M_SurfaceMaster` + instances `M_<Section>` (textures from `/Game/Textures`,
   UVs in metres, procedural anti-aliased windows on facades, per-building tint via vertex colour). Updates in place.
3. `Scripts/import_osm_area.py <area>` — C++ `UDgMeshImporter` (module `DrivingGameEditor`) builds Nanite static meshes
   with complex-as-simple collision, then creates `/Game/Maps/<area>` with lighting from `Scripts/world_lighting.py`.
4. Check visually without a headset: `Scripts/screenshot.sh <map> "x,y,z,pitch,yaw;..."` → `Saved/Screenshots/shot_NN.png`
   (map = name under /Game/Maps or a full package path; env `CMDS="cvar 1,..."` adds console commands; first run after
   material/foliage changes is slow because shaders compile; env `EXTRA="-SpawnCar"` adds game arguments). Look around on the desktop: `MAP=<map> Scripts/run_desktop.sh`.
Lighting is physically based (sun 75 klx, exposure locked at EV100 13 via unbound PostProcessVolume; needs
`r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True`). Free-fly viewer (`-FreeCam`): WASD/QE, Shift = 50 km/h.
`build_area.py --reuse` reuses the cached OSM/DEM/road/terrain step (`cache.pkl`, saves ~2 min); `--skip-tiles` only
rewrites vegetation.json.

**Vegetation** (`osmimport/vegetation.py` → `vegetation.json`, placed by `Scripts/import_vegetation.py` as one
`AVegetationActor` (plugin `Plugins/MapRuntime`) with instanced static meshes + invisible trunk collision cylinders):
real street trees from the Hamburg Straßenbaumkataster (`GeoData/raw/strassenbaeume/`, WFS, species + crown diameter,
height measured in the bDOM), OSM trees / tree rows / hedges, and trees + bushes **detected in the Hamburg bDOM 2020**
(1 m image-matched surface model, `GeoData/raw/bdom_hamburg/tif/`, converted by `convert_bdom.py`; `osmimport/canopy.py`:
nDSM = bDOM − DGM outside buildings, local maxima = tree tops). Outside bDOM coverage: scattered trees in OSM woods/parks.
Models: scanned trees from the engine's Procedural Vegetation Editor samples (plugin `ProceduralVegetationEditor`),
baked by `Scripts/bake_vegetation.py` (C++ `UVegetationAssetTools`) from skinned to **static Nanite assemblies** in
`/Game/Vegetation` — the skinned (wind) versions crashed the GPU before `r.Nanite.Foliage=True` was set; static is also
cheaper. Model table + scaling in `Scripts/vegetation_models.py`. Wind sway is not there yet.

**Terrain material**: `M_TerrainMaster` (in `create_materials.py`), 4 layers blended by vertex colour from
`osmimport/landcover.py` (none = lawn, R meadow/green crops, G field soil, B forest floor, + leaf litter under tree
crowns), each sampled at two scales/rotations mixed by noise against tiling, macro brightness variation. HLSL helpers in
`Plugins/MapRuntime/Shaders/Private/TerrainNoise.ush` (mapped as `/Plugin/MapRuntime`). Lawn = rocky_terrain_02 (drone
scan); a real close-up lawn needs 3D grass later (no CC0 photo lawn texture exists).

**Horizon** (`Tools/osmimport/build_horizon.py` → `GeoData/build/horizon/`): low-detail terrain in 2 km tiles around the
detailed area out to ±13 km (10/20/40 m grid by distance, DGM inside the geodata bbox, GLO-30 outside, skirts against
cracks), OSM buildings, water, measured trees (bDOM) in the tiles touching the detailed area, extruded canopy
silhouettes (`Canopy_Far`) for woods further out, and a flat ground ring to 60 km. `import_osm_area.py <area>` adds it
automatically when its manifest's `detail` matches (skip with `-nohorizon`).

## Car & wheel
- **Classes** (`Source/DrivingGame/`): `ACarPawn` (default pawn: mesh + attached meshes from settings, seated HMD camera
  at `DriverEyeLocation`, speed/gear/rpm text in front of the driver, keyboard + wheel input, FFB);
  `UCarMovementComponent` (Chaos wheeled vehicle; Chaos keeps rigid body, suspension raycasts/springs, wheel anim data);
  its physics-thread `FCarVehicleSimulation` (in the .cpp) replaces Chaos' engine/gearbox/tyres/steering with
  `FCarDrivetrain` (`CarDrivetrain.*`, plain C++); `UCarSettings` (all assets + tuning); `CarSimTypes.h` (params,
  driver input, telemetry, GT↔PT exchange); `UCarWheelFront/Rear`; `ADriveTestRunner` (`DriveTest.*`).
- **Physics model** (every 2 ms physics step, 8 sub-steps): engine = full-load torque curve + friction/pumping drag,
  progressive pedal map, turbo lag (NA share instant, boost lags), idle/anti-stall PI controller, rev limiter, stall
  below 350 rpm, starter with clutch interlock, bump start. Clutch, engine friction and brakes are torque-limited
  couplings in a small sequential-impulse solver → progressive slip, lock-up, stalling and wheel locking emerge
  (clutch capacity = 380 Nm × engagement² between pedal 0.8 and 0.25). Gear only engages with the clutch pressed or
  revs matched (else "grind", stays neutral); reverse only when nearly stopped. Open diff, FWD, 92 % driveline
  efficiency, ABS. Tyres: simplified Pacejka with combined slip (friction ellipse, μx 1.1, μy 0.99, load sensitivity),
  semi-implicit wheel spin, low-speed clamps (parks without jitter). Suspension forces act along the ground normal
  (Chaos' own version braked the car whenever the body pitched); body never sleeps, no linear damping; aero drag
  0.5·ρ·CdA·v². Compliance steer (0.2°/kN) gives realistic understeer.
- **FFB**: kingpin torque = −(pneumatic trail, collapsing at the grip limit, + caster trail) × front lateral force,
  / steering ratio = rack torque at the wheel; minus EPS assist (85 % parked → 75 % fast, 0 with engine off) → hands torque;
  `FfbFullScaleNm` (6 Nm) = 100 % motor, capped at `FfbMaxForce` 0.8. Damping/friction (heavy when parked) run in the
  1 kHz wheel thread. No canned effects.
- **Tuning** lives in `Config/DefaultGame.ini` `[/Script/DrivingGame.CarSettings]` (documented in `CarSettings.h`);
  no rebuild needed. Golf VII 1.4 TSI 150 PS values: 1300 kg, 61 % front, 250 Nm 1500–3500, 110 kW at 5000–6000,
  MQ250 ratios 3.778/2.118/1.360/1.029/0.857/0.733, final 3.647, R 3.6, tyre radius 0.31 m, steering ratio 13.6 (900° wheel).
- **Test**: `Scripts/drive_test.sh` (headless, `-nullrhi -benchmark -fps=100 -DriveTest -NoWheel`, ~20 s) prints
  idle, 0-50/0-100/top speed with force balance, 50 km/h rpm per gear, clutch starts, stall, restart, reverse, grind,
  100-0 braking, ramp steer with steering torque. Measured: 0-100 9.2 s, top 217 km/h, 50 km/h = 3270/2130/1610/
  1340/1140 rpm in 2nd–6th, 100-0 38 m, 0.93 g lateral, idle clutch start ok, clutch dump at idle stalls.
- **Keys**: W/S throttle/brake, A/D steer, Left Shift clutch (released slowly), 1–6 gears, N neutral, B reverse,
  E start/stop (push-button), Space parking brake, Backspace put car back on its wheels, R recentre.
  With the wheel connected, pedals/wheel/H-shifter take over (keyboard still adds). `-FreeCam` = old free-fly pawn
  (`-Shots=` implies it); `-SpawnCar` also parks a car at the player start; `-SeatShot [-ShotName=x]` saves the
  driver's view. `-WheelDebug` logs every wheel axis/button change.
- **Car model**: City Sample Vehicles **vehicle07** (4.4 m sedan with full interior, closest to a Golf) from Fab, in
  `Content/CitySampleVehicles/` (UE 5.4 build, loads fine in 5.8). Structure of every City Sample car: skeletal mesh
  with only wheel bones (`wheel_front_turn_*` = Chaos steer+spin bones, `wheel_rear_*`; children `wheel_front_*` /
  `wheel_rear_no_spin_*` carry the brake calipers) + static meshes **modelled in car space** (`*_No_Wheel` body,
  `SM_All_Trans_*` glass, wheels, brake pads) → each needs `Location = -(bone position)` in `AttachedMeshes`;
  AnimBP `/Game/CitySampleVehicles/Rig/ABP_Vehicle`. Paint/colours come from the mesh material instances.
  The steering wheel is part of the body mesh and does not turn yet. Other cars (traffic later): 02 estate, 03 big
  sedan, 05 SUV, 06 sports coupé, 12 taxi, 13 police, plus vans/trucks/bus/trailer.
  `Scripts/inspect_citysample.py [vehicleNN_Car ...]` logs components, offsets, bones (prefix `CSV`);
  `Scripts/create_car_lineup.py` builds `/Game/Maps/CarLineup` (all cars in a row at y = 0, 4, 8… m).
  To swap: set `SkeletalMesh`, `AnimClass`, `AttachedMeshes` (`Location`/`Rotation` offsets), `WheelBones` (FL, FR, RL, RR),
  `VisualWheelRadiusCm`, `OriginHeightAboveGroundCm` (wheel radius − wheel bone height), `DriverEyeLocation`,
  `DashboardLocation`; check with `-SeatShot` and `EXTRA=-SpawnCar Scripts/screenshot.sh ProvingGround "5,4,1.5,-5,-141"`.
  Physics values stay. Old placeholder: `/Game/Vehicles/SportsCar`. `Scripts/vehicle_materials.py` creates the
  exposure-independent dashboard text material.
- **Wheel verification checklist** (wheel plugged in; `cd Tools/wheeltest`):
  1. `./wheeltest list` → G923 listed with `ffb=yes` (else: permissions / udev rule for `/dev/input/event*`).
  2. `./wheeltest monitor`: turn wheel → which axis changes (expect 0x00); press throttle, brake, clutch fully one at
     a time → note axis codes and whether the value goes up or down when pressed; put the shifter in 1–6 and R → note
     the button indices; pick buttons for start engine / parking brake / recentre / reset car.
  3. `./wheeltest ffb`: does the POSITIVE level pull the wheel RIGHT (clockwise)? If it pulls left: `bInvertForce=True`.
  4. Enter the values in `[/Script/WheelInput.WheelInputSettings]` (DefaultGame.ini): `SteeringAxis`, `ThrottleAxis`,
     `BrakeAxis`, `ClutchAxis`, `bInvert*` (True if the value DEcreases when pressed / turning right), `GearButtonIndices`,
     `ReverseButtonIndex`, `*ButtonIndex`. Check in-game with `-WheelDebug` (log prints the same numbering) and the
     dashboard (gear, rpm); turning right must steer right, and at speed the wheel must pull back to centre.

## Asset rules
- Everything must be **photorealistic** (scanned/photo-based); no stylized or low-poly assets.
- Surfaces use height maps for parallax occlusion mapping (default) or Nanite displacement (close-up cobblestones, kerbs).

## Build & run
Keep the editor **closed** while building (no Live Coding on Linux).
```fish
set UE /mnt/storage/UnrealEngine/5.8.1
# build editor target
$UE/Engine/Build/BatchFiles/Linux/Build.sh DrivingGameEditor Linux Development -project=(pwd)/DrivingGame.uproject -waitmutex
# run a headless editor Python script
$UE/Engine/Binaries/Linux/UnrealEditor-Cmd (pwd)/DrivingGame.uproject -run=pythonscript -script=(pwd)/Scripts/<script>.py -unattended -nosplash
# open editor
$UE/Engine/Binaries/Linux/UnrealEditor (pwd)/DrivingGame.uproject
```
In VR: start SteamVR (+ Steam Link) first, then `Scripts/run_vr.sh [screen%]` (standalone game).
Avoid the editor's VR Preview: editor + PIE + stereo exceeded VRAM (OOM crash). `Scripts/measure_vram.sh` measures VRAM offscreen (`-emulatestereo`).
Measured (empty map, 5056x2704 stereo): ~8.7 GB at 100%, ~6.0 GB at 70% screen percentage, Lumen ≈ 2.4 GB of that.
GPU profiling: `Scripts/profile_gpu.sh <label> "<cvars>"` (headless, emulated stereo, `-ProfileGPUAfter=N` in
`ADrivingGameMode`) + `python3 -I Scripts/parse_profilegpu.py Saved/Logs/gpu_<label>.log [depth] [min_ms]`.
Findings (empty map): VolumetricCloud ≈ 5 ms in stereo regardless of settings → removed (use HDRI / cached sky instead);
TSR history 200% → 100% saves ~1.2 ms; 85% screen percentage default in run_vr.sh. Scene ≈ 8 ms at 85%. `R` recenters the seated view.
Useful console commands: `stat fps`, `stat unit`, `stat gpu`, `r.ScreenPercentage`.

## Conventions
- C++ for systems (input, vehicle, import, traffic); avoid binary-only Blueprint logic so changes stay reviewable.
- Units: Unreal centimetres; OSM import projects WGS84 → UTM 32N (EPSG:25832, same as Hamburg open data) with a local origin.
