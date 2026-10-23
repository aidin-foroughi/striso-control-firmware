#!/usr/bin/env bash
# Build the Striso firmware in Docker: a thin wrapper around `cmake --preset <preset>` and
# `cmake --build --preset <preset>` (see CMakePresets.json and README "Building").
#
# Usage: [FW_DIR=<checkout>] tools/build.sh [VOICE=<v>] [SYNTH_ENGINE=strisy|faust] [PRESET=<p>]
#                                           [SYNTH_OPT="<flags>"] [SYNTH_LTO=yes|no]
#                                           [-D<CMAKE_OPTION>=<value>...] [clean] [verbose]
#   VOICE         strisy (default), all, or <name> for dsp/include/striso_dsp/voice_<name>.h
#   SYNTH_ENGINE  strisy (default: the C++ engines) or faust (synth.dsp)
#   PRESET        firmware (default, Release) or firmware-debug
#   FW_DIR        firmware checkout, relative to the current directory or absolute (default: the
#                 checkout these tools are in), e.g. FW_DIR=../worktrees/fw-bowed
#   `uf2` and `all` are accepted and ignored (the old make targets: the build always makes the .uf2)
# Every run configures from these arguments (an option left out goes back to its default), as
# make did. The guards (no soft-float, synth stack, load address) run in the build: if one fails
# the build fails and no .uf2 is written. Output: <checkout>/build/<preset>/striso_control.uf2
# (and .elf .bin .hex .map); `tools/striso.py flash <that .uf2>` flashes it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"

PRESET=firmware VOICE=strisy ENGINE=strisy CLEAN=0 VERBOSE=""
DEFS=(-USTRISO_SYNTH_OPT -USTRISO_SYNTH_LTO)
for a in "$@"; do
  case "$a" in
    VOICE=*) VOICE="${a#VOICE=}" ;;
    SYNTH_ENGINE=*) ENGINE="${a#SYNTH_ENGINE=}" ;;
    PRESET=*) PRESET="${a#PRESET=}" ;;
    SYNTH_OPT=*) DEFS+=("-DSTRISO_SYNTH_OPT=${a#SYNTH_OPT=}") ;;
    SYNTH_LTO=yes) DEFS+=(-DSTRISO_SYNTH_LTO=ON) ;;
    SYNTH_LTO=no) DEFS+=(-DSTRISO_SYNTH_LTO=OFF) ;;
    USE_VERBOSE_COMPILE=yes|verbose) VERBOSE=--verbose ;;
    clean) CLEAN=1 ;;
    uf2|all) ;;
    -D*|-U*) DEFS+=("$a") ;;
    *) echo "build.sh: unknown argument '$a' (see the usage at the top of tools/build.sh)" >&2; exit 2 ;;
  esac
done

if [ "$CLEAN" = 1 ]; then
  rm -rf "$FW/build/$PRESET"
  echo "removed build/$PRESET"
  [ $# -gt 1 ] || exit 0
fi

# no git here (e.g. a worktree seen from inside Docker): CMake asks git at build time
GITV="$(striso_git_version 2>/dev/null || true)"
GITDEF=""; [ -z "$GITV" ] || GITDEF="-DSTRISO_GIT_VERSION=$GITV"
# quote each argument for the shell in the container
q() { printf "'%s' " "${@//\'/\'\\\'\'}"; }
SUFFIX=""; [ "$VOICE" = strisy ] || SUFFIX="_$VOICE"
echo "firmware version: ${GITV:-(from git at build time)}$SUFFIX"
image_build_dir "build/$PRESET"
in_docker bash -c "cmake --preset $PRESET -DSTRISO_VOICE=$VOICE -DSTRISO_SYNTH_ENGINE=$ENGINE \
  $GITDEF $(q "${DEFS[@]}") > build/configure-$PRESET.log 2>&1 \
  || { cat build/configure-$PRESET.log; exit 1; }
  cmake --build --preset $PRESET $VERBOSE"
echo "firmware: $FW/build/$PRESET/striso_control.uf2"
