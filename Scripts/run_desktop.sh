#!/usr/bin/env bash
# Runs the game standalone in a desktop window (no VR) - the quickest way to look around a map.
# Usage: [MAP=<map name>] Scripts/run_desktop.sh [extra args...]
#   e.g. MAP=bergedorf_core Scripts/run_desktop.sh
#   You sit in the car (keys in CLAUDE.md "Car & wheel"). -FreeCam flies instead: mouse looks, WASD moves,
#   Q/E down/up, Shift = 50 km/h; add -SpawnCar to park the car at the start. `~` opens the console (stat fps, stat unit).
set -euo pipefail

UE=/mnt/storage/UnrealEngine/5.8.1
PROJECT="$(cd "$(dirname "$0")/.." && pwd)/DrivingGame.uproject"
# No VR here: point the OpenXR loader at a runtime that doesn't exist, so the OpenXR plugin fails at once instead of
# trying to start SteamVR four times at launch (about 5 s each, and it looks like crashes in the console).
export XR_RUNTIME_JSON=/nonexistent/openxr_runtime.json

exec "$UE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" "/Game/Maps/${MAP:-bergedorf_core}" -game -windowed \
	-ResX=1920 -ResY=1080 -log "$@"
