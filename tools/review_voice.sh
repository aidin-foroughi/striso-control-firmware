#!/usr/bin/env bash
# Review a voice engine from its worktree before it may be flashed: firmware build with all guards,
# host unit tests of every engine (ctest: finite, level, stale input, parameter fuzz, golden
# renders), a host render of the default performance (finite, level), and the branch state.
# Usage: [WT=<worktree>] [BASE=<branch>] tools/review_voice.sh <voice>     (VOICE=<voice>)
#   WT: the worktree to review, relative to this repository's root or absolute; default
#   ../worktrees/fw-<voice> (the worktrees live next to the firmware checkout).
#   BASE: the branch the voice started from (default main), for the log and the diff.
# Leaves the checked firmware at $WT/build/firmware/striso_control.uf2. Does not flash.
set -euo pipefail
V="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WT="${WT:-../worktrees/fw-$V}"   # override for voices living in another worktree (e.g. WT=../worktrees/fw-reed for wind)
BASE="${BASE:-main}"
cd "$ROOT"
[ -d "$WT" ] || { echo "!! no worktree $WT (set WT=...)"; exit 1; }
echo "== branch"
git -C "$WT" log --oneline -5 "$BASE"..HEAD
git -C "$WT" status --short || true
echo "== files changed vs $BASE"
git -C "$WT" diff --stat "$BASE"..HEAD
echo "== firmware build"
FW_DIR="$WT" tools/build.sh VOICE="$V" > "tools/.review_$V.log" 2>&1 || { tail -30 "tools/.review_$V.log"; echo "!! BUILD FAILED"; exit 1; }
grep -E 'ram0|ram5|stack \(|passed|warning: .*voice_' "tools/.review_$V.log" || true
grep -q 'build checks passed' "tools/.review_$V.log" || { echo "!! BUILD CHECKS DID NOT PASS"; exit 1; }
echo "== host unit tests"
FW_DIR="$WT" tools/test.sh > "tools/.review_${V}_tests.log" 2>&1 || { tail -40 "tools/.review_${V}_tests.log"; echo "!! HOST TESTS FAILED"; exit 1; }
grep -E 'tests passed|tests failed' "tools/.review_${V}_tests.log" || true
echo "== host render (default performance)"
FW_DIR="$WT" VOICE="$V" tools/harness/render_strisy.sh "review_$V" 2>&1 | grep -E 'error|NON-FINITE|peak' || true
tools/.venv/Scripts/python - "$V" <<'EOF'
import sys
import numpy as np
v = sys.argv[1]
x = np.fromfile(f'tools/harness/out/review_{v}.f32', dtype=np.float32)
fin = np.isfinite(x).all()
peak = float(np.abs(x).max()) if fin else float('nan')
rms = float(np.sqrt(np.mean(x.astype(float) ** 2))) if fin else float('nan')
print(f"finite: {fin}  peak {peak:.2f}  rms {rms:.3f}  (strisy: peak 4.6, rms ~0.6)")
if not fin or peak > 8:
    print("!! LEVEL/FINITE CHECK FAILED")
    sys.exit(1)
EOF
tools/.venv/Scripts/python tools/harness/compare.py "tools/harness/out/review_$V.f32" "tools/harness/out/review_$V.f32" --wav > /dev/null
echo "review OK: firmware at $WT/build/firmware/striso_control.uf2, listen: tools/harness/out/review_$V.wav"
