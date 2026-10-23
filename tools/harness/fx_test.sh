#!/usr/bin/env bash
# Build the fx host test (tools/harness/fx_test.cpp) in Docker and run it.
# Usage: [FW_DIR=<checkout>] [VOICE=strisy|va|...] [PERF=perf_fx.h] tools/harness/fx_test.sh <out-name> [fx_test args...]
#   FW_DIR: firmware checkout, relative to the current directory or absolute (default: this
#     repository; e.g. FW_DIR=../worktrees/fw-fx from the repository root)
#   output goes to tools/harness/out/<out-name>.{f32,wav}; several runs: separate them with ' ;; '
#   e.g. tools/harness/fx_test.sh fx_a rvmix=0.3 ';;' fx_b src=impulse rvmix=1
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS="$(cd "$HERE/.." && pwd -W 2>/dev/null || pwd)"
FW="$(cd "${FW_DIR:-$HERE/../..}" && pwd -W 2>/dev/null || pwd)"
mkdir -p "$HERE/out"
VDEF=""
[ -z "${VOICE:-}" ] || [ "$VOICE" = strisy ] || VDEF="-DSYNTH_VOICE_HEADER=\\\"voice_$VOICE.h\\\" -DSYNTH_VOICE_NS=$VOICE"
# split the arguments into runs at ';;'
RUNS=""
CUR="/tmp/fx /tools/harness/out/$1"
shift
for a in "$@"; do
  if [ "$a" = ";;" ]; then RUNS="$RUNS $CUR && "; CUR="/tmp/fx"; FIRST=1; continue; fi
  if [ "${FIRST:-0}" = 1 ]; then CUR="$CUR /tools/harness/out/$a"; FIRST=0; else CUR="$CUR '$a'"; fi
done
RUNS="$RUNS $CUR"
MSYS_NO_PATHCONV=1 docker run --rm -v "$FW:/fw:ro" -v "$TOOLS:/tools" -w /tmp striso-build bash -c "
set -e
g++ -O2 ${CXXFLAGS:-} $VDEF -DPERF_HEADER=\\\"${PERF:-performance.h}\\\" -std=c++14 -Wall -Wextra -I /fw -I /tools/harness /tools/harness/fx_test.cpp -o /tmp/fx
$RUNS
"
