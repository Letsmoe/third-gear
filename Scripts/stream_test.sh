#!/usr/bin/env bash
# Runs the free camera through the runtime-generated world at driving speed and prints frame time statistics
# ("STREAMTEST") and the per-tile build and spawn times. Renders offscreen, so frame times include the GPU.
# Usage: Scripts/stream_test.sh [region, default bergedorf_core] ["x0,y0,x1,y1" route in metres] [km/h, default 100]
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REGION=${1:-bergedorf_core} ROUTE=${2:--900,-900,900,900} SPEED=${3:-100}
LOG="$ROOT/Saved/Logs/streamtest.log"
export XR_RUNTIME_JSON=/nonexistent/openxr_runtime.json

timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/Streamed -game -RenderOffscreen \
	-nosound -unattended -NoWheel -Region="$REGION" -StreamTest="$ROUTE" -StreamSpeed="$SPEED" -log=streamtest.log \
	-ExecCmds="r.SetRes 1600x900w" ${EXTRA:-} >/dev/null 2>&1 || true

grep -E 'STREAMTEST' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]LogStreamTest: Display: //'
grep -E 'LogWorldStreamer: Tile' "$LOG" |
	sed -E 's/.*detail ([0-9]): built in ([0-9]+) ms, spawned in ([0-9.]+) ms over ([0-9]+) steps, longest step ([0-9.]+) ms/\1 \2 \3 \5/' |
	awk '{n[$1]++; b[$1]+=$2; s[$1]+=$3; if ($4>m[$1]) m[$1]=$4} END {for (d in n) printf "detail %s: %d tiles, build %.0f ms average, spawn %.1f ms average in total, longest single step %.1f ms\n", d, n[d], b[d]/n[d], s[d]/n[d], m[d]}' | sort
