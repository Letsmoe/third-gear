#!/usr/bin/env bash
# Records what the real audio mixer plays (procedural car sound through the synth component, ambience loops and thunder
# through the audio engine, with the preferences' volumes) on the scripted ambience scenes, in real time, headless.
# SDL's dummy audio driver stands in for a sound card, so nothing comes out of the speakers. Writes <out>/game_mix.wav.
# Usage: Scripts/audio_mix_test.sh [out_dir] [extra args...]    (add -AudioTest scenes with SCENES=drive)
set -euo pipefail
source "$(dirname "$0")/lock.sh"
hold_lock gpu "$0" "$@"

UE=/mnt/storage/UnrealEngine/5.8.1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-/mnt/storage/third-gear/audio/test_mixer}"
[ $# -gt 0 ] && shift
LOG="$ROOT/Saved/Logs/audiomixtest.log"
mkdir -p "$OUT"
SCENE_ARG="-AmbienceTest"
[ "${SCENES:-ambience}" = "drive" ] && SCENE_ARG=""

SDL_AUDIODRIVER=dummy timeout 900 "$UE/Engine/Binaries/Linux/UnrealEditor" "$ROOT/DrivingGame.uproject" /Game/Maps/ProvingGround -game \
	-nullrhi -unattended -NoWheel -AudioTest $SCENE_ARG -AudioMixerRecord="$OUT" -log=audiomixtest.log \
	-ExecCmds="t.MaxFPS 100" "$@" >/dev/null 2>&1 || true

grep -E 'LogCarAudio|LogAudioMixer.*(Error|Warning)|AudioMixer.*[Nn]ull|SDL' "$LOG" | sed -E 's/^\[[^]]*\]\[[^]]*\]//' | head -20
