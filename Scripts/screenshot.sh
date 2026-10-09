#!/usr/bin/env bash
# Headless screenshots of a map from given viewpoints (desktop render, not stereo).
# Usage: Scripts/screenshot.sh <map name or /full/package/path> "<x,y,z,pitch,yaw;...>" [WxH]
#   Coordinates in metres in world space (x east, y south, z up), pitch/yaw in degrees (yaw 0 = east, 90 = south).
# Optional env SHOT_DELAY=<seconds before the first shot, default 20>, CMDS="cvar 1,cvar2 0" adds console commands.
# Images land in Saved/Screenshots/shot_NN.png (overwritten each run).
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAP=$1 SHOTS=$2 RES=${3:-1600x900}
case $MAP in /*) MAP_PATH=$MAP ;; *) MAP_PATH=/Game/Maps/$MAP ;; esac
OUT="$ROOT/Saved/Screenshots"
mkdir -p "$OUT"
MARKER=$(mktemp)
# Desktop render: skip the OpenXR runtime (see run_desktop.sh)
export XR_RUNTIME_JSON=/nonexistent/openxr_runtime.json

timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" "$MAP_PATH" -game -RenderOffscreen \
	-nosound -unattended -log=screenshot.log -Shots="$SHOTS" -ShotDelay=${SHOT_DELAY:-20} ${EXTRA:-} \
	-ExecCmds="r.SetRes ${RES}w${CMDS:+,$CMDS}" >/dev/null 2>&1 || true

find "$OUT" -newer "$MARKER" -name '*.png' | sort
rm -f "$MARKER"
