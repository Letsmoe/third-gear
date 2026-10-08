#!/usr/bin/env bash
# Headless screenshots of a map from given viewpoints (desktop render, not stereo).
# Usage: Scripts/screenshot.sh <map name or /full/package/path> "<x,y,z,pitch,yaw;...>" [WxH]
#   Coordinates in metres in world space (x east, y south, z up), pitch/yaw in degrees (yaw 0 = east, 90 = south).
# Optional env CMDS="cvar 1,cvar2 0" adds console commands.
# Images land in Saved/Screenshots/shot_NN.png (overwritten each run).
set -euo pipefail

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAP=$1 SHOTS=$2 RES=${3:-1600x900}
case $MAP in /*) MAP_PATH=$MAP ;; *) MAP_PATH=/Game/Maps/$MAP ;; esac
OUT="$ROOT/Saved/Screenshots"
mkdir -p "$OUT"
MARKER=$(mktemp)

timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" "$MAP_PATH" -game -RenderOffscreen \
	-nosound -unattended -log=screenshot.log -Shots="$SHOTS" -ShotDelay=20 ${EXTRA:-} \
	-ExecCmds="r.SetRes ${RES}w${CMDS:+,$CMDS}" >/dev/null 2>&1 || true

find "$OUT" -newer "$MARKER" -name '*.png' | sort
rm -f "$MARKER"
