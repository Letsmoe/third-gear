# Hamburg building typology

The ground truth that every kit style (#82) and every placement and classification rule (#81) is checked against.
It covers Bergedorf, Vierlande and Hamburg in general, plus the German types that appear there.

How to read the numbers. Shares of residential buildings, dwellings per building and construction periods come from
the 2022 census (Zensus 2022, Statistikamt Nord, tables 3.1 to 3.3 for the districts and quarters of Hamburg), read
directly from the published tables. Shares of OSM tags come from `Tools/buildingkit/osm_tag_stats.py` on
`bergedorf_core`. Class shares are an estimate that combines both, and say so. Dimensions (storey height, bay width,
window size, pitch) are the usual values of each period from the IWU German residential building typology (TABULA),
German building practice of the period (the 2.5 m minimum clear height of the 1950s, 2.4 m after, the later Hamburg
building code) and from measuring the reference photos, not from a survey of Bergedorf. Treat them as plausible
defaults with a spread, and measure on the photos in `/mnt/storage/third-gear/building_kit/references/<class>/`
(sources and licences in `SOURCES.md` there) when a style is built.

## Numbers for Hamburg and Bergedorf

Residential buildings (Gebäude mit Wohnraum), census of 2022-05-15.

| | Hamburg | Bergedorf district |
| --- | --- | --- |
| Buildings with living space | 260,630 | 22,833 |
| 1 dwelling | 60.4 % | 68.4 % |
| 2 dwellings | 6.4 % | 9.1 % |
| 3 to 6 dwellings | 12.9 % | 13.1 % |
| 7 to 12 dwellings | 15.4 % | 7.4 % |
| 13 or more dwellings | 4.9 % | 2.0 % |
| Built before 1950 | 24.0 % | 24.0 % |
| 1950 to 1969 | 34.0 % | 22.0 % |
| 1970 to 1989 | 17.7 % | 19.6 % |
| 1990 to 2009 | 15.5 % | 25.8 % |
| 2010 or later | 8.8 % | 8.6 % |

Bergedorf quarters, which differ a lot and decide which classes dominate where.

| Quarter | Buildings | Before 1950 | 1950 to 1969 | 1990 to 2009 | 1 dwelling | 13 or more dwellings |
| --- | --- | --- | --- | --- | --- | --- |
| Bergedorf (centre) | 5,419 | 39.5 % | 19.0 % | 13.2 % | 60.2 % | 3.5 % |
| Lohbrügge | 5,880 | 12.4 % | 43.2 % | 20.7 % | 64.6 % | 3.4 % |
| Neuallermöhe | 3,248 | 0.8 % | 0.5 % | 67.3 % | 71.2 % | 1.5 % |
| Kirchwerder | 2,998 | 29.7 % | 13.8 % | 23.4 % | 74.8 % | 0.2 % |
| Neuengamme | 1,149 | 34.9 % | 19.8 % | 18.5 % | 75.5 % | 0.0 % |
| Curslack | 1,001 | 29.1 % | 12.2 % | 29.8 % | 76.7 % | 0.3 % |
| Ochsenwerder | 858 | 32.2 % | 11.7 % | 18.3 % | 76.1 % | 0.6 % |
| Altengamme | 694 | 39.0 % | 14.8 % | 19.6 % | 74.6 % | 0.0 % |

So the centre (Sachsentor, Holstenstraße, Hinterm Graben) is the Gründerzeit and 1920s core with shops, Lohbrügge is the
1950s to 1970s belt with the large slab blocks in Lohbrügge-Nord, Neuallermöhe is the 1990s planned district, and the
four marsh quarters (Vierlande) are one-dwelling houses along the dykes with an old farmhouse share of about a third
built before 1950.

The census has no building type below the number of dwellings, so "one dwelling" stands for detached, semi-detached
and terraced houses together.

### What OSM tags in `bergedorf_core`

4,353 buildings, from `osm_tag_stats.py`.

| Tag | Buildings with it | Share |
| --- | --- | --- |
| `building` (always) | 4,353 | 100 % |
| `building:levels` | 1,251 | 28.7 % |
| `roof:shape` | 890 | 20.4 % |
| `roof:levels` | 1,048 | 24.1 % |
| `addr:housenumber` | 2,672 | 61.4 % |
| `start_date` | 107 | 2.5 % |
| `height` | 4 | 0.1 % |
| `building:material` | 2 (both plaster) | 0.05 % |
| `roof:material` | 3 | 0.07 % |
| `roof:colour`, `building:colour` | 1 and 2 | under 0.1 % |

Values of `building`: `yes` 1,760 (40.4 %), `apartments` 807, `detached` 593, `garage` 350, `house` 222,
`semidetached_house` 137, `shed` 79, `industrial` 54, `retail` 49, `garages` 46, `allotment_house` 38, `school` 35,
`residential` 33, `roof` 32, `commercial` 22, `terrace` 17, `office` 12, then under ten each (service, hut, public,
train_station, kindergarten, sports_centre, carport, greenhouse).

Values of `building:levels`: 2 for 588, 1 for 296, 3 for 203, 4 for 105, 5 for 37, 6 for 15, 7 to 9 for 7. For the
apartment blocks that have it, the levels are 2 (145), 3 (142), 4 (90), 5 (25), 6 (13), so the typical Bergedorf
apartment building is three storeys, not five. Values of `roof:shape`: gabled 306, flat 224, hipped 164, many 43,
half-hipped 40, quadruple saltbox 26, double saltbox 25, gambrel 20, saltbox 15, mansard 8, skillion 8, pyramidal 6,
round 3. For the houses tagged `detached`, `house` or `semidetached_house` and having it: gabled 196, hipped 134,
flat 60. `roof:levels` is 1 for 778 buildings, 0 for 161 and 2 for 109, so one habitable attic level is the norm.
The `start_date` values that exist are almost all 1886 to 1912 (Gründerzeit and Jugendstil) and are on the
listed buildings, so they say nothing about the whole stock.

Footprint area (all buildings): under 25 m² 11.1 %, 25 to 40 m² 5.8 %, 40 to 70 m² 10.3 %, 70 to 120 m² 20.4 %,
120 to 250 m² 33.2 %, 250 to 600 m² 12.5 %, 600 to 1,500 m² 5.1 %, over 1,500 m² 1.7 %. 57.5 % of all footprints
share a wall (lie within 0.1 m of another footprint along at least 3 m); that includes garage rows, semi-detached
pairs and terraces, so it overstates closed perimeter blocks.

Consequences for #81. `building:levels` and `roof:shape` exist for only a quarter and a fifth of the buildings, and
`height`, `building:material` and colours are essentially absent, so every material and colour is a rule or a
statistic. The tag most worth trusting is `building=house` or `detached` or `terrace`, then the footprint area and
terrace contact. 40 % of the buildings are plain `yes` and need the whole geometry rule set.

## Class shares

Estimated share of the buildings in `bergedorf_core` (the town and Lohbrügge edge, no Vierlande) and of the whole
Bergedorf district, from the tags above, the footprint bands and the census. The two columns are not additive with
the census table, which counts only residential buildings.

| Class | Folder | Share in `bergedorf_core` | Share in Hamburg residential stock (estimate) | Period |
| --- | --- | --- | --- | --- |
| 1. Gründerzeit and Jugendstil town house | `gruenderzeit_clinker` | 7 % | 8 % | 1870 to 1914 |
| 2. 1920s and 30s brick block and brick estate | `brick_block_1920s` | 3 % | 7 % | 1919 to 1939 |
| 3. Postwar plaster house and row | `postwar_plaster` | 12 % | 15 % | 1948 to 1968 |
| 4. 1960s to 1970s slab block and high-rise | `slab_block` | 3 % | 6 % | 1960 to 1979 |
| 5. Terraced house | `terraced` | 3 % | 7 % | 1925 to now |
| 6. Semi-detached house | `semidetached` | 4 % | 9 % | 1925 to now |
| 7. Detached single-family house | `detached_postwar` | 16 % | 23 % | 1920 to now |
| 8. Villa | `villa` | 1 % | 1 % | 1880 to 1935 |
| 9. 1990s town house and modern plaster and glass | `modern` | 5 % | 12 % | 1985 to now |
| 10. Shop and office house with commercial ground floor | `commercial_groundfloor` | 4 % | 3 % | any |
| 11. Vierlande farmhouse and half-timbered | `vierlande_farmhouse` | 0 % (3 % of the Vierlande quarters) | 1 % | 1600 to 1900 |
| 12. Garage, shed, carport and outbuilding | `shed_garage` | 26 % | not counted | any |
| 13. Industrial hall and large commercial shed | `industrial_hall` | 2 % | not counted | 1950 to now |
| 14. Public and institutional (school, church, station, fire station) | not photographed | 2 % | not counted | any |

The 26 % outbuildings come from garage, garages, carport, shed, hut and allotment tags (555 buildings, 12.7 %) plus
the untagged footprints under 40 m² (about 13 %). Class 3 and 7 together are the bulk of the residential stock in
Lohbrügge and the marsh quarters. A building that has more than one class (a Gründerzeit house with a shop) takes
class 10 for its ground floor and class 1 above.

## Dimensions common to German houses

- Floor to floor: Gründerzeit 3.2 to 3.6 m (clear height 3.0 to 3.4 m); 1920s to 1930s 2.9 to 3.1 m; 1950s 2.6 to 2.8 m;
  1960s to 1980s 2.6 to 2.75 m; since 1990 2.7 to 2.85 m. A shop ground floor is 3.5 to 4.5 m.
- A pitched roof storey (Dachgeschoss) is counted as a level only when it is habitable. OSM `building:levels` normally
  excludes the roof, `roof:levels` counts it.
- A storey is about 3 m in the Hamburg building code for a rough height estimate: eaves height is levels times
  2.75 to 3.25 m plus a plinth of 0.3 to 0.9 m, ridge height is eaves height plus half the depth times tan of the
  pitch.
- Window frames by period: single-glazed painted timber casement or double casement (Kastenfenster) before 1950, with
  glazing bars (Sprossen) of 2 to 6 panes; single-glazed or early double-glazed painted timber 1950 to 1970; white
  PVC or aluminium with double glazing 1975 to now, mostly without glazing bars; since about 2000 grey anthracite
  frames on new buildings. A refurbished older house keeps its facade proportions but has white PVC windows with
  stuck-on or no bars.
- Roller shutters in white or grey plastic or aluminium appear from the 1960s on every residential type, visible as a
  box above the window on postwar houses.

## The classes

### 1. Gründerzeit and Jugendstil town house (`gruenderzeit_clinker`)

- Share: 7 % of the core, with a peak in the centre (39.5 % of the centre's buildings are pre-1950). A third of them
  are exactly this type.
- Period: 1870 to 1914. Bergedorf grew after the Hamburg-Berlin railway of 1846 and the Gründerzeit; the examples
  are on Von-Anckeln-Straße, Daniel-Hinsche-Straße, Ernst-Mantius-Straße and Lohbrügger Landstraße.
- Storeys: 3 to 4 plus an attic (`building:levels` 3 to 4, `roof:levels` 1). Storey height 3.2 to 3.6 m, ground floor
  often 3.6 to 4.0 m with a raised ground floor (Hochparterre, 0.8 to 1.2 m above ground, several steps).
- Window grid: axis spacing 2.0 to 2.8 m, a module of about 2.3 m. Openings 1.1 to 1.4 m wide and 1.9 to 2.4 m high,
  proportion about 1 to 1.8 (standing, tall), segmental or flat arches with a stone or stucco lintel; double casement
  with 2 to 3 panes across, a fixed top light with 2 more, glazing bars thin and painted white; frames white; stone or
  cement sills projecting 6 to 10 cm. Reveal depth 0.15 to 0.25 m.
- Walls: red to brown-red machine brick or clinker (Hamburg clinker, hard-fired, dark), thin light mortar joints,
  often with light stucco bands (plastered strips) between the floors; also plastered facades painted white,
  cream or pale grey (as the photos show). Wall thickness 0.36 to 0.5 m.
- Roof: hipped or gabled, 40 to 50 degrees, clay Pfannen (red-brown) or blue-black slate; steep mansard on the larger
  houses. Dormers as gabled or hipped dormers, often a central gable (Zwerchgiebel) over the axis of the entrance.
- Ground floor: housing, or a shop in the centre (class 10). Entrance with a few steps, a panelled timber door with a
  fanlight, a stone surround.
- Details: cornice with brick corbels or stucco, string courses between the floors, a stone or brick plinth of 0.5 to
  0.9 m (see the kit's `brick` style), corner quoins, bay windows (Erker) on the corner or the front, balconies with
  wrought iron railings on the upper floors, cast-iron fences with brick posts.
- OSM recognition: `building=house` or `apartments` or `yes`, `start_date` before 1914 if present, `building:levels`
  3 or 4, footprint 70 to 200 m² and narrow (7 to 12 m frontage) or squarish for villas, in a street where footprints
  touch both neighbours (terrace contact on two sides) or stand 3 to 6 m apart, near the old centre; roofs gabled
  or hipped with ridge parallel to the street.

### 2. 1920s and 30s brick block and brick estate (`brick_block_1920s`)

- Share: 3 % of the core, 7 % of Hamburg (the Hamburg brick expressionism of Fritz Schumacher; Bergedorf has the
  Adolf-von-Elm-Hof, Dulsberg and Schumacher estates elsewhere in Hamburg; in the district, parts of Lohbrügge and
  the Bergedorf centre and several railway and municipal estates).
- Period: 1919 to 1939.
- Storeys: 3 to 5 plus an attic, storey height 2.9 to 3.1 m, ground floor with a half-sunken cellar light.
- Window grid: regular rows (Lochfassade), axis 1.8 to 2.4 m, windows 1.1 to 1.3 m wide and 1.4 to 1.8 m high
  (proportion about 1 to 1.35, standing), flat lintels in brick on edge or rowlock, white painted timber casements with
  a cross of glazing bars or a split top light, projecting brick sills or stone; deep reveals 0.2 m. Stair windows are
  narrow, high and offset (half-landing height).
- Walls: dark red to brown-red hard-fired clinker with rich colour variation, pale mortar, plastic brickwork
  ornament (corbels, projecting courses, zig-zag friezes, vertical brick stripes on piers), occasionally yellowish clinker.
  Wall thickness 0.36 to 0.5 m.
- Roof: steep hipped or gabled, 45 to 55 degrees, red clay Pfannen, many small dormers (Gauben) in a regular row, or
  occasionally a flat roof with a brick parapet in the later estates.
- Ground floor: housing; shops in corner buildings; arched passages into courtyards (Torhäuser).
- Details: stepped corbel cornices, rounded or chamfered corners, brick bay windows in the stair towers, entrance
  with a recessed porch and brick pillars, loggias to the courtyard.
- OSM recognition: `building=apartments` or `residential`, 3 to 5 `building:levels`, long thin footprint (10 to 14 m
  deep, 30 to 120 m long) or a ring or U shape enclosing a courtyard (an inner way, or `building` ring with a hole),
  rows along street edges, often bordered by `landuse=residential`; `start_date` 1919 to 1939 if present.

### 3. Postwar plaster house and row (`postwar_plaster`)

- Share: 12 % of the core, 22 % of the buildings of the 1950 to 1969 census period (Lohbrügge: 43.2 % of its
  buildings), the most numerous Bergedorf type after the detached house.
- Period: 1948 to 1968, the Wiederaufbau with its simple Siedlung houses, two to three storeys, and the rows of the
  Siedlung estates.
- Storeys: 2 to 3 plus a pitched attic. Storey height 2.6 to 2.8 m (the 1950s rule of a minimum clear height of 2.5 m).
- Window grid: axis 1.5 to 2.2 m, windows 1.0 to 1.4 m wide, 1.2 to 1.5 m high, proportion about 1 to 1.2 (nearly
  square, wide), single casement or two casements with a horizontal bar, painted white timber, later PVC; shallow
  reveal 0.1 to 0.15 m, a simple projecting sill of concrete or clinker; roller shutter boxes from the 1960s; stair
  windows are narrow vertical slits or glass-block panels.
- Walls: fine scratch render (Kratzputz) or smooth rendered brick, painted in off-white, pale yellow, beige, light
  grey, occasionally terracotta and green; a plinth zone in dark render, clinker or cement 0.4 to 0.8 m; rarely
  clinker. Refurbished houses often have external thermal insulation with a thin render, which makes the walls
  slightly thicker (0.4 to 0.5 m instead of 0.3), the reveal shallower and the window sill a thin metal flashing.
- Roof: gabled 30 to 40 degrees, sometimes hipped or half-hipped, red or brown-red concrete or clay tiles from the 1950s,
  anthracite engobed concrete tiles after 1975; eaves overhang 0.3 to 0.5 m with a timber soffit; one or two
  small dormers, gable end chimneys.
- Ground floor: housing, with the corner shop occasionally in a larger block. Entrance with a small flat canopy or a
  porch (Windfang), a door with a glass panel, 2 to 3 concrete steps.
- Details: bay with loggia or balcony with a steel railing on the garden side, a flat canopy over the entrance, small
  rooflight or flat dormers, occasionally a corner window.
- OSM recognition: `building=house`, `residential`, `terrace` or `apartments` with 2 to 3 levels, footprint 80 to 200
  m² for houses and 100 to 400 m² for slabs of 6 to 12 dwellings, in the pre-1970 residential grid, with a gabled
  roof parallel to the long axis; terraces of 4 to 8 units of 5 to 6 m width.

### 4. 1960s to 1970s slab block and high-rise (`slab_block`)

- Share: 3 % of the core. In Hamburg 6 %, concentrated in the large estates: Lohbrügge-Nord (the Lindwurm, the
  Billebogen, Binnenfeldredder), Bergedorf-West and the tower blocks along the Weidenbaumsweg.
- Period: 1960 to 1979; the slab blocks of Neuallermöhe-West are 1990s and belong to class 9.
- Storeys: 4 to 8 for slabs (long), 9 to 17 for towers; storey height 2.7 m (floor to floor 2.75 m to 2.8 m).
- Window grid: a strict repeating grid of 2.4 to 3.0 m, windows 1.2 to 1.6 m wide as wide ribbon or a window and
  balcony door pair, proportion 1 to 1.1 to 1 to 0.8 (wide), single large fixed pane with a tilt-turn casement, white
  PVC after refurbishment or aluminium frames in the original; parapet fields below the window in a contrasting colour
  (a panel of coloured plate, brick or concrete).
- Walls: prefab concrete panels with exposed aggregate or a smooth painted finish, light grey, white, ochre, red-brown
  and blue accent panels; many buildings were refurbished since 2000 with an external insulation composite system
  (WDVS) in coloured render with bright bands in red, yellow and green, and a darker plinth; rarely a yellow clinker
  facing. Stair towers in brick or glass.
- Roof: flat with a parapet, bitumen sheet or gravel, a lift or stair head house on top, a flat overhang 0.3 to 0.6 m.
- Ground floor: housing, some with entrance lobbies in glass and steel, garages and parking decks at the edge.
- Details: continuous balcony stacks with concrete or steel balustrades, often in colour; loggias; satellite
  dishes; outside stairs; the facade is mostly a rhythm of windows and balconies.
- OSM recognition: `building=apartments`, `building:levels` 4 to 8 (or none) and a long thin rectangle of 10 to 14 m by
  30 to 120 m, or a tower with 18 to 30 m square footprint; inside a `landuse=residential` area with rows and open
  green between the buildings; many stand alone (no contact).

### 5. Terraced house (`terraced`)

- Share: 3 % of the core (`terrace` in OSM is rare, 17 buildings, because rows are mostly drawn as separate houses);
  Neuallermöhe and parts of Lohbrügge have many.
- Period: from the 1920s Siedlung rows to the 1990s Neuallermöhe estates; most are 1950 to 2010.
- Storeys: 2 to 3 plus attic, 5 to 6 m wide per unit, 8 to 11 m deep. Storey height 2.6 to 2.8 m.
- Window grid: one window and a door per bay, a ground floor with a wide window and the entrance, one or two windows
  above, axis 2.5 to 3.0 m; window 1.2 to 1.4 m wide, 1.2 to 1.6 m high, white PVC or timber.
- Walls: plaster in light colours (white, beige, yellow) or brick facing in red and brown clinker; rows alternate in
  colour; a dark plinth.
- Roof: gabled 30 to 45 degrees along the row or a shallow monopitch or flat roof in later estates, red or anthracite
  tiles, a continuous ridge, party walls rising slightly above the roof (brick fire walls) in older rows.
- Details: individual front gardens with hedges, a carport or garage in a block at the end, a small porch, a shared
  garage court behind.
- OSM recognition: `building=terrace` or `house` or `residential` in a row of 3 to 12 footprints of 5 to 6 m
  frontage, 8 to 11 m depth (footprint 40 to 80 m²) sharing walls, in a straight line along the street, often with
  garage footprints at the end.

### 6. Semi-detached house (`semidetached`)

- Share: 4 % of the core (OSM `semidetached_house`: 137, 3 %); the type that is drawn as one footprint per half.
- Period: 1925 to 1970 mainly, later infill.
- Storeys: 2 plus attic, each half 6 to 8 m wide, 9 to 11 m deep (footprint 60 to 100 m² per half); storey height
  2.6 to 2.9 m, higher in older ones (3.0 m, as in the older houses of the photo class).
- Window grid: two windows and a door per half, axis 2.0 to 2.6 m, window 1.0 to 1.4 m wide and 1.3 to 1.7 m high,
  white timber or PVC, with or without bars; side wall without windows or with small ones.
- Walls: plaster in a pale colour, brick or clinker on older ones, a plinth 0.5 m.
- Roof: gabled 35 to 50 degrees with the ridge parallel to the street or a gable-front; one steep roof for both
  halves, one dormer per half, a shared chimney on the party wall.
- Details: a symmetrical pair of porches, a bay window per half, small front gardens, a shared drive and garage in
  between.
- OSM recognition: `building=semidetached_house` or two touching `house` footprints of identical size and mirrored
  shape, total width 12 to 16 m.

### 7. Detached single-family house (`detached_postwar`)

- Share: 16 % of the core. 68 % of the residential buildings of Bergedorf have one dwelling, and in the Vierlande
  quarters 75 %. OSM: `detached` 593 and `house` 222 (815, 19 % of the core).
- Period: 1920 to now, with three common waves: the 1930s Siedlung house (Bauhaus-like and the gabled Heimatstil),
  the 1950s to 1970s Siedlungshaus and bungalow, and the 1980s to 2010s house.
- Storeys: 1 plus attic (the Bungalow and the 1950s house) or 2 plus attic; storey height 2.6 to 2.8 m (1950s), 2.7 to
  2.85 m (modern). Footprint 80 to 160 m²; building height 6 to 9 m.
- Window grid: two to four windows per front, axis 1.8 to 2.8 m, window 1.0 to 1.4 m wide, 1.2 to 1.5 m high,
  occasionally a floor-to-ceiling terrace door; white PVC from the 1975 on.
- Walls: smooth or scratch render in white, off-white or beige, a clinker facing in the Hamburg red-brown (often
  half-brick facing on 1930s to 1960s houses), occasionally a timber or slate-grey cladding on the gable; a plinth of
  0.4 m.
- Roof: gabled 30 to 45 degrees; hipped 25 to 40 (in the OSM tags, hipped 134 against gabled 196 for these houses);
  half-hipped; flat roofs on the 1930s Bauhaus-like and 1960s bungalow types; red or anthracite concrete tiles.
- Details: an attached single garage or carport, an entrance porch, a conservatory, a chimney, roof windows;
  the garden side often has a terrace door.
- OSM recognition: `building=house` or `detached`, one footprint with 80 to 160 m², 10 to 20 m from the next
  building, `building:levels` 1 or 2 (409 and 235 of those that have it), in `landuse=residential` with
  gardens; roof gabled with the ridge along the longest axis unless the house is squarish (then hipped).

### 8. Villa (`villa`)

- Share: 1 %; Bergedorf has a notable number of Gründerzeit and Jugendstil villas on Wentorfer Straße, Sichter
  and Hermann-Distel-Straße.
- Period: 1880 to 1935. Storeys: 2 to 3 plus attic, storey height 3.2 to 3.8 m.
- Window grid: tall windows 1.2 to 1.5 m by 2.0 to 2.6 m with sandstone surrounds, mixed with oriel windows (Erker)
  and a tower or loggia; glazing bars in the upper sash; shutters in green.
- Walls: white or cream stucco with plastic decoration, or red clinker with sandstone bands, or half-timber in
  the gable.
- Roof: complex, with hip-and-gable intersections, steep 45 to 60 degrees, a mansard on bigger villas, clay tiles or
  slate, dormers with decorated bargeboards, a tower with a pointed roof.
- Details: balconies with turned balusters or iron, carved porches, a garden with a cast-iron fence on a brick plinth.
- OSM recognition: `building=house` with a large irregular footprint (150 to 300 m²) with many corners (projections),
  in a green street with large plots (over 800 m²), `start_date` 1880 to 1935 if present.

### 9. 1990s town house and modern plaster and glass (`modern`)

- Share: 5 % of the core; 25.8 % of Bergedorf's residential buildings date from 1990 to 2009 (Neuallermöhe 67 %),
  8.6 % since 2010.
- Period: 1985 to now.
- Storeys: Neuallermöhe type 3 to 4 with a flat or shallow monopitch roof, storey height 2.7 to 2.85 m; new apartment
  blocks 4 to 5 plus a set-back top floor (Staffelgeschoss); single houses 2 storeys, cubic (Stadtvilla with a hipped
  roof and a square footprint of 10 by 10 m, or a flat-roof cube).
- Window grid: strict, large, 1.2 to 2.4 m wide and 1.4 to 2.2 m high including floor-to-ceiling French windows,
  aluminium or PVC in anthracite grey or white, no glazing bars, flush with the facade on thick insulated walls (a
  reveal of 0.1 to 0.2 m), triple glazing; roller shutters in aluminium in the same colour.
- Walls: smooth render in white, light grey or colour accents (Neuallermöhe uses red, yellow, blue), with clinker
  panels on the ground floor, large glass areas, balconies in steel and glass, timber or metal cladding in a darker
  tone on the top floor.
- Roof: flat with a green roof or gravel, parapet with a metal coping, or a monopitch 5 to 15 degrees in zinc;
  Stadtvillas with hipped roofs at 25 degrees and anthracite tiles.
- Details: projecting balconies, steel stairs, entrance canopy in steel and glass, integrated garages, bicycle sheds.
- OSM recognition: `building=apartments`, `house` or `residential` with a rectangular footprint, 3 to 5 levels,
  `roof:shape=flat` or none, in the planned grid of Neuallermöhe (many identical footprints) or a new-build
  infill; `start_date` after 1985 if present.

### 10. Shop and office house with commercial ground floor (`commercial_groundfloor`)

- Share: 4 % of the core, mostly in the centre: Sachsentor (pedestrian zone), Holstenstraße, Alte Holstenstraße,
  Lohbrügger Markt; OSM `retail` 49, `commercial` 22, `office` 12.
- Period: Gründerzeit and 1950s infill to 1980s department stores; the typical pattern is a shop and display window on
  the ground floor with the housing above.
- Storeys: 3 to 4 above a ground floor that is 3.5 to 4.5 m high; the upper floors take the form of the period of the
  house; department stores and malls are 3 to 5 storeys at 4 m.
- Window grid: the ground floor is a continuous shop front of 3 to 6 m glazing divided by narrow piers (0.3 to 0.5
  m), the entrance recessed, a signboard band 0.6 to 1.0 m above it, a roller shutter box or awning; the upper floors
  keep the grid of the house type. A shop window sill is 0.4 to 0.7 m above the pavement.
- Walls: the upper facade as class 1 to 3; the ground floor facade in stone, glass, aluminium or coloured panels, a
  fascia sign in plastic or metal.
- Roof: as the host house (class 1 hipped or gabled, or flat for 1960s buildings).
- Details: awnings, hanging signs, a shop sign band, a canopy, a passage entrance, advertising boards, a cash
  machine or a delivery door at the side.
- OSM recognition: `building=retail`, `commercial`, `office`, or any building with a `shop=*` or `amenity=*`
  tag, or `addr` and a footprint touching its neighbours along the street of a `highway=pedestrian` or `living_street`
  or a `service` in the centre; footprint 80 to 300 m²; 3 to 4 levels.

### 11. Vierlande farmhouse and half-timbered (`vierlande_farmhouse`)

- Share: 0 % of the core, about 3 % of the buildings in the four marsh quarters Kirchwerder, Neuengamme, Curslack,
  Altengamme and Ochsenwerder (the census has 29 to 39 % of their buildings from before 1950, many are the
  old farm houses and their barns), and 1 % of Hamburg. Famous examples: the Rieck-Haus in Curslack
  (Freilichtmuseum), Altengammer Hauptdeich and Elbdeich, Kirchwerder.
- Period: 1600 to 1900; the typical Vierländer Fachhallenhaus and the later Vierländer Haus with a brick front.
- Storeys: 1 plus a roof space, storey height 2.1 to 2.5 m (low), a wall height of 2.3 to 3.0 m to the eaves;
  footprint 10 to 14 m wide, 20 to 40 m long (house and barn under one roof) or 8 by 15 m for the smaller house.
- Window grid: small windows 0.8 to 1.0 m wide and 1.0 to 1.3 m high with a 6- or 8-pane (Sprossen) casement in white
  or green; irregular spacing of 1.2 to 2.0 m; a lower door with a half-door (Klöntür) in green or blue.
- Walls: timber frame (Fachwerk) in black-brown oak with a bay width of 1.0 to 1.4 m and brick infill (Gefach) in
  red, often with patterns (herringbone, diagonal), and the gable side often in decorative half-timber with white
  plaster in some; brick front (`Backsteinfront`) on the street side of the later ones, plinth in granite
  fieldstone 0.4 to 0.8 m.
- Roof: a large, steep hipped roof (Krüppelwalm) with a thatched reed roof (Reet) 50 to 55 degrees, 0.3 to 0.4 m
  thick, with a ridge in turf or reed, overhanging eaves to 0.7 m, a roof ridge decoration on a few; in the 19th
  century red clay tiles replaced thatch.
- Details: the main door in the gable with a decorative frame and an inscription on the beam, Vierländer lattice
  windows, a green timber gate, the front garden with a hedge, a barn with a large gate (Tor) and a brick pillar
  next to it, the dyke in front.
- OSM recognition: `building=house` or `farm` or `yes` with a long footprint (200 to 500 m², 10 to 14 m wide, 20 to 40
  m long, ratio over 2), in the marsh quarters along `highway` named "Deich" or "Hauptdeich" or "Elbdeich", with
  nearby `building=farm_auxiliary` or `barn`, one or two on each plot; `roof:shape=hipped` or `half-hipped` and
  `roof:material=reed` if mapped; a `start_date` before 1900.

### 12. Garage, shed, carport and outbuilding (`shed_garage`)

- Share: about 26 % of the buildings in the core, most of them under 40 m²; OSM `garage` 350, `garages` 46, `shed` 79,
  `allotment_house` 38, `carport` 4, `hut` 6, `roof` 32 (a covered area without walls).
- Period: any; the garage rows of the 1960s to 1970s are the largest group.
- Storeys: 1, height 2.4 to 3.2 m; single garage 3 by 6 m (18 m²) to 3 by 6 m each in a row of 5 to 20; sheds 3 by 4
  m to 4 by 8 m; allotment garden cabins 3 by 4 m with a gabled roof at 25 degrees.
- Openings: a metal sectional or swing garage door, 2.4 to 2.5 m wide and 2.0 to 2.1 m high, painted in red-brown,
  green or white, a small window in a side wall of a shed, a timber door.
- Walls: exposed concrete or concrete block, rendered white or grey, corrugated sheet or timber boarding in
  dark brown or green (allotment cabins), rarely brick.
- Roof: flat with a bitumen sheet and a metal edge, or a shallow monopitch of 3 to 10 degrees with corrugated fibre
  cement or metal sheet; allotment cabins gabled with bitumen shingles or tiles.
- Details: a row has identical doors, a concrete apron, a drain pipe on the corner, graffiti on garage rows, an
  overgrown edge, a service road at the back.
- OSM recognition: `building=garage`, `garages`, `carport`, `shed`, `hut`, `allotment_house`, or any `yes` below
  40 m², and `roof` for open shelters; a row has several identical footprints of 3 by 6 m side by side.

### 13. Industrial hall and large commercial shed (`industrial_hall`)

- Share: 2 % of the buildings but a large share of the footprint area (54 `industrial`, 49 `retail` of which many are
  halls, 22 `commercial`); the industrial estates of Bergedorf (Curslacker Neuer Deich, Billbrookdeich side, Allermöhe).
- Period: 1950 to now; older brick works and factory buildings (1900 to 1930) exist in the centre.
- Storeys: 1 to 2, a clear height of 6 to 10 m, an office block of 2 to 3 storeys attached at the front (storey
  height 3.0 to 3.5 m); footprint 600 to 8,000 m², 20 to 60 m deep.
- Window grid: a ribbon of windows in the office part, 1.5 to 2.5 m wide, aluminium; the hall has few or no windows,
  a row of loading doors (roller or sectional, 3.0 to 4.0 m wide, 3.5 to 4.5 m high), a continuous clerestory or
  a band of translucent panels near the eaves.
- Walls: sandwich panels in light grey, white or an accent colour, trapezoidal sheet in grey or white, a concrete
  plinth 1.0 to 1.5 m high, exposed prefab concrete, brick infill in older halls; the office block in brick, plaster
  or glass and steel.
- Roof: flat or a shallow gable of 3 to 10 degrees with a membrane or trapezoidal metal sheet, roof lights in rows, a
  parapet or an eaves gutter, ventilation units on the roof; sawtooth roofs on older halls.
- Details: loading ramps, canopies, silos, tanks, containers, a fence with a gate, a security house, large
  parking areas, signboards.
- OSM recognition: `building=industrial`, `warehouse`, `retail`, `commercial` with a footprint over 600 m², low
  `building:levels` (1 or none), in `landuse=industrial` or `commercial`, `roof:shape=flat`.

### 14. Public and institutional (not photographed)

- Share: about 2 %: `school` 35, `kindergarten` 4, `public` 5, `train_station` 4, fire station, church (8 places of
  worship), town hall, court.
- Period and form vary: a brick school of 1900 to 1930 (the Luisen-Gymnasium Altbau), a 1960s to 1970s pavilion
  school in concrete and brick, a Gothic brick church (St. Petri und Pauli), Bergedorf castle, the water tower.
  Handle as landmarks (#81) or as large brick buildings with the class 2 style and a lot of glazing.

## Which kit styles cover which classes (feeds #82)

The kit has four styles in `Tools/buildingkit/kit/styles.py`: `brick`, `plaster`, `block` and `farm`.

| Class | Covered by | What is missing |
| --- | --- | --- |
| 1 Gründerzeit town house | `brick` (3.25 m storey, 0.36 m wall, 45 degrees, arched windows 1.0 by 1.8 m, cornice, string course, plinth) | Plastered Gründerzeit variant in white and cream; a raised ground floor with steps; bay window (Erker); balconies; mansard and Zwerchgiebel; slate roof. |
| 2 1920s brick block | partly `brick` | A 2.9 to 3.1 m storey; a flat lintel with brick-on-edge instead of the arch; offset stair windows; ornamental brick corbels and vertical piers; a row of small dormers on a 50 degree roof; courtyard passage. |
| 3 Postwar plaster house | `plaster` (2.75 m storey, 0.30 m wall, 35 degrees) | Wide near-square window with a horizontal bar and roller shutter box; a plinth in dark render; external insulation variant (thicker wall, shallower reveal); small flat entrance canopy; dormer variants. |
| 4 Slab block | `block` (2.75 m storey, flat roof) | A panel grid with parapet fields in contrasting colour; continuous balcony stacks; coloured insulation render with bands; tower of 9 to 17 storeys; stair head house on the roof. |
| 5 Terraced house | none (`plaster` would do for the wall) | A narrow bay of 5 to 6 m with a door and a window; party walls with no side windows; roof continuing along the row; fire wall above the roof; garage end block. |
| 6 Semi-detached house | none (`plaster` for the wall) | A mirrored pair with a shared roof, a side wall without windows, double porch. |
| 7 Detached house | `plaster` for the 1950s house | A bungalow with one storey and a hipped roof; a flat-roof Bauhaus cube; a half-hipped roof; an attached garage and a porch; a brick facing variant. |
| 8 Villa | none (`brick` for the walls) | Complex roofs, a tower, oriel windows, a decorated porch, shutters. |
| 9 Modern | none | Flat roof with parapet; large flush aluminium windows with French doors; balconies in steel and glass; a set-back top floor; coloured render panels; a Stadtvilla hip roof. |
| 10 Commercial ground floor | none | A shop front module (3 to 6 m glazing, piers, fascia band, awning, roller shutter box), recessed entrance, signboards. |
| 11 Vierlande farmhouse | `farm` (2.25 m storey, thatch roof 50 degrees, timber frame) | The brick front variant; the barn with a large gate; a hipped half-hip (Krüppelwalm) gable; the lattice window; the green door with a half-door. |
| 12 Garage and shed | none | A garage row with sectional doors, a flat roof with a metal edge, a timber shed, an allotment cabin, a carport. |
| 13 Industrial hall | none | A sandwich panel wall with a concrete plinth, a loading door, a clerestory band, a low gable or flat roof with roof lights, an office front. |
| 14 Public | none | Larger windows, a tower, a hip roof in copper or slate; landmarks only. |

So the four styles serve four of the fourteen classes (1, 3, 4, 11), partly serve 2 and 7, and nine classes need a style or a module set: 5, 6, 8, 9, 10, 12, 13, 14 and the 1920s brick block. By share of buildings the
missing ones that matter most are garages and sheds (26 %), detached house variants (16 %), the modern 1990s houses (5 %),
and the shop front (4 %).

## Known gaps

- Dimensions were not surveyed. The next step is to measure bay width, storey height and window size from
  photographs of Bergedorf streets (Mapillary could not be viewed here), which should be done when #87 is built.
- The census gives no building type; the class shares are estimates. The ratios can be sharpened with Hamburg's
  LoD2 data (the check in #81), which has roof shape and height for every building.
- The photo folders `semidetached`, `industrial_hall` and `modern` are thin: semi-detached has 2 photos, industrial
  has 3, and `modern` holds 1970s and 1990s buildings from Lohbrügge-Nord and Neuallermöhe but no true modern
  plaster-and-glass block. A few photos in `brick_block_1920s` are from Dulsberg and Borner Stieg outside the district,
  because Commons has no usable 1920s block in Bergedorf itself.
- Public and institutional buildings (class 14) have no photos.
