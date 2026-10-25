#!/usr/bin/env bash
# Rewrite the golden render checksums from the current DSP and toolchain: run after an intended
# change of the engines' output, or after a toolchain update (and say in the commit message what
# changed and why). In Docker:
#   tests/golden/renders.txt         host renders (ctest -L render, preset host)
#   tests/golden/target_renders.txt  the DSP compiled for the board, run on QEMU (ctest -L
#                                    target_render, preset firmware)
#   tests/golden/control_replay.txt  the recorded key streams through synth_control.cpp (ctest -L
#                                    control_replay, preset host; only with 'control')
# The renderers write their hashes instead of comparing them (AWVA_GOLDEN_OUT); the other checks
# (finite, peak, silent stale tail) still apply and must pass.
# Usage: [FW_DIR=<checkout>] tools/update_goldens.sh [host|target|control]   (default: host and target)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"
WHAT="${1:-both}"
update() {  # <preset> <label> <golden file> <build target>
  image_build_dir "build/$1"
  in_docker bash -c "
set -e
cmake --preset $1 > /dev/null && cmake --build --preset $1 --target $4 > /tmp/build.log 2>&1 || { tail -30 /tmp/build.log; exit 1; }
rm -rf /tmp/golden && mkdir /tmp/golden
AWVA_GOLDEN_OUT=/tmp/golden ctest --preset $1 -L '^$2\$' -j\$(nproc) > /tmp/golden.log 2>&1 || { grep -E 'Failed|!!|FATAL' /tmp/golden.log | head -30; echo '!! render checks failed: goldens not written'; exit 1; }
{ sed -n '/^#/p' $3; cat /tmp/golden/*.hash | LC_ALL=C sort; } > /tmp/new.txt
mv /tmp/new.txt $3
echo \"\$(ls /tmp/golden | wc -l) golden checksums written to $3\""
}
if [ "$WHAT" = control ]; then
  update host control_replay tests/golden/control_replay.txt test_synth_control
  exit 0
fi
[ "$WHAT" = target ] || update host render tests/golden/renders.txt all
[ "$WHAT" = host ] || update firmware target_render tests/golden/target_renders.txt target_renders
