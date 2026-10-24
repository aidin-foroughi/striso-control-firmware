#!/usr/bin/env bash
# A release dry run on this PC: builds what CI publishes for a tag v* (.github/workflows/firmware.yml),
# with the same scripts and tests, into dist/ (or DIST), the parts a Windows PC makes:
#   striso_control_<fw version>.uf2        VOICE=strisy (the default firmware)
#   striso_control_<fw version>_all.uf2    VOICE=all
#   striso_virtual-<version>-windows-x64.zip
#   SHA256SUMS
# (CI adds striso_virtual-<version>-macos-universal.zip, from UNIVERSAL=1 tools/virtual.sh and
# tools/package.sh virtual on macOS.) Every file is checked at the end (tools/package.sh check).
# Nothing is pushed, tagged or uploaded.
#
# Usage: [DIST=<dir>] tools/release_dryrun.sh [--no-tests]
#   --no-tests  build and package only (CI runs the firmware's ctest, QEMU target renders
#               included, and the virtual board's tests before it publishes)
# Needs Docker (the firmware) and what tools/virtual.sh needs (the virtual board, natively).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"
tests=1
case "${1:-}" in
"") ;;
--no-tests) tests=0 ;;
*) echo "usage: tools/release_dryrun.sh [--no-tests]" >&2; exit 2 ;;
esac
FWDIR="$(cd "$HERE/.." && pwd)"
dist="${DIST:-$FWDIR/dist}"
rm -rf "$dist"
mkdir -p "$dist"
start=$SECONDS
step() { printf '\n== %s (%d s)\n' "$*" $((SECONDS - start)); }

for voice in all strisy; do
  step "firmware VOICE=$voice"
  "$HERE/build.sh" VOICE="$voice"
  if [ "$tests" = 1 ]; then
    # shellcheck disable=SC2016 # expanded in the container
    in_docker bash -c 'cmake --build --preset firmware --target target_renders && ctest --preset firmware -j"$(nproc)"'
  fi
  "$HERE/package.sh" uf2 "$dist"
done

step "virtual board"
if [ "$tests" = 1 ]; then "$HERE/virtual.sh" all; else "$HERE/virtual.sh" build; fi
"$HERE/package.sh" virtual "$FWDIR/build/host-virtual" "$dist"

step "SHA256SUMS and checks"
"$HERE/package.sh" sums "$dist"
"$HERE/package.sh" check "$dist" windows-x64
step "done: $dist"
ls -l "$dist"
