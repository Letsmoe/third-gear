#!/usr/bin/env bash
# Runs a command while holding one of the machine-wide locks, so agents working in parallel worktrees take turns:
#   gpu    every Unreal game or editor run and every ComfyUI job. They share 16 GB of VRAM and 30 GB of RAM, and two
#          headless games at once also spoil each other's frame times.
#   build  Unreal builds; one already uses every core.
# Usage: Scripts/lock.sh <gpu|build> <command...>
# Scripts that start Unreal take the gpu lock themselves (see `hold_lock`); wrap the rest by hand, e.g.
#   Scripts/lock.sh gpu $UE/Engine/Binaries/Linux/UnrealEditor-Cmd ... -run=pythonscript ...
#   Scripts/lock.sh build $UE/Engine/Build/BatchFiles/Linux/Build.sh DrivingGameEditor Linux Development ...
# Nested calls don't deadlock: a lock already held by a parent process (THIRD_GEAR_LOCK_<NAME> set) is reused.
set -euo pipefail

# Re-runs the calling script under the named lock unless this process tree already holds it.
hold_lock() {
	local name=$1
	shift
	local held_var="THIRD_GEAR_LOCK_${name^^}"
	if [ -n "${!held_var:-}" ]; then
		return 0
	fi
	exec "$(dirname "${BASH_SOURCE[0]}")/lock.sh" "$name" "$@"
}

# Runs the command holding /tmp/third-gear-<name>.lock, saying so when it has to wait.
run_locked() {
	local name=$1
	shift
	local lock_file="/tmp/third-gear-$name.lock"
	local held_var="THIRD_GEAR_LOCK_${name^^}"
	if [ -n "${!held_var:-}" ]; then
		exec "$@"
	fi
	exec 9>>"$lock_file"
	if ! flock -n 9; then
		echo "Waiting for the $name lock ($lock_file)..." >&2
		flock 9
	fi
	export "$held_var=1"
	"$@"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	run_locked "$@"
fi
