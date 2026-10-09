#!/usr/bin/env bash
# Records what the car sounds like on a scripted drive, without a sound card: runs the AudioTest scenes headless and
# renders the car sound offline (same DSP as the game, fixed 10 ms steps) to <out>/car.wav, with a telemetry CSV.
# Then makes the spectrograms and checks (Tools/audiogen/analyze.py). Default output is the data root's audio/test.
# Usage: Scripts/audio_test.sh [out_dir] [extra args...]
set -euo pipefail
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-/mnt/storage/third-gear/audio/test}"
[ $# -gt 0 ] && shift
LOG="$ROOT/Saved/Logs/audiotest.log"
mkdir -p "$OUT"

timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/${AUDIO_MAP:-ProvingGround} -game \
	-nullrhi -nosound -unattended -NoWheel -AudioTest -AudioCapture="$OUT" -benchmark -fps=100 -log=audiotest.log "$@" >/dev/null 2>&1 || true

grep -E 'DRIVETEST|LogCarAudio' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | tail -40
