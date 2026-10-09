#!/usr/bin/env bash
# Generates the window interior atlas with ComfyUI (starts it on 127.0.0.1:8189 if it is not running, frees VRAM and
# stops it afterwards). Run under the gpu lock: Scripts/lock.sh gpu Tools/buildingkit/rooms/run_rooms.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-/mnt/storage/third-gear/building_kit/rooms}"
BASE=/mnt/storage/third-gear/building_kit
COMFY_ROOT="${COMFY_ROOT:-$HOME/comfy/ComfyUI}"
VENV="$BASE/comfy/venv"
STARTED_PID=""
if ! curl -s --max-time 2 http://127.0.0.1:8189/system_stats > /dev/null; then
	mkdir -p "$BASE/comfy/out" "$BASE/comfy/in"
	"$VENV/bin/python" "$COMFY_ROOT/main.py" --listen 127.0.0.1 --port 8189 \
		--output-directory "$BASE/comfy/out" --input-directory "$BASE/comfy/in" \
		--disable-all-custom-nodes --disable-auto-launch > "$BASE/comfy/server.log" 2>&1 &
	STARTED_PID=$!
	trap '[ -n "$STARTED_PID" ] && kill "$STARTED_PID" 2>/dev/null || true' EXIT
	for _ in $(seq 1 120); do
		curl -s --max-time 2 http://127.0.0.1:8189/system_stats > /dev/null && break
		sleep 1
	done
fi
python3 "$HERE/make_room_atlas.py" "$OUT"
curl -s -X POST -H 'Content-Type: application/json' -d '{"unload_models": true, "free_memory": true}' http://127.0.0.1:8189/free > /dev/null || true
