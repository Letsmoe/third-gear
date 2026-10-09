#!/usr/bin/env bash
# Headless screenshot from the driver's seat with the car placed on the road at a given pose in the streamed world.
# Usage: Scripts/seat_shot.sh <name> <region> <x> <y> <yaw> [WxH]
#   x, y in metres (x east, y south), yaw in degrees (0 = east, 90 = south). Env CMDS="tg.NightScene 1,..." adds console
#   commands, env EXTRA adds game arguments. Result: Saved/Screenshots/<name>_seat.png
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME=$1 REGION=$2 X=$3 Y=$4 YAW=$5 RES=${6:-1600x900}
export XR_RUNTIME_JSON=/nonexistent/openxr_runtime.json

timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/Streamed -game -RenderOffscreen \
	-nosound -unattended -NoWheel -log=seatshot.log -Region="$REGION" -StartPose="$X,$Y,$YAW" -SeatShot -ShotName="$NAME" \
	-ShotDelay=${SHOT_DELAY:-14} ${EXTRA:-} -ExecCmds="r.SetRes ${RES}w${CMDS:+,$CMDS}" >/dev/null 2>&1 || true
echo "$ROOT/Saved/Screenshots/${NAME}_seat.png"
