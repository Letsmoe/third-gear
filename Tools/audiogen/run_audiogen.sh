#!/usr/bin/env bash
# Generates the world and weather sounds into the data root (raw takes), then turns them into loops and one-shots.
# Takes the gpu lock for the ComfyUI run. Usage: Tools/audiogen/run_audiogen.sh [sound_id ...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
AUDIO_ROOT="${AUDIO_ROOT:-/mnt/storage/third-gear/audio}"
mkdir -p "$AUDIO_ROOT/raw"
"$HERE/../../Scripts/lock.sh" gpu python3 -I "$HERE/generate.py" "$AUDIO_ROOT/raw" "$@"
python3 -I "$HERE/postprocess.py" "$AUDIO_ROOT" "$@"
