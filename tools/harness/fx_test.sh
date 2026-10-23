#!/usr/bin/env bash
# Build the fx host test (tools/harness/fx_test.cpp, CMake target fx_custom) in Docker and run it.
# Usage: [FW_DIR=<checkout>] [VOICE=strisy|va|...] [PERF=perf_fx.h] [CXXFLAGS=...] tools/harness/fx_test.sh <out-name> [fx_test args...]
#   FW_DIR: firmware checkout, relative to the current directory or absolute (default: this
#     repository; e.g. FW_DIR=../worktrees/fw-fx from the repository root)
#   output goes to tools/harness/out/<out-name>.{f32,wav}; several runs: separate them with ' ;; '
#   e.g. tools/harness/fx_test.sh fx_a rvmix=0.3 ';;' fx_b src=impulse rvmix=1
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../docker_env.sh"
mkdir -p "$HERE/out"
MOUNTS+=(-v "$(_winpath "$HERE"):/perf:ro" -v "$(_winpath "$HERE/out"):/out")
KEY="$(printf '%s|%s|%s' "${VOICE:-strisy}" "${PERF:-performance.h}" "${CXXFLAGS:-}" | (sha256sum 2>/dev/null || shasum -a 256) | cut -c1-10)"
FX="build/render-$KEY/tools/harness/fx_custom"
# split the arguments into runs at ';;'
RUNS=""
CUR="$FX /out/$1"
shift
for a in "$@"; do
  if [ "$a" = ";;" ]; then RUNS="$RUNS $CUR && "; CUR="$FX"; FIRST=1; continue; fi
  if [ "${FIRST:-0}" = 1 ]; then CUR="$CUR /out/$a"; FIRST=0; else CUR="$CUR '$a'"; fi
done
RUNS="$RUNS $CUR"
in_docker bash -c "set -e
cmake --preset host -B build/render-$KEY -DSTRISO_RENDER_VOICE=${VOICE:-strisy} -DSTRISO_RENDER_PERF=${PERF:-performance.h} \
  -DSTRISO_RENDER_PERF_DIR=/perf '-DSTRISO_RENDER_FLAGS=${CXXFLAGS:-}' > /tmp/configure.log 2>&1 || { cat /tmp/configure.log; exit 1; }
cmake --build build/render-$KEY --target fx_custom > /tmp/build.log 2>&1 || { cat /tmp/build.log; exit 1; }
$RUNS
"
