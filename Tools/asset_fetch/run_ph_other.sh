#!/bin/bash
R=$(cd "$(dirname "$0")/../.." && pwd)/RawAssets
F=$(cd "$(dirname "$0")/../.." && pwd)/Tools/asset_fetch/ph_fetch.py
while pgrep -f run_ph_tex.sh >/dev/null; do sleep 20; done
python3 -u -I $F $R hdri binnenalster:4k hausdorf_meadow:4k hausdorf_clear_sky:4k hamburg_canal:4k evening_road_01:4k narrow_moonlit_road:4k suburban_field_01:2k
python3 -u -I $F $R model $(for i in street_lamp_01 street_lamp_02 fire_hydrant utility_box_01 utility_box_02 metal_trash_can modular_street_seating painted_wooden_bench wooden_picnic_table concrete_road_barrier concrete_road_barrier_02 modular_chainlink_fence modular_electricity_poles water_manhole_cover tree_small_02 fir_tree_01 pine_tree_01 dead_tree_trunk tree_stump_01 tree_stump_02 shrub_01 shrub_02 shrub_03 shrub_04 grass_medium_01 grass_medium_02 fern_02 dandelion_01 celandine_01 periwinkle_plant nettle_plant weed_plant_02 boulder_01 rock_07 rock_09 stone_01 rock_moss_set_01 rock_moss_set_02 planter_box_01 security_light covered_car old_tyre large_iron_gate modular_urban_apartments_facade exterior_aircon_unit modular_metal_gutter outdoor_table_chair_set_01 korean_public_payphone_01; do echo $i:2k; done)
