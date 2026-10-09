#!/usr/bin/env bash
# Regenerates the whole building kit: pieces, GLBs, .blend files, contact sheets and assembled test houses.
# Add --props to also run the Hunyuan prop pipeline (starts and stops its own ComfyUI).
# Usage: Tools/buildingkit/build_all.sh [--props] [output_dir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
RUN_PROPS=0
if [ "${1:-}" = "--props" ]; then
	RUN_PROPS=1
	shift
fi
OUT="${1:-/mnt/storage/third-gear/building_kit}"

blender -b --factory-startup --python-exit-code 1 -P "$HERE/generate.py" -- "$OUT"
blender -b --factory-startup --python-exit-code 1 -P "$HERE/assemble.py" -- "$OUT"
if [ "$RUN_PROPS" = 1 ]; then
	"$HERE/props/run_props.sh" "$OUT"
fi
echo "Kit written to $OUT"
