#!/usr/bin/env bash
# Runs the game standalone in VR (much less GPU memory than the editor's VR Preview).
# Start SteamVR + Steam Link first. Close the editor first if possible.
# Usage: [MAP=<map name>] Scripts/run_vr.sh [screen percentage, default 85] [extra args...]
#   MAP defaults to Streamed; the start menu floats in front of you (D-pad and buttons on the wheel), -NoMenu skips it.
#   Screen percentage < 100 renders fewer pixels and lets TSR upscale (saves VRAM + GPU time).
set -euo pipefail

UE=/mnt/storage/UnrealEngine/5.8.1
PROJECT="$(cd "$(dirname "$0")/.." && pwd)/DrivingGame.uproject"
SCREEN_PERCENTAGE=${1:-85}
shift $(( $# > 0 ? 1 : 0 ))

if ! pgrep -x vrserver >/dev/null; then
	echo "SteamVR doesn't seem to be running - start it (and Steam Link) first." >&2
	exit 1
fi

exec "$UE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" "/Game/Maps/${MAP:-Streamed}" -game -vr -log \
	-ExecCmds="r.ScreenPercentage $SCREEN_PERCENTAGE" "$@"
