# Building kit

Kit pieces for the runtime building generator (#23, kit tracked in #26). Structural pieces come from a parametric
generator written as Blender Python. Organic one-off pieces come from Hunyuan 3D 2.1 in a local ComfyUI. Everything
generated goes to `/mnt/storage/third-gear/building_kit/` and is not committed.

## Regenerate everything

```fish
Tools/buildingkit/build_all.sh
```

This builds all pieces, exports one GLB per piece and one `.blend` per style, and renders the contact sheets and
assembled test houses. Pass `--props` to also run the Hunyuan pipeline (it starts ComfyUI on 127.0.0.1:8189 and stops
it again). `blender -b --factory-startup -P Tools/buildingkit/generate.py -- <out> [style ...]` builds single styles.

## Conventions

These hold for every structural piece of every style, so pieces snap together without per-piece offsets.

- Units are metres, Z is up. The facade is seen from outside along +Y, X runs to the right.
- The wall face is the plane `Y = 0`. The wall body extends toward `+Y` by the wall thickness. Anything that sticks out
  of the wall (sills, cornices, balconies, gutters) sits at negative Y.
- The origin of a wall piece is the bottom-left corner of its wall face. Window units, sills, lintels, doors and other
  attachments are modelled in the frame of the wall bay they belong to, so they are placed with exactly the same
  transform as the bay and need no offset.
- The module width is 2.0 m for all styles. Wall bays are one module wide (`Wall_Half` is one metre). Heights are
  multiples of 0.25 m, so a texture that tiles per 0.25, 0.5 or 1 metre runs on from storey to storey without a seam.
- UVs are in world metres: 1 UV unit is 1 m, U to the right as seen from outside, V up the face. Horizontal faces
  use X and Y. Tiling materials therefore need no per-piece scale. UVs restart at 0 at each piece's own origin.
- Corner posts are `thickness` wide and `thickness` deep. `Corner_L` has its front on `Y = 0` for `x` in
  `[0, thickness]` and its side face on `X = 0`; `Corner_R` is its mirror image. A facade of n bays between two corners
  is `n * 2.0 + 2 * thickness` long.
- Horizontal bands (plinth, string course, cornice, floor band, roof edge, gutter) come as `_M`, `_Half`, `_CornerL`
  and `_CornerR`. The corner pieces cover the corner post plus the projection around it with mitred joints.
- Storeys stack: place the same pieces again at `z = storey_height * storey_index`. Bands are modelled at the top of the
  storey (cornice and string course just below `H`, plinth at the bottom, `farm` plinth below `z = 0`).
- Roof pieces are exported already tilted by the roof pitch: the origin is the lower-left corner of the visible
  surface, X runs along the eaves and Y up the slope in plan. Each plane piece covers 2.0 m along the eaves and 1.0 m
  along the slope (horizontal run `cos(pitch)`). The back slope is the front slope rotated 180 degrees about the
  building's vertical axis. Gable fill triangles are plain generated meshes and not kit pieces.
- GLB files are written Y-up as glTF requires. Check the axis mapping once in the Unreal importer (the facade front
  is glTF +Z).
- Material slots, named per surface: `Brick`, `Plaster`, `Timber`, `Frame`, `Glass`, `Sill`, `RoofTile`, `Thatch`,
  `Metal`, `Concrete`. Props add one slot of their own (`Plastic` for the wheelie bin). The GLBs carry flat preview
  colours only, one hue per slot.

## Structural pieces

Style dimensions (storey height, wall thickness, window reveal depth, roof pitch) are in `kit/styles.py`.

- `brick`: red clinker town house around 1900. Storey 3.25 m, wall 0.36 m, reveal 0.20 m, roof 45 degrees.
- `plaster`: plastered house of the 1950s and 60s. Storey 2.75 m, wall 0.30 m, reveal 0.15 m, roof 35 degrees.
- `block`: postwar block of the 1960s and 70s. Storey 2.75 m, wall 0.30 m, reveal 0.15 m, flat roof.
- `farm`: Vierlande half-timbered farmhouse. Storey 2.25 m, wall 0.25 m, reveal 0.15 m, thatch roof at 50 degrees.

Modules: `kit/geom.py` is the pure-Python mesh builder (boxes, prisms, sweeps with mitred corners, heightfields),
`kit/parts.py` has the components (wall bays with real openings, window and door units, sills, arches, bands,
gutters, roofs, dormers, chimney), `kit/styles.py` combines them per style, `kit/blender_io.py` turns meshes into
Blender objects and exports GLB. `assemble.py` builds one three-module house per style from the pieces and renders it.

### Assembling a building

The assembly test shows the placement rules. Front wall: bay `i` at `x = thickness + i * 2.0`. Right wall: rotate 90
degrees about Z, origin at `(width, thickness + i * 2.0)`. Back wall: rotate 180, origin at
`(width - thickness - i * 2.0, depth)`. Left wall: rotate -90, origin at `(0, depth - thickness - i * 2.0)`. Corners:
`Corner_L` at the front left, `Corner_R` at `(width - thickness, 0)`, `Corner_L` rotated 180 at `(width, depth)` and
`Corner_R` rotated 180 at `(thickness, depth)`.

## Structures on OSM nodes

`generate_structures.py [family ...]` builds the structures that stand on a node instead of a footprint. Each family
is a module in `kit/` with `build()` for the pieces and `assemblies()` for how its contact sheet shows them. For a
family it writes the GLBs to `glb/<family>/`, the models assembled in a row to `blend/<family>.blend`, and the contact
sheet as `renders/<family>_lineup.png` and `renders/<family>_closeups.png`.

### Wind turbines

`kit/turbines.py` (#100) has the four models in the Bergedorf extract (Senvion MM100, Nordex N117, Enercon E-92, NEG
Micon NM48), each as `<Model>_Tower`, `<Model>_Nacelle` and `<Model>_Rotor`.

The tower's origin is the centre of its foot on the ground, the nacelle's is on the yaw axis at the top flange, and
the rotor's is the hub centre with the rotor axis along Y, facing -Y. The rotor attaches to the nacelle at
`rotor_socket` and is tilted front end up by `rotor_tilt_degrees`, both written per model to
`kit_stats_turbines.json`; it turns clockwise as seen from the front. The new slots are `Paint`, `PaintRed` for the
blade tip marking, `PaintGreen` for Enercon's tower foot (the band colour follows the height, V of the metre UVs) and
`Beacon` for the obstruction lights.

### Power pylons

`kit/pylons.py` (#101) has the 110 kV lattice designs Donau (two arm levels) and one-level (one long arm), and the
spun concrete barrel pole (three arm levels, middle one widest) of the line through the Curslack wind farm, each as a
suspension and an anchor variant and each as one piece: `Donau_Suspension`, `Donau_Anchor`, `OneLevel_Suspension`,
`OneLevel_Anchor`, `Barrel_Suspension`, `Barrel_Anchor`. The origin is the centre of the foot, the line runs along Y and the arms along X. Every conductor
and the earth wire has a socket in `kit_stats_pylons.json`, where the runtime hangs the catenary: under the
insulator on suspension pylons, and at the end of the horizontal insulator on each side (`_Back` and `_Ahead`) on
anchor pylons. The slots are `Lattice` for the steel angles, `Insulator` for the silicone sheds, `Metal` for fittings
and `Concrete` for the foundations. The conductor stubs on the sheet are only a preview.

### Bus stops

`kit/shelters.py` (#102) has Hamburg's JCDecaux glass shelter in two and three bays (`Glass_2Bay`, `Glass_3Bay`:
slim black posts, a framed glass end, frameless back glass with clamps, the red dot band and the information case,
a CityLight advertising case at the left end, a thin glass roof that overhangs the kerb and rises toward it, a black
steel bench), the rural `Timber` shelter (boarded back and ends, pent roof) and the red `StopSign` mast as photographed
at Roßweg (the white stop panel beside its top, a red timetable case, and Hamburg's red street bin with its sticker).
A shelter's origin is the middle of its open front edge, which faces -Y toward the road; the mast's is its foot, and it
faces -Y too. The shelter follows a photo of the Rathausmarkt stop; the 1.5 m bay and the heights are estimates from
it. `PowderCoat` is the black steel.

### Posters and printed faces

The shelters and the mast carry printed faces with 0 to 1 UVs in their own slots: `AdPanel` (CityLight poster,
1160 by 1710 mm, both faces of the advertising case), `Timetable` (A-format, in the shelter's information case and the
mast's case), `StopSignFace` (the stop panel beside the mast top) and `BinSticker` (the joke on the street bin).
`posters/generate_ads.sh [name ...]` makes the adverts with Codex's image tool from `posters/prompts.py` (fictional
brands, German text) and fits them to the CityLight size with `posters/fit_poster.py`; `posters/draw_signs.py` draws
the stop panel, a timetable and the bin stickers with PIL, since their text has to be exact. Everything lands in
`<data root>/building_kit/posters/`, and the contact sheet shows it through `preview_textures()`.

### Greenhouses

`kit/greenhouses.py` (#103) is a footprint style, not a node structure: the pieces are meant to be placed along a
greenhouse footprint like walls, and the sheet shows whole houses assembled from them the way the runtime would. Two
glasshouse styles, the modern `Venlo` (4.5 m gutter, 3.2 m spans, 4 m bays, 1 m panes) and the low `VenloOld` (2.6 m
gutter, brick plinth, 3 m bays, small panes), each with `_Side`, `_Gable`, `_GableDoor` in the wall convention and
`_Roof`, `_RoofVent`, `_Gutter`, `_Post`, `_Column`, `_Truss` in the house frame (X across the spans, Y along the
ridges); `glasshouse_parts()` is the placement rule. The foil tunnel has `Tunnel_Section` and `Tunnel_End`. New slots:
`GreenhouseGlass` (whitewash and the plants inside belong in its material), `Foil` and `Aluminium`.

## Props from Hunyuan 3D

`props/run_props.sh` runs the whole chain. ComfyUI is started from a private venv in
`/mnt/storage/third-gear/building_kit/comfy/venv` (the venv in `~/comfy/ComfyUI` no longer exists), with custom nodes
disabled, and stopped afterwards.

1. `props/make_reference_images.py`: FLUX.2 klein makes one reference image per prop, BiRefNet removes the background
   and the cut-out is composited on mid grey. A white background made Hunyuan add a floor slab.
2. `props/make_meshes.py`: Hunyuan 3D 2.1 makes a shape-only mesh from each image.
3. `props/process_props.py` (Blender): rotates, scales to the measured size in `props/prop_list.py`, decimates, sets the
   origin, projects metre UVs and exports GLB. Ground props have the origin at the bottom centre with the front toward
   -Y. Wall props (door surround, window pediment) have the origin at the bottom-left of the wall face with the body
   toward -Y.

## Known rough spots

- Pieces carry flat preview colours only. Real materials (FLUX.2 klein tiles through Chord) still have to be made.
- The tile and thatch planes overlap their neighbour by up to a few centimetres when a roof depth is not a whole number
  of pieces; the assembly lifts the overlapping row by 1 cm. The runtime generator needs a rule for this.
- Gable fill triangles, eave closure at the ridge ends and hip roofs are not kit pieces.
- Window units are one style per building style; no shutters, blinds or interior fill yet.
- Props are shape only. The door surround looks more like a fireplace surround from the side, and the column's poster
  band is bare geometry.
