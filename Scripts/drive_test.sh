#!/usr/bin/env bash
# Headless drivetrain/handling test of the player car on the ProvingGround map (no window, no GPU rendering).
# Drives scripted runs (idle, 0-100/top speed, 50 km/h per gear, clutch starts, stall, 100-0 braking, ramp steer)
# and prints the "DRIVETEST" result lines. Tuning lives in Config/DefaultGame.ini [/Script/DrivingGame.CarSettings];
# no rebuild is needed after changing it.
# Usage: Scripts/drive_test.sh [extra args...]
set -euo pipefail

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$ROOT/Saved/Logs/drivetest.log"

# -benchmark -fps=100: fixed 10 ms game frames regardless of real time (physics runs at its fixed 2 ms step).
# -NoWheel: don't open (or apply force feedback to) a connected wheel.
timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/ProvingGround -game \
	-nullrhi -nosound -unattended -NoWheel -DriveTest -benchmark -fps=100 -log=drivetest.log "$@" >/dev/null 2>&1 || true

grep -E 'DRIVETEST' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]LogDriveTest: Display: DRIVETEST //'
