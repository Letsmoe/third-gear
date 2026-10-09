#!/usr/bin/env bash
# Prop pipeline: FLUX.2 klein reference images, Hunyuan 3D 2.1 meshes, Blender clean-up and export.
# Starts ComfyUI on 127.0.0.1:8189 from a private venv and stops it again, unless one is already listening.
# The venv is created on first use (about 20 minutes, torch is large) from ComfyUI's requirements.
# Usage: Tools/buildingkit/props/run_props.sh [output_dir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/mnt/storage/third-gear/building_kit}"
COMFY_ROOT="${COMFY_ROOT:-$HOME/comfy/ComfyUI}"
VENV="$OUT/comfy/venv"
STARTED_PID=""

if [ ! -x "$VENV/bin/python" ]; then
	mkdir -p "$OUT/comfy"
	uv venv --python 3.13 "$VENV"
	uv pip install --python "$VENV/bin/python" --index-url https://download.pytorch.org/whl/cu130 \
		--extra-index-url https://pypi.org/simple --index-strategy unsafe-best-match -r "$COMFY_ROOT/requirements.txt"
fi

if ! curl -s --max-time 2 http://127.0.0.1:8189/system_stats > /dev/null; then
	mkdir -p "$OUT/comfy/out" "$OUT/comfy/in"
	"$VENV/bin/python" "$COMFY_ROOT/main.py" --listen 127.0.0.1 --port 8189 \
		--output-directory "$OUT/comfy/out" --input-directory "$OUT/comfy/in" \
		--disable-all-custom-nodes --disable-auto-launch > "$OUT/comfy/server.log" 2>&1 &
	STARTED_PID=$!
	trap '[ -n "$STARTED_PID" ] && kill "$STARTED_PID" 2>/dev/null || true' EXIT
	for _ in $(seq 1 120); do
		curl -s --max-time 2 http://127.0.0.1:8189/system_stats > /dev/null && break
		sleep 1
	done
fi

python3 "$HERE/make_reference_images.py" "$OUT/props/ref"
python3 "$HERE/make_meshes.py" "$OUT/props/ref" "$OUT/props/raw"
blender -b --factory-startup --python-exit-code 1 -P "$HERE/process_props.py" -- "$OUT/props/raw" "$OUT"
