#!/usr/bin/env bash
# Render the scripted performance through the C++ engine on the host (in Docker).
# Usage: [FW_DIR=<checkout, relative to project root>] [VOICE=<name>] tools/harness/render_strisy.sh <out-name> [extra g++ flags]
#   VOICE: voice engine voice_<name>.h (default: strisy.h); FW_DIR default striso-control-firmware
#   PERF: alternative performance header in tools/harness (default performance.h)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd -W 2>/dev/null || pwd)"
mkdir -p "$HERE/out"
VDEF=""
[ -z "${VOICE:-}" ] || [ "$VOICE" = strisy ] || VDEF="-DSYNTH_VOICE_HEADER=\\\"voice_$VOICE.h\\\" -DSYNTH_VOICE_NS=$VOICE"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/p" -w /tmp striso-build bash -c "
set -e
g++ -O2 -ffp-contract=off ${2:-} $VDEF -DPERF_HEADER=\\\"${PERF:-performance.h}\\\" -std=c++14 -Wall -Wextra -I /p/${FW_DIR:-striso-control-firmware} -I /p/tools/harness /p/tools/harness/render_strisy.cpp -o /tmp/r
/tmp/r /p/tools/harness/out/$1.f32
"
