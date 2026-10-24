#!/usr/bin/env bash
# Build and test the virtual board (striso_virtual, virtual/) natively, for this machine's OS: the
# library is loaded by programs here (Striso Live, the tests), so it is not built in Docker.
#   Windows  clang (LLVM) with the Visual Studio libraries, found with vswhere (Git Bash)
#   macOS    Xcode's clang (the command line tools)
#   Linux    clang (or CC/CXX)
# Needs cmake (3.25+), ninja and python3.
#
# Usage: [FW_DIR=<checkout>] tools/virtual.sh [build|test|all] [ctest arguments...]
#   build   cmake --preset host-virtual, cmake --build --preset host-virtual
#   test    ctest --preset host-virtual (the goldens through the board, the protocol, the API)
#   all     both (default)
# The library: build/host-virtual/virtual/striso_virtual.dll | libstriso_virtual.dylib | .so
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
      echo "@call \"$vs\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul || exit /b 1"
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

q() { [ $# -eq 0 ] || printf '"%s" ' "$@"; }
if [ "$what" = build ] || [ "$what" = all ]; then
  in_env "cmake --preset host-virtual" > /dev/null
  in_env "cmake --build --preset host-virtual"
fi
if [ "$what" = test ] || [ "$what" = all ]; then
  in_env "ctest --preset host-virtual -j$jobs $(q "$@")"
fi
