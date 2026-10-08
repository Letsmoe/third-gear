#!/usr/bin/env bash
# Creates Plugins/OpenXR: a copy of the engine's OpenXR plugin with our fix applied
# (Patches/openxr-swapchain-flags.patch). Project plugins override engine plugins of the same name.
# Without it, VR hangs on frame 3 with SteamVR on Linux. Re-run after an engine upgrade.
set -euo pipefail

UE=${UE:-/mnt/storage/UnrealEngine/5.8.1}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="$ROOT/Plugins/OpenXR"

rm -rf "$TARGET"
cp -r "$UE/Engine/Plugins/Runtime/OpenXR" "$TARGET"
chmod -R u+w "$TARGET"
rm -rf "$TARGET/Binaries" "$TARGET/Intermediate"
patch -d "$TARGET" -p1 < "$ROOT/Patches/openxr-swapchain-flags.patch"
echo "Patched OpenXR plugin in $TARGET"
