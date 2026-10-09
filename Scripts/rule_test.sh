#!/usr/bin/env bash
# Headless test of the rule checker (red light, amber, speeding) on the real signals and roads of a region; prints the
# "RULETEST" lines of Source/DrivingGame/RuleTest.cpp.
# Usage: Scripts/rule_test.sh [region, default bergedorf_core]
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REGION=${1:-bergedorf_core}
LOG="$ROOT/Saved/Logs/ruletest.log"
export XR_RUNTIME_JSON=/nonexistent/openxr_runtime.json

timeout 300 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/Streamed -game -nullrhi -nosound \
	-unattended -NoWheel -benchmark -fps=100 -Region="$REGION" -RuleTest -log=ruletest.log ${EXTRA:-} >/dev/null 2>&1 || true

grep -E 'RULETEST|RULECHECK' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//'
