#!/usr/bin/env bash
# Headless test of AI traffic: the free camera rides along random lanes while cars spawn around it, and the run prints
# the "TRAFFICTEST" lines of Source/DrivingGame/TrafficTest.cpp (violations by AI cars, collisions between AI cars, cars
# stuck for a minute, frame times) plus every AITRAFFIC and RULECHECK line the cars logged.
# Usage: Scripts/traffic_test.sh [region, default bergedorf_test] [minutes, default 10]
# Env EXTRA adds game arguments, e.g. EXTRA="-NoTraffic" for the frame time without traffic or "-TrafficCars=60".
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REGION=${1:-bergedorf_test} MINUTES=${2:-10}
LOG="$ROOT/Saved/Logs/traffictest.log"
export XR_RUNTIME_JSON=/nonexistent/openxr_runtime.json

# Env GDB=1 runs the game under gdb and prints a backtrace when it crashes.
RUNNER=()
if [ -n "${GDB:-}" ]; then
	RUNNER=(gdb -batch -ex "handle SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass" -ex run -ex "bt 40" -ex "thread apply all bt 8" --args)
fi

timeout 3000 "${RUNNER[@]}" "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/Streamed -game -nullrhi -nosound \
	-unattended -NoWheel -benchmark -fps=30 -Region="$REGION" -TrafficTest="$MINUTES" -log=traffictest.log ${EXTRA:-} >"$ROOT/Saved/Logs/traffictest_stdout.log" 2>&1 || true

grep -E 'TRAFFICTEST|AITRAFFIC|RULECHECK|LogAITraffic' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//'
