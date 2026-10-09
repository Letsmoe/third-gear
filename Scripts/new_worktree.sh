#!/usr/bin/env bash
# Creates a git worktree for a branch under .worktrees/<branch> and links it to the data root (downloads, geodata,
# world data, Python environments, generated and third-party content), so the editor and scripts work there.
# The data root is shared: regenerating content in a worktree changes it for every checkout.
# Usage: Scripts/new_worktree.sh <branch> [base, default main]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRANCH=$1 BASE=${2:-main}
TREE="$ROOT/.worktrees/$BRANCH"

if git -C "$ROOT" show-ref --verify --quiet "refs/heads/$BRANCH"; then
	git -C "$ROOT" worktree add "$TREE" "$BRANCH"
else
	git -C "$ROOT" worktree add "$TREE" -b "$BRANCH" "$BASE"
fi

# Same data root as the root checkout
TARGET="$(readlink -f "$ROOT/External" 2>/dev/null || echo /mnt/storage/third-gear)"
python3 -I "$TREE/Tools/bootstrap/data_root.py" "$TARGET"

# The patched OpenXR plugin is compiled with the project, and Unreal's build accelerator fails on symlinked sources:
# copy it, build output included, so the worktree doesn't have to recompile it.
if [ -d "$ROOT/Plugins/OpenXR" ] && [ ! -e "$TREE/Plugins/OpenXR" ]; then
	cp -a "$ROOT/Plugins/OpenXR" "$TREE/Plugins/OpenXR"
fi
echo "Worktree ready: $TREE (build it before opening the editor there)"
