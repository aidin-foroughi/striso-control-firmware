#!/usr/bin/env bash
# Render a scripted performance through a C++ voice engine on the host (CMake target render_custom,
# built and run in Docker).
# Usage: [FW_DIR=<checkout>] [VOICE=<name>] [PERF=<header>] tools/harness/render_strisy.sh <out-name> [compiler flags] [renderer options...]
#   FW_DIR: firmware checkout, relative to the current directory or absolute (default: this
#     repository; e.g. FW_DIR=../worktrees/fw-bowed from the repository root)
#   VOICE: voice engine voice_<name>.h (default: strisy.h)
#   PERF: performance header (default performance.h), looked up in this tools/harness directory
#     first, so a perf_<voice>.h next to this script works for any FW_DIR
#   compiler flags: e.g. "-DNV=10" (one argument, as before)
#   renderer options: --preset N, --nv N, --fx, --rec <csv>, --wav <file>: see
#     tools/harness/render_strisy.cpp (file paths as the container sees them: /out/x.wav)
# -> tools/harness/out/<out-name>.f32 (float32, 48000 Hz, mono; stereo with --fx); prints the peak
#    and warns about non-finite samples.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../docker_env.sh"
NAME="$1"; shift
FLAGS="${1:-}"; [ $# -gt 0 ] && shift
mkdir -p "$HERE/out"
MOUNTS+=(-v "$(_winpath "$HERE"):/perf:ro" -v "$(_winpath "$HERE/out"):/out")
# one build directory per configuration, so renders of different engines can run in parallel
KEY="$(printf '%s|%s|%s' "${VOICE:-strisy}" "${PERF:-performance.h}" "$FLAGS" | (sha256sum 2>/dev/null || shasum -a 256) | cut -c1-10)"
q() { [ $# -eq 0 ] || printf "'%s' " "${@//\'/\'\\\'\'}"; }
in_docker bash -c "set -e
B=build/render-$KEY
cmake --preset host -B \$B -DSTRISO_RENDER_VOICE=${VOICE:-strisy} -DSTRISO_RENDER_PERF=${PERF:-performance.h} \
  -DSTRISO_RENDER_PERF_DIR=/perf $(q "-DSTRISO_RENDER_FLAGS=$FLAGS") > /tmp/configure.log 2>&1 || { cat /tmp/configure.log; exit 1; }
cmake --build \$B --target render_custom > /tmp/build.log 2>&1 || { cat /tmp/build.log; exit 1; }
\$B/tools/harness/render_custom /out/$NAME.f32 $(q "$@") > /dev/null
"
