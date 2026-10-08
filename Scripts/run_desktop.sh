#!/usr/bin/env bash
# Runs the game standalone in a desktop window (no VR) - the quickest way to look around a map.
# Usage: [MAP=<map name>] Scripts/run_desktop.sh [extra args...]
#   e.g. MAP=bergedorf_core Scripts/run_desktop.sh
#   Mouse looks around, WASD moves, Q/E down/up, Shift = 50 km/h. `~` opens the console (stat fps, stat unit).
set -euo pipefail

UE=/mnt/storage/UnrealEngine/5.8.1
PROJECT="$(cd "$(dirname "$0")/.." && pwd)/DrivingGame.uproject"

exec "$UE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" "/Game/Maps/${MAP:-bergedorf_core}" -game -windowed \
	-ResX=1920 -ResY=1080 -log "$@"
