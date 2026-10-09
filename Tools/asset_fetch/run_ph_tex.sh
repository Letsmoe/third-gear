#!/bin/bash
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
R=${THIRD_GEAR_DATA:-$ROOT/External}/raw_assets
F=$(cd "$(dirname "$0")/../.." && pwd)/Tools/asset_fetch/ph_fetch.py
python3 -I $F $R tex $(for i in asphalt_pit_lane asphalt_04 asphalt_02 asphalt_01 aerial_asphalt_01 cobblestone_floor_08 cobblestone_floor_02; do echo $i:4k; done)
python3 -I $F $R tex $(for i in brick_pavement brick_floor_003 concrete_pavers concrete_pavers_02 concrete_pavement concrete_pavement_02 brick_pavement_03 large_square_pattern_01 cobblestone_03 cobblestone_floor_09 granite_tile_04 leafy_grass grass_ground forrest_ground_01 gravel_floor_02 gravel_road gravel_ground_01 farm_soil dry_mud_field_001 dirt_floor forest_ground_05 forest_leaves_04 brick_wall_001 brick_wall_006 brick_4 brick_wall_09 exterior_wall_cladding_02 brick_wall_10 white_stucco white_stucco_02 beige_wall_001 beige_wall_002 plastered_wall white_plaster_02 concrete_wall_001 concrete_wall_004 concrete_slab_wall clay_roof_tiles clay_roof_tiles_02 clay_roof_tiles_03 roof_tiles roof_slates_02 grey_roof_tiles corrugated_iron corrugated_iron_02 rusty_corrugated_iron wood_planks wood_planks_grey wood_floor_deck; do echo $i:2k; done)
