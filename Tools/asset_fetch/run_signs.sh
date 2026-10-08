#!/bin/bash
R=$(cd "$(dirname "$0")/../.." && pwd)/RawAssets
while pgrep -f "signs_fetch.py" >/dev/null; do sleep 15; done
until python3 -u -I $(cd "$(dirname "$0")/../.." && pwd)/Tools/asset_fetch/signs_fetch.py $R; do echo "failed, sleeping"; sleep 400; done
