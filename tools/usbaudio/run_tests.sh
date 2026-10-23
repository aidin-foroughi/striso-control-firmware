#!/usr/bin/env bash
# Host tests of the USB audio output: the ring/packet-sizing core (usb_audio_core.h, compiled
# with gcc in the striso-build Docker image) and the configuration descriptor in the built ELF.
# Usage: [FW_DIR=<firmware checkout>] tools/usbaudio/run_tests.sh   (after tools/build.sh)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "${FW_DIR:-$HERE/../..}" && pwd -W 2>/dev/null || pwd)"
T="$(cd "$HERE" && pwd -W 2>/dev/null || pwd)"
MSYS_NO_PATHCONV=1 docker run --rm -v "$FW:/fw:ro" -v "$T:/t:ro" -w /tmp striso-build bash -c "
set -e
gcc -O2 -std=gnu11 -Wall -Wextra -Werror -I /fw /t/test_core.c -lm -o /tmp/test_core
/tmp/test_core
"
PY="$HERE/../.venv/Scripts/python"; [ -x "$PY" ] || PY=python3
"$PY" "$HERE/check_descriptors.py" --elf "${FW_DIR:-$HERE/../..}/build/striso_control.elf"
