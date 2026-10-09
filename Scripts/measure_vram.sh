#!/usr/bin/env bash
# Measures peak GPU memory of the game rendered offscreen at a given resolution with optional console commands.
# Usage: Scripts/measure_vram.sh <label> <resx> <resy> [comma-separated console commands] [extra args...]
# Default resolution matches the Quest 3 / Steam Link stereo target seen in the log (5056x2704, both eyes).
set -euo pipefail
# One Unreal run at a time across worktrees (see lock.sh).
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
PROJECT="$(cd "$(dirname "$0")/.." && pwd)/DrivingGame.uproject"
LABEL=$1 RESX=${2:-5056} RESY=${3:-2704} CMDS=${4:-}
shift $(( $# < 4 ? $# : 4 ))
WAIT=${WAIT:-40}

"$UE/Engine/Binaries/Linux/UnrealEditor" "$PROJECT" /Game/Maps/ProvingGround -game -RenderOffscreen \
	-ResX="$RESX" -ResY="$RESY" -nosound -unattended -log="vram_$LABEL.log" -ExecCmds="r.SetRes ${RESX}x${RESY}w${CMDS:+, $CMDS}" "$@" >/dev/null 2>&1 &
PID=$!

PEAK=0
for _ in $(seq "$WAIT"); do
	sleep 1
	kill -0 "$PID" 2>/dev/null || { echo "$LABEL: process exited early (see Saved/Logs/vram_$LABEL.log)"; exit 1; }
	USED=$(nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits | awk -F', ' -v p="$PID" '$1==p {print $2}')
	USED=${USED:-0}
	(( USED > PEAK )) && PEAK=$USED
done
kill "$PID"; wait "$PID" 2>/dev/null || true
echo "$LABEL: peak ${PEAK} MiB at ${RESX}x${RESY} [$CMDS]"
