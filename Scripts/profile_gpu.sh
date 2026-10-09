#!/usr/bin/env bash
# Headless GPU profile at VR resolution with emulated stereo: warms up, runs ProfileGPU, quits.
# Prints the top-level GPU passes from the log.
# Usage: Scripts/profile_gpu.sh <label> ["console cmd, console cmd"] [extra args...]
#   MAP=<map name> (default ProvingGround; Streamed needs -Region=bergedorf_core as an extra argument)
#   WARMUP=<seconds> (default 25), RES=<WxH> (default 5056x2704 = Quest 3 via Steam Link, both eyes)
# Run with the VR game closed, otherwise both compete for the GPU.
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu-exclusive "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABEL=$1 CMDS=${2:-}
shift $(( $# < 2 ? $# : 2 ))
RES=${RES:-5056x2704}
WARMUP=${WARMUP:-25}
LOG="$ROOT/Saved/Logs/gpu_$LABEL.log"

timeout 300 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" "/Game/Maps/${MAP:-ProvingGround}" -game \
	-RenderOffscreen -emulatestereo -nosound -unattended -log="gpu_$LABEL.log" -ProfileGPUAfter="$WARMUP" \
	-ExecCmds="r.SetRes ${RES}w, r.ProfileGPU.ShowUI 0${CMDS:+, $CMDS}" "$@" >/dev/null 2>&1 || true

# ProfileGPU prints an indented tree; keep the frame total and the passes up to two levels deep.
grep -E 'LogRHI: +([0-9.]+%|.*Total GPU Time|.*GPU Profile for)' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]LogRHI: //' \
	| awk -v label="$LABEL" 'BEGIN{print "== " label " =="} /^ {0,6}[0-9]/ || /Total|Profile/ {print}' | head -60
