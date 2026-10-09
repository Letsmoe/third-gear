# Megascans shopping list (#46)

The exact Megascans assets on Fab to add to the Fab library, per surface and typology class (`typology.md`, #80).
Megascans are the scans for facade, roof, timber, window and decal materials; where Megascans has nothing that looks
like Hamburg, the last section says what we take or generate instead.

## How this list was checked, and what it does not prove

fab.com blocks scripted requests (a Cloudflare challenge answers 403), so no listing page could be opened. Every
entry below was found through a search restricted to fab.com on 2026-10-09, which returned the listing title, the
URL and the scan data (texel density, scan area, map set) from the listing. A title marked "category" appeared on a
Fab category page without its own URL in the results; search that title on fab.com. What is not verified: the price
(the category pages showed "Free" on some and "From $0.99" on others, and the prices of single entries were not
readable), whether the preview colour is the one we want, and whether a listing is Quixel's own. Several titles occur
twice with different ids (two "Brick Wall", two "Stucco Wall"); both are listed, and the checks at the end settle
which one to keep.

Fab lists Megascans in four quality tiers: Raw 8K, High 4K, Mid 2K, Low 1K. Take **High (4K)** for facades and roofs
(a 2 m scan at 4096 px/m is 4K per 2 m, which is the right density in VR at 1 m viewing distance and still affordable),
and Raw only for the few hero materials if memory allows.

URLs are `https://www.fab.com/listings/<id>`.

## Brick and clinker (typology: Gründerzeit, 1920s block, postwar, villas)

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Brick Facade | b2fe6bb7-d8c0-4e77-9ac9-9f243a5f4b70 | 2x2 m, 4096 px/m, with displacement | Clean modern brick veneer, regular bond; the best base for 1920s and 1950s brick blocks. |
| Brick Wall | b1b6e2f4-3e7f-4d90-803c-8cbb5e483961 | 2x1 m, 4096 px/m, displacement and AO | Main brick, hue-shifted red, brown and yellow in the material. |
| Brick Wall (second listing) | 36928d3a-4664-4f8d-9d44-be009164f2b6 | 2x1 m, 4096 px/m, tagged weathered | Old Gründerzeit brick. |
| Brick Wall Worn | 499d9ba7-f361-491c-b66e-c16c89bac34a | 2x1 m, 8192 px/m, no displacement | Worn facade for the oldest buildings and for close-up hero walls. |
| Dirty Brick Wall | 1e4abea1-da15-4915-9f7f-c028b8626c9a | 2x1 m, 4096 px/m, soot and chipped | Sooty industrial and railway-side brick; too dirty for housing. |
| Decorative Brick Wall | 3d687b58-bb57-4e1e-8432-b0a82085ad25 | 2x2 m, 4096 px/m | Patterned brick for cornices, string courses and brick ornament. |
| Red Brick | 15151b81-430c-4efd-9ef1-106794f0454c | single 3D brick, 1225 px/m | One brick as a close-up prop for broken walls and tilted brick courses; not a surface. |

Megascans does not have a Hamburg clinker as such (dark red-brown, slightly vitrified, with light mortar, plus the
yellow-brown Klinker of the Backsteinexpressionismus). Plan on hue shifting the above in `create_materials.py`, and
check the previews of the two Brick Wall listings for a deep red against the references in
`/mnt/storage/third-gear/building_kit/references/gruenderzeit_clinker`. The generated clinker was rejected in the
texture shootout, so the scans stay the base and the colour variation is a tint, not a new texture.

## Plaster and render (postwar, detached houses, modern, terraces)

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Stucco Wall | e2f69285-c695-4aa8-851c-1b6492b79b74 | 2x2 m, 4096 px/m; the Fab plaster category showed it as free | Rough scratch render; the standard 1950s to 1970s facade, tinted white, beige, light yellow and grey. |
| Stucco Wall (second listing) | 0e6cc564-e072-4289-9d35-41e650a089ac | 2x2 m, 4096 px/m, tagged clean and fresh | The clean variant for new buildings. |
| Stucco Facade | 9b8166a4-7cff-4af2-b3fd-537747e29895 | 2x2 m, 4096 px/m, tagged rough, decorative | Heavier rough render for terraces and farm outbuildings. |
| Plaster Wall | c8abaad6-05d4-4351-b537-7318df677968 | 2x2 m, 4096 px/m, cracked, peeling | Old repaired plaster; the weathering base for refurbished 1950s houses. |
| Damaged Wall Plaster | 363ea75c-8182-48c4-8c22-651551818b3d | 2x2 m, 4096 px/m, cracked, dirty | Plinth zones and gable walls where the render is failing. |
| Plaster Line | a671eb72-31ee-4253-9b6f-3e07ceac5e7b | 1x0.25 m, 16384 px/m, opacity | A trim and moulding piece, not a wall; for cornice relief. |
| Painted Wall Plaster | category | plaster category | Smooth painted wall, for white and pastel finishes. |
| Wall Paint | category | plaster category | Smooth flat paint; interior, and Neuallermöhe-style colour render. |
| Wall Plaster | category | plaster category | A second smooth plaster. |
| Flaked Paint Wall | category | plaster category | Flaking paint for the oldest weathered walls and for #84. |
| Painted Plaster | category | plaster category | Same family; compare previews before taking all of them. |

Browse `https://www.fab.com/category/material/building-human-made--plaster`. External insulation on refurbished
postwar blocks (a very smooth, slightly textured thin render, white or light pastel) is closest to Wall Paint or
Painted Wall Plaster with a very low normal strength.

## Concrete (slab blocks, garages, industrial halls, plinths)

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Precast Concrete Wall | 7d5ed180-d5aa-4d73-bee9-20c6d2dc7d52 | 2x2 m, 4096 px/m, panel joints, stained | The 1960s and 70s panel block (Lohbrügge-Nord, Bergedorf-West). |
| Weathered Concrete Wall | a76a34a2-07d6-4c1e-9313-01e0621c29c1 | 2x2 m, 4096 px/m, stain and seepage | Old garage and shed walls. |
| Smooth Concrete | 7f276aab-d74e-44ae-bd25-4dcf75ae6267 | smooth, stained | Clean concrete for modern buildings and plinths. |
| Wood Lined Concrete Wall | 6f9101f6-dac0-4ea5-a51e-39fda81fd736 | board-marked concrete | Modern and 1960s exposed concrete. |
| Bunker Concrete Wall | e9edfbf4-7212-4335-8213-5ebece8dfa7d | dirty, chipped | Industrial base walls and loading docks. |

## Roofs (clay, concrete, anthracite, slate, thatch)

Hamburg roofs are red-brown clay Pfannen (S-shaped interlocking tiles) on the pitched older houses, black or anthracite
engobed concrete tiles from the 1970s on, slate on the Gründerzeit mansards, thatch (Reet) on Vierlande farms, and
bitumen or gravel on flat roofs. Megascans is weak here.

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Roof Tiles | 60323c66-ff8a-4ac7-af91-7d501a993dd9 | 1x1 m, 8192 px/m, painted, smooth, clean, with displacement | The only clean modern tile. Tint it anthracite for concrete tiles and brown-red for clay. |
| Red Roof Tiles | category (tile category, from $0.99) | not read | The nearest to Hamburg clay Pfannen if the shape is right; check the preview. |
| Japanese Roof Tiles | 43bc2fbe-ff7e-419f-813c-b7b7f84eb249 | 2x2 m, 4096 px/m, weathered clay | Old clay with moss and dirt; the wrong profile (Japanese kawara), so use it only as a weathering overlay. |
| Slate Roof | category (roofing category, from $0.99) | not read | Gründerzeit mansard slate. |
| Weathered Wood Slate Roof | category | not read | Shingle slate for old roofs. |
| Thatch Roof | category (from $0.99) | not read | Vierlande Reetdach. |
| Painted Roof | category (free) | not read | Painted sheet or tile; check whether it is metal. |
| Wooden Roof Shingles | category (from $0.99) | not read | Chapels, sheds; rare in Hamburg. |
| Red Slate Tiles | f6ae65c1-ff58-44d5-a345-4d6ac451a9dd | 2x2 m, 8192 px/m | A floor-like slate; useful for dark Schiefer wall cladding on gables. |
| Roof Tile (3D) | cb26343f-045d-44dd-9366-ef580f62a182 | single tile 0.16 x 0.25 m | A close-up prop for ridge and hip details. |

Browse `https://www.fab.com/category/material/building-human-made--roofing`. The kit's roof pieces carry their own
tile relief, so the material needs colour, roughness and the tile edge shading more than the height.

## Timber and half-timbering (Vierlande farms, Sachsentor)

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Old Damaged Wood | 3ced280f-679d-48e3-9334-855c24496fb7 | 1x1 m, 8192 px/m | Dark oak beams of the Fachwerk; tinted brown-black or green-painted. |
| Wood Plank | 9dcd9635-47bd-4e61-a027-6b603db487d3 | 2x0.5 m, 8192 px/m, weathered | Shed cladding and barn boarding. |
| Painted Wooden Planks | category | wood category | Painted boarding and window shutters. |
| Old Wooden Beam (3D) | 298204f9-d5b1-4936-9bd8-6180418a81ad | 0.43 x 3.77 x 0.42 m | A beam to test the timber-frame look next to the procedural one. |
| Wooden Piece (3D) | be259385-f11d-42af-adfb-451207b7a2ca | 0.92 x 0.31 x 0.08 m | Beam, plank and fence prop. |
| Medieval Modular Wall | 896c5dee-7f27-4d68-b589-306597898883 | 0.16 x 2 x 2 m, tagged plaster, wood, window | The only timber-and-plaster wall piece; a reference for the infill (Gefach) look, not a kit part. |

The Vierlande brick infill between the beams is a normal brick or whitewashed plaster, taken from the lists above.

## Window frames, doors, sills

Megascans windows and doors are old and worn (Mediterranean and medieval), which is wrong for Hamburg's white-painted
timber casements and white plastic frames. Use them as reference and as hero props on the oldest buildings only.

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Old Wooden Window | a093a890-4f09-4575-adde-e316f16d7bb5 | 0.1 x 1.27 x 2.03 m, 6311 px/m, flaking paint | Weathered painted casement, as a paint-wear texture source for window frames. |
| Decorative Window Frame | 76d37737-505c-4693-99bc-eb71d667f646 | 0.32 x 1.44 x 2.34 m, closed mesh | Ornate Gründerzeit frame. |
| Modular Building Window | b7a9e369-6781-4a2e-aa12-2d42ec33e744 | 0.46 x 3.0 x 3.5 m | Reference for the reveal depth. |
| Modular Building Door and Window | f266a77b-4756-4ba4-a157-eea4579b8168 | 0.68 x 3.0 x 3.5 m | Door-and-window bay. |
| Modular Building Door | f449ef06-932b-4251-9795-8bfd4de9c3d5 | 0.75 x 3.0 x 3.5 m | Front door; the kit door is the better fit, so use this for the wood texture. |
| Modular Wooden Door | 70336b68-a5ed-4711-8944-bccb5df15eb7 | 0.37 x 1.5 x 4.0 m | Barn and farm doors. |
| Worn Door | 61c0ee97-6cea-4a0b-b1c5-28f91873e68d | decal-type, 1x2 m, 4096 px/m, with opacity | Door leaf as a texture. |

Sills and lintels come from the kit's stone sill and the stone entries in the plinth section below.

## Plinth and stone

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Stone Brick Wall | 4c4b7616-2f39-4da1-bd4e-9ca25820d676 | 2x2 m, 4096 px/m, mortar, worn | Plinth of Gründerzeit houses and farm foundations. |
| Rough Stone Wall | f45edf76-e624-4fef-ad13-0fdd29366ec2 | 2x2 m, 4096 px/m | Field-stone plinth and garden walls. |
| Castle Wall Stone (3D) | ca359e26-33e1-4400-90bb-b3f73325bb03 | 0.61 x 1.01 x 0.35 m | Quoin and cap stone prop. |
| Stone Pavement | e03d170f-98f8-4e88-9eb7-63909411375d | 2x2 m, 4096 px/m | Door step and entrance paving. |
| Cement Curbs | 3f4932b2-ab27-446b-9a33-b4fb1a7bc6b3 | 3D curb pieces | Concrete plinth edge and kerbs. |

## Metal cladding (sheds, industrial halls, garages)

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Corrugated Metal Sheet | 048a87bb-8228-459c-8827-3c00da9e9151 | 2x2 m, 4096 px/m, painted, factory | Industrial hall walls and roofs; tint grey, white and green. |
| Dirty Corrugated Metal Sheet | 9abce0ff-9f4d-4c47-825e-da20b7a54c67 | 1x1 m, 8192 px/m | Old sheds and shelters. |
| Rusty Corrugated Metal Sheet | 9edfca8b-f417-4625-8b4b-45e7528e9f0d | 2x2 m, 4096 px/m | Derelict outbuildings. |
| Rusty Painted Metal Sheet | 651a17e7-bd42-473f-bf34-b48d8a7712d3 | 2x1 m, 4096 px/m | Garage doors and workshop doors. |

## Facade decals (weathering, also #84)

| Fab title | Listing id | Data | Use |
| --- | --- | --- | --- |
| Leakage | ab46f137-ab5c-4f68-a2f5-508d7a04680e | decal, 2x4 m, 2048 px/m, tagged seepage, stained, wall; appears twice in the category | Vertical rain streaks and seepage below sills, parapets and downpipes; the main rain-streak source. |
| Moss | category (decal, from $0.99) | not read | Moss on north faces, plinths and roofs. |
| Tileable Moss Patches | category (decal, "From Free") | not read | Moss and algae in patches on ledges and tile roofs. |
| Mud Stain | 4e897b61-f0f2-4605-86ce-64a46d15592b | decal, 0.25x0.25 m, wall | Splash dirt at the plinth. |
| Concrete Patch | c5b8b76a-74d7-43c2-acda-5d4723dafaa2 | decal, 8192 px/m, 0.5x0.5 m, opacity | Mortar and render repairs. |
| Concrete Damaged | 0e8ad45f-be2d-4760-a427-2dd3fa9b0b18 | surface, cracked, chipped | Cracks and spalling on slab blocks. |
| Military Old Bunker Tunnel Decal Concrete Damage 15 (and the series 01 to 18) | 85a34a6e-23a4-4662-b54d-a89b004e3a94 | decal, 8192 px/m, 2x0.5 m, with opacity | Spalling, cracks and stains, usable on concrete and render despite the name. |
| Damaged Concrete | category (decal) | not read | Chipped corners. |

Megascans has no efflorescence decal and no general grime or soot decal. See the next section.

## Facade props from #85

The searches for downpipes, gutters, letterboxes and mailboxes returned no Megascans items; the hits were all
third-party models. There is no Megascans entry to add here. Hamburg-specific props (the round zinc downpipe,
the yellow Deutsche Post box, house-number plates, bell panels) are generated with the existing Hunyuan prop pipeline
(`Tools/buildingkit/props`).

## What Megascans does not have

- Hamburg clinker in a deep red-brown with light mortar and in the yellow-brown of the 1920s: tint the Brick Wall
  scans, or take the six CC0 brick sets from Poly Haven already in `Content/Textures`.
- Smooth white and pastel render in the exact finishes of Neuallermöhe and refurbished blocks: use the CC0 plaster sets
  from Poly Haven and ambientCG, tinted, with the thin-render normal turned down.
- Clay Pfannen with the S-profile and anthracite engobed concrete tiles: the kit roof pieces model the profile;
  take colour and weathering from Roof Tiles and from Poly Haven roof tile sets.
- White-painted timber casements and white PVC frames: a flat-colour frame material with a paint-wear mask (from Old
  Wooden Window) is better than a scan.
- Efflorescence, grime and algae streaks as decals: ambientCG has grunge and dirt maps (CC0) for a procedural overlay
  in #84, and the weathering shader should generate rain streaks and efflorescence from masks.
- Letterboxes, downpipes, bell panels, satellite dishes, window blinds: generate with Hunyuan.

## What the download step needs

`Tools/asset_fetch/fab_download.py "<title>" <out>` reads the user's Fab library, takes the first item whose title
contains the given text, and downloads `projectVersions[-1]`, the newest Unreal project version, as a chunked Epic
build. I did not run it (the Heroic token lasts about a day). From Fab's documentation and the listing data:

- Megascans on Fab are offered in the Unreal Engine format, FBX and glTF, in four quality tiers. In the Unreal
  Engine format the Fab window in the editor delivers the assets as a project pack with the tier chosen in the
  window; whether the library entry carries a `projectVersions` list with an artifact per tier could not be
  confirmed without the login. The first run should therefore be
  `fab_download.py "Brick Facade" /tmp/megascans_probe --list`, which prints the files without downloading, and
  shows whether the artifact is there and which tier it holds.
- The script must be changed before a batch: an exact title match (several of the titles above are substrings of
  others, and two are duplicated, so the first hit is not always the intended one), an optional listing id to pick
  between duplicates, and a tier choice if the artifacts differ per tier. A missing `projectVersions` must give a
  readable message, not an exception.
- Output goes to a data-root folder such as `<data root>/unreal/Megascans` linked into `Content/Megascans`, never
  into git. `Tools/bootstrap/data_root.py` needs one more link for it.
- The items must be in the library first. If the user claimed Megascans during the free period that ended in 2024,
  the entries are in the legacy Quixel library and may not show up in the Fab library; the free claim on Fab is a
  separate one. Deprecated assets, such as the old "Painted Brick Wall", were not moved to Fab.

## What the user does on fab.com

1. Log in to fab.com with the account Heroic uses.
2. Open each listing in the tables (ids are in the URLs, titles can be searched), and press the add-to-library
   button on the free ones, and buy the "From $0.99" ones if wanted. The ones to add first, by value for the
   vertical slice (#87): Brick Facade, Brick Wall (both), Brick Wall Worn, Stucco Wall (both), Stucco Facade,
   Plaster Wall, Precast Concrete Wall, Roof Tiles, Leakage, Moss, Tileable Moss Patches, Concrete Patch, Old
   Damaged Wood, Stone Brick Wall.
3. Tell me when the items are in the library, then the first probe above runs.
