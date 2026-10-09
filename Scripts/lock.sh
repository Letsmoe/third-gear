#!/usr/bin/env bash
# Runs a command while holding one of the machine-wide locks, so agents working in parallel worktrees take turns:
#   gpu            screenshots, test runs, headless editor scripts and ComfyUI jobs. There are two slots, so two such
#                  runs can share the 16 GB card; the second slot is only taken while enough VRAM is free.
#   gpu-exclusive  runs that measure frame times or memory (profile_gpu, stream_test, measure_vram): both slots, so
#                  nothing else spoils the numbers.
#   build          Unreal builds; one already uses every core.
# Usage: Scripts/lock.sh <gpu|gpu-exclusive|build> <command...>
# Scripts that start Unreal take the lock themselves (see `hold_lock`); wrap the rest by hand, e.g.
#   Scripts/lock.sh gpu $UE/Engine/Binaries/Linux/UnrealEditor-Cmd ... -run=pythonscript ...
#   Scripts/lock.sh build $UE/Engine/Build/BatchFiles/Linux/Build.sh DrivingGameEditor Linux Development ...
# Nested calls don't deadlock: a lock already held by a parent process (THIRD_GEAR_LOCK_<NAME> set) is reused.
# Every wait and hold is logged to /tmp/third-gear-locks.log.
#
# The command runs in its own systemd scope with a memory cap. systemd's default OOM policy stops the whole unit a
# process belongs to when the kernel OOM-kills it, so an Unreal run that ran out of memory used to take down the
# terminal session (and every agent in it) that started it. In its own scope only the run itself dies.
set -euo pipefail

# Slot 0 keeps the old single lock's file name, so checkouts with an older copy of this script still take turns.
GPU_SLOT_FILES=(/tmp/third-gear-gpu.lock /tmp/third-gear-gpu.1.lock)
# The second slot is only taken when at least this much VRAM and RAM are free.
SECOND_SLOT_FREE_MIB=7000
SECOND_SLOT_FREE_RAM_MIB=14000
# Memory caps of one locked command (its scope), RAM and swap.
MEMORY_MAX_GPU=14G
MEMORY_MAX_BUILD=12G
SWAP_MAX=4G
LOG_FILE=/tmp/third-gear-locks.log

# Re-runs the calling script under the named lock unless this process tree already holds it.
hold_lock() {
	local name=$1
	shift
	if [ -n "$(held_lock_value "$name")" ]; then
		return 0
	fi
	exec "$(dirname "${BASH_SOURCE[0]}")/lock.sh" "$name" "$@"
}

# Name of the environment variable that marks a lock as held by this process tree.
held_lock_variable() {
	local name=${1^^}
	echo "THIRD_GEAR_LOCK_${name//-/_}"
}

# Value of that variable, empty when not held. An exclusive gpu lock also counts as holding a gpu slot.
held_lock_value() {
	local variable
	variable=$(held_lock_variable "$1")
	if [ -n "${!variable:-}" ]; then
		echo 1
	elif [ "$1" = gpu ] && [ -n "${THIRD_GEAR_LOCK_GPU_EXCLUSIVE:-}" ]; then
		echo 1
	fi
}

# Appends one line to the lock log: time, event, lock, seconds, working directory and command.
log_event() {
	printf '%s %-8s %-13s %5ss %s :: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" "$2" "$3" "$PWD" "${*:4}" \
		| cut -c1-300 >>"$LOG_FILE" 2>/dev/null || true
}

# Free VRAM in MiB, or 0 when it can't be read.
free_vram_mib() {
	nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits 2>/dev/null | head -1 || echo 0
}

# Available RAM in MiB.
free_ram_mib() {
	awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo
}

# Runs the command in its own systemd scope with a memory cap and OOMPolicy=continue (see the top of this file).
# Falls back to running it directly where there is no systemd user manager.
run_isolated() {
	local memory_max=$1
	shift
	if ! systemctl --user is-system-running >/dev/null 2>&1 && ! systemctl --user status >/dev/null 2>&1; then
		"$@"
		return
	fi
	systemd-run --user --scope --quiet --collect -p OOMPolicy=continue -p "MemoryMax=$memory_max" \
		-p "MemorySwapMax=$SWAP_MAX" "$@"
}

# Takes one gpu slot on file descriptor 8 or 9, waiting until one is free. Slot 1 also needs enough free VRAM for
# the last 45 seconds: a run that has just taken slot 0 hasn't loaded its map yet.
take_gpu_slot() {
	exec 8>>"${GPU_SLOT_FILES[0]}" 9>>"${GPU_SLOT_FILES[1]}"
	local roomy_checks=0
	while true; do
		# A short blocking wait rather than a bare try: blocked waiters (older copies of this script) otherwise
		# always win slot 0 the moment it is released.
		if flock -w 10 8; then
			exec 9>&-
			GPU_SLOT=0
			return 0
		fi
		if [ "$(free_vram_mib)" -ge "$SECOND_SLOT_FREE_MIB" ] && [ "$(free_ram_mib)" -ge "$SECOND_SLOT_FREE_RAM_MIB" ]; then
			roomy_checks=$((roomy_checks + 1))
		else
			roomy_checks=0
		fi
		if [ "$roomy_checks" -ge 5 ] && flock -n 9; then
			exec 8>&-
			GPU_SLOT=1
			return 0
		fi
	done
}

# Takes every gpu slot, in order, so it can't deadlock against another exclusive run.
take_all_gpu_slots() {
	exec 8>>"${GPU_SLOT_FILES[0]}" 9>>"${GPU_SLOT_FILES[1]}"
	flock 8
	flock 9
}

# Runs the command holding the named lock, saying so when it has to wait.
run_locked() {
	local name=$1
	shift
	if [ -n "$(held_lock_value "$name")" ]; then
		exec "$@"
	fi
	local started=$SECONDS
	case "$name" in
		gpu)
			take_gpu_slot
			name="gpu.$GPU_SLOT"
			;;
		gpu-exclusive)
			take_all_gpu_slots
			;;
		*)
			exec 9>>"/tmp/third-gear-$name.lock"
			if ! flock -n 9; then
				echo "Waiting for the $name lock..." >&2
				flock 9
			fi
			;;
	esac
	local waited=$((SECONDS - started))
	log_event acquired "$name" "$waited" "$@"
	export "$(held_lock_variable "${name%%.*}")=1"
	local status=0
	local memory_max=$MEMORY_MAX_GPU
	if [ "$name" = build ]; then
		memory_max=$MEMORY_MAX_BUILD
	fi
	run_isolated "$memory_max" "$@" || status=$?
	log_event released "$name" "$((SECONDS - started - waited))" "$@"
	return "$status"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
	run_locked "$@"
fi
