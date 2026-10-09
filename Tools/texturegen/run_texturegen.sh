#!/usr/bin/env bash
# Texture generation batch: holds the gpu lock, starts ComfyUI (private port, our custom nodes) if none of ours is
# running, runs texturegen.py for at most the time budget, frees VRAM and stops the server it started.
# Usage: Tools/texturegen/run_texturegen.sh [texturegen.py options]   (e.g. --only asphalt_fresh --draft)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
if [ -z "${THIRD_GEAR_LOCK_GPU:-}" ]; then
	exec "$REPO/Scripts/lock.sh" gpu "$0" "$@"
fi

DATA="${THIRD_GEAR_DATA:-/mnt/storage/third-gear}"
WORK="$DATA/texturegen/_comfy"
VENV="$DATA/building_kit/comfy/venv"
COMFY_ROOT="${COMFY_ROOT:-$HOME/comfy/ComfyUI}"
PORT=8190
export TEXTUREGEN_SERVER="http://127.0.0.1:$PORT"
STARTED_PID=""

for other_port in 8188 8189; do
	if curl -s --max-time 2 "http://127.0.0.1:$other_port/system_stats" > /dev/null; then
		echo "Another ComfyUI answers on port $other_port; only one server at a time. Stop it first." >&2
		exit 1
	fi
done

stop_server() {
	if curl -s --max-time 2 "$TEXTUREGEN_SERVER/system_stats" > /dev/null; then
		curl -s --max-time 20 -X POST -H 'Content-Type: application/json' \
			-d '{"unload_models": true, "free_memory": true}' "$TEXTUREGEN_SERVER/free" > /dev/null || true
	fi
	if [ -n "$STARTED_PID" ]; then
		kill "$STARTED_PID" 2>/dev/null || true
		wait "$STARTED_PID" 2>/dev/null || true
	fi
}
trap stop_server EXIT

mkdir -p "$WORK/out" "$WORK/in"
cat > "$WORK/extra_paths.yaml" <<YAML
thirdgear:
  base_path: $HERE
  custom_nodes: comfy_nodes
YAML

if ! curl -s --max-time 2 "$TEXTUREGEN_SERVER/system_stats" > /dev/null; then
	"$VENV/bin/python" "$COMFY_ROOT/main.py" --listen 127.0.0.1 --port "$PORT" \
		--output-directory "$WORK/out" --input-directory "$WORK/in" \
		--extra-model-paths-config "$WORK/extra_paths.yaml" --disable-auto-launch > "$WORK/server.log" 2>&1 &
	STARTED_PID=$!
	for _ in $(seq 1 180); do
		curl -s --max-time 2 "$TEXTUREGEN_SERVER/system_stats" > /dev/null && break
		sleep 1
	done
fi

"$VENV/bin/python" "$HERE/texturegen.py" "$@"
