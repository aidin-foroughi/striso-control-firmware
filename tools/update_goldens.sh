#!/usr/bin/env bash
# Rewrite tests/golden/renders.txt from the current DSP: run after an intended change of the
# engines' output (and say in the commit message what changed and why). Runs every render test in
# Docker (preset host) with STRISO_GOLDEN_OUT set, so the renderers write their hashes instead of
# comparing them. The other checks (finite, peak, silent stale tail) still apply.
# Usage: [FW_DIR=<checkout>] tools/update_goldens.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"
in_docker bash -c '
set -e
cmake --preset host > /dev/null && cmake --build --preset host
rm -rf /tmp/golden && mkdir /tmp/golden
STRISO_GOLDEN_OUT=/tmp/golden ctest --preset host -L render -j$(nproc) > /tmp/golden.log 2>&1 || { tail -30 /tmp/golden.log; echo "!! render checks failed: goldens not written"; exit 1; }
{ sed -n "/^#/p" tests/golden/renders.txt; cat /tmp/golden/*.hash | LC_ALL=C sort; } > /tmp/renders.txt
mv /tmp/renders.txt tests/golden/renders.txt
echo "$(ls /tmp/golden | wc -l) golden renders written to tests/golden/renders.txt"
'
