#!/usr/bin/env bash
# CI: runs a command in the build image ($IMAGE) with the checkout at /src, passing VOICE and
# PRESET through. With ccache installed by tools/ci_ccache.sh in $RUNNER_TEMP/ccache (CI skips that
# on a tag) the compiles go through it, its cache in .ccache of the checkout; its stats at the end.
# Usage: IMAGE=<image> tools/ci_docker.sh <command...>
set -euo pipefail
: "${IMAGE:?IMAGE: the build image}"
args=(--rm -v "$PWD:/src" -w /src -e VOICE -e PRESET)
cc="${RUNNER_TEMP:-/nonexistent}/ccache"
if [ -f "$cc/ccache" ]; then
  args+=(-v "$cc:/opt/ccache:ro" -e CMAKE_C_COMPILER_LAUNCHER=/opt/ccache/ccache
    -e CMAKE_CXX_COMPILER_LAUNCHER=/opt/ccache/ccache -e CCACHE_DIR=/src/.ccache -e CCACHE_BASEDIR=/src
    -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-500M}")
fi
rc=0
docker run "${args[@]}" "$IMAGE" "$@" || rc=$?
if [ -f "$cc/ccache" ]; then CCACHE_DIR="$PWD/.ccache" "$cc/ccache" --show-stats || true; fi
exit "$rc"
