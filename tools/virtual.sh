#!/usr/bin/env bash
# Build and test the virtual board (striso_virtual, virtual/) natively, for this machine's OS: the
# library is loaded by programs here (Striso Live, the tests), so it is not built in Docker.
#   Windows  clang (LLVM) with the Visual Studio libraries, found with vswhere (Git Bash)
#   macOS    Xcode's clang (the command line tools)
#   Linux    clang (or CC/CXX)
# Needs cmake (3.25+), ninja and python3.
#
# Usage: [FW_DIR=<checkout>] [UNIVERSAL=1] [BUILD_DIR=<dir>] [CHIBIOS_DIR=<dir>]
#          tools/virtual.sh [build|test|all] [ctest arguments...]
#   build   configure (the host-virtual preset) and build
#   test    ctest (the goldens through the board, the protocol, the API)
#   all     both (default)
#   UNIVERSAL=1   macOS: one library for arm64 and x86_64, macOS 10.15 and later (as Striso Live),
#                 in build/host-virtual-universal; tools/ctest_arch.py runs its x86_64 slice
#   BUILD_DIR     the build directory, relative to the checkout (default build/host-virtual)
#   CHIBIOS_DIR   ChibiOS (its os/hal is built in) when the checkout has no third_party/ChibiOS
#                 and is no git worktree of one that has it (e.g. a `git archive` copy)
# The library: <build dir>/virtual/striso_virtual.dll | libstriso_virtual.dylib | .so
# (build/host-virtual is where Striso Live's tests look for it)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "${FW_DIR:-$HERE/..}" && pwd)"
what="${1:-all}"
[ $# -gt 0 ] && shift
jobs="$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 4)"

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
  vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  vs="$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | tr -d '\r')"
  [ -n "$vs" ] || { echo "virtual.sh: no Visual Studio with the C++ tools (vswhere)" >&2; exit 1; }
  bat="$(mktemp -d)/virtual.bat"
  # every step in the Visual Studio environment (its libraries for clang, its cmake and ninja)
  in_env() {
    {
      printf '@call "%s\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1\n' "$vs"
      echo "@cd /d \"$(cygpath -w "$FW")\" || exit /b 1"
      echo "@$*"
    } > "$bat"
    cmd //c "$(cygpath -w "$bat")"
  }
  ;;
*)
  in_env() { (cd "$FW" && eval "$*"); }
  ;;
esac

# ChibiOS (its HAL code is built in): CHIBIOS_DIR, the submodule, or in a git worktree without it
# the main checkout's (as tools/docker_env.sh)
cfg=()
if [ -n "${CHIBIOS_DIR:-}" ]; then
  cfg+=("-DSTRISO_CHIBIOS_DIR=$(cd "$CHIBIOS_DIR" && { pwd -W 2>/dev/null || pwd; })")
elif [ ! -f "$FW/third_party/ChibiOS/os/hal/src/hal_buffers.c" ]; then
  common="$(git -C "$FW" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
  for c in "$common/../third_party/ChibiOS" "$common/../ChibiOS"; do
    if [ -n "$common" ] && [ -f "$c/os/hal/src/hal_buffers.c" ]; then
      cfg+=("-DSTRISO_CHIBIOS_DIR=$(cd "$c" && { pwd -W 2>/dev/null || pwd; })")
      break
    fi
  done
fi

bdir="${BUILD_DIR:-build/host-virtual}"
if [ "${UNIVERSAL:-0}" = 1 ]; then
  [ "$(uname -s)" = Darwin ] || { echo "virtual.sh: UNIVERSAL=1 is for macOS" >&2; exit 2; }
  bdir="${BUILD_DIR:-build/host-virtual-universal}"
  cfg+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15)
fi

q() { [ $# -eq 0 ] || printf '"%s" ' "$@"; }
if [ "$what" = build ] || [ "$what" = all ]; then
  log="$(mktemp)"
  in_env "cmake --preset host-virtual -B $(q "$bdir" ${cfg[@]+"${cfg[@]}"})" > "$log" 2>&1 || { cat "$log"; exit 1; }
  in_env "cmake --build $(q "$bdir")"
fi
if [ "$what" = test ] || [ "$what" = all ]; then
  # as the host-virtual test preset (which only knows build/host-virtual)
  in_env "ctest --test-dir $(q "$bdir") --output-on-failure --no-tests=error -j$jobs $(q "$@")"
fi
