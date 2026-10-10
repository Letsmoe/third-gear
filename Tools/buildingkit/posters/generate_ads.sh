#!/usr/bin/env bash
# Generates the shelter advertising posters with Codex's image tool, one Codex run per poster, then crops and scales
# each to the CityLight ratio. Prompts are in prompts.py; output goes to <data root>/building_kit/posters/.
# Usage: Tools/buildingkit/posters/generate_ads.sh [name ...]   (default: every poster that doesn't exist yet)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${THIRD_GEAR_DATA:-/mnt/storage/third-gear}"
OUT="$DATA/building_kit/posters"
RAW="$OUT/raw"
PYTHON="$DATA/venvs/osmimport/bin/python"
mkdir -p "$RAW"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
	mapfile -t names < <(python3 -I -c "import sys; sys.path.insert(0, '$HERE'); import prompts; print('\n'.join(prompts.ADS))")
fi
for name in "${names[@]}"; do
	# Without names, posters that were already generated are only fitted again; naming one regenerates it.
	if [ ! -e "$RAW/$name.png" ] || [ $# -gt 0 ]; then
		prompt=$(python3 -I -c "import sys; sys.path.insert(0, '$HERE'); import prompts; print(prompts.FORMAT + ' ' + prompts.ADS['$name'])")
		echo "== $name"
		codex exec --skip-git-repo-check -s workspace-write -C "$RAW" \
			"Generate one image with your image generation tool. $prompt Save the final image as $name.png in the current directory." \
			>"$RAW/$name.log" 2>&1
	fi
	"$PYTHON" -I "$HERE/fit_poster.py" "$RAW/$name.png" "$OUT/ad_$name.png"
done
