#!/usr/bin/env bash
# Creates a git worktree for a branch under .worktrees/<branch> and links in everything that is not in git
# (generated and third-party content, downloads, the patched OpenXR plugin), so the editor and scripts work there.
# Generated content is shared with the root through these links: regenerating it in a worktree changes it everywhere.
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

# Untracked paths shared from the root (see .gitignore)
SHARED=(
	RawAssets GeoData/raw GeoData/osm GeoData/build DerivedDataCache
	Content/CitySampleVehicles Content/Textures Content/World Content/Vegetation
	Content/Vehicles/SportsCar Content/Vehicles/PhysicsMaterials
)
for path in "${SHARED[@]}"; do
	if [ -e "$ROOT/$path" ] && [ ! -e "$TREE/$path" ]; then
		mkdir -p "$(dirname "$TREE/$path")"
		ln -s "$ROOT/$path" "$TREE/$path"
	fi
done
# The patched OpenXR plugin is compiled with the project, and Unreal's build accelerator fails on symlinked sources:
# copy it, build output included, so the worktree doesn't have to recompile it.
if [ -d "$ROOT/Plugins/OpenXR" ] && [ ! -e "$TREE/Plugins/OpenXR" ]; then
	cp -a "$ROOT/Plugins/OpenXR" "$TREE/Plugins/OpenXR"
fi
# Generated maps are single files next to the tracked ProvingGround map
for map in "$ROOT"/Content/Maps/*.umap; do
	name=$(basename "$map")
	if [ ! -e "$TREE/Content/Maps/$name" ]; then
		ln -s "$map" "$TREE/Content/Maps/$name"
	fi
done
for venv in "$ROOT"/Tools/*/.venv; do
	rel=${venv#"$ROOT/"}
	if [ ! -e "$TREE/$rel" ]; then
		ln -s "$venv" "$TREE/$rel"
	fi
done
echo "Worktree ready: $TREE (build it before opening the editor there)"
