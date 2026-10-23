#!/usr/bin/env bash
# Build and run the host tests in Docker (ctest; see tests/CMakeLists.txt).
# Usage: [FW_DIR=<checkout>] tools/test.sh [host|host-asan] [ctest arguments...]
#   host (default): unit tests and every engine/preset render with its golden checksum
#   host-asan: the same under AddressSanitizer + UndefinedBehaviorSanitizer
#   e.g. tools/test.sh host -L unit        tools/test.sh host -R 'render\.bowed'
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"
PRESET=host
case "${1:-}" in host|host-asan) PRESET="$1"; shift ;; esac
q() { [ $# -eq 0 ] || printf "'%s' " "${@//\'/\'\\\'\'}"; }
mkdir -p "$FW/build"
in_docker bash -c "cmake --preset $PRESET > build/configure-$PRESET.log 2>&1 || { cat build/configure-$PRESET.log; exit 1; }
  cmake --build --preset $PRESET && ctest --preset $PRESET -j\$(nproc) $(q "$@")"
