#!/usr/bin/env bash
# CI: ccache for this OS (a pinned release, checksum verified) into <dir>, for the builds'
# CMAKE_C_COMPILER_LAUNCHER / CMAKE_CXX_COMPILER_LAUNCHER (CMake reads them from the environment on
# a first configure). linux: the static build, which also runs in the build image (mounted).
# Usage: tools/ci_ccache.sh <dir>        prints the path of the ccache program
set -euo pipefail
VERSION=4.14.1
case "$(uname -s)" in
Linux) asset=linux-x86_64-musl-static.tar.gz sha=8f684cf82a7acd2f1daddbefcda505c499afe8245cfbfe78f751ce389cd6df52 exe=ccache ;;
Darwin) asset=darwin.tar.gz sha=c279fa81e2e806b4d9e64d4a06cb569a84ef54151feef796e5d4f4e95a464563 exe=ccache ;;
MINGW* | MSYS* | CYGWIN*) asset=windows-x86_64.zip sha=6219f3865ca59aec41ee4b678df171d5d35855ecb2b6dbbbd20690b3a68af7b4 exe=ccache.exe ;;
*) echo "ci_ccache.sh: no ccache for $(uname -s)" >&2; exit 1 ;;
esac
[ $# -eq 1 ] || { echo "usage: tools/ci_ccache.sh <dir>" >&2; exit 2; }
dir="$1"
mkdir -p "$dir"
file="$dir/ccache-$VERSION-$asset"
# read from stdin: for a path with a backslash, sha256sum prefixes its sum with a backslash
# from stdin: given a path with a backslash, sha256sum puts a "\\" before its sum
got="$( (sha256sum < "$file" 2>/dev/null || shasum -a 256 < "$file") | cut -d' ' -f1)"
[ "$got" = "$sha" ] || { echo "ci_ccache.sh: $file: sha256 $got, want $sha" >&2; exit 1; }
case "$asset" in
*.zip) unzip -q -j -o "$file" "*/$exe" -d "$dir" ;;
*) tar -xzf "$file" -C "$dir" --strip-components=1 "ccache-$VERSION-${asset%.tar.gz}/$exe" ;;
esac
rm -f "$file"
"$dir/$exe" --version | head -1 >&2
echo "$dir/$exe"
