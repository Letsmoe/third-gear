#!/usr/bin/env bash
# Headless GPU profile at VR resolution with emulated stereo: warms up, runs ProfileGPU, quits.
# Prints the top-level GPU passes from the log.
# Usage: Scripts/profile_gpu.sh <label> ["console cmd, console cmd"] [extra args...]
#   MAP=<map name> (default ProvingGround; Streamed needs -Region=bergedorf_core as an extra argument)
#   WARMUP=<seconds> (default 25), RES=<WxH> (default 5056x2704 = Quest 3 via Steam Link, both eyes)
#   DEPTH and MIN_MS limit the printed pass tree (default 10 and 0.3 ms).
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

# Summarise the ProfileGPU table: frame time, then passes down to the scene's main stages that take at least 0.3 ms.
echo "== $LABEL =="
python3 -I "$ROOT/Scripts/parse_profilegpu.py" "$LOG" "${DEPTH:-10}" "${MIN_MS:-0.3}" | head -60
