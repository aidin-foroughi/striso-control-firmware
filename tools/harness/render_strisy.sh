#!/usr/bin/env bash
# Render the scripted performance through the C++ engine on the host (in Docker).
# Usage: [FW_DIR=<checkout>] [VOICE=<name>] tools/harness/render_strisy.sh <out-name> [extra g++ flags]
#   FW_DIR: firmware checkout, relative to the current directory or absolute (default: this
#     repository; e.g. FW_DIR=../worktrees/fw-bowed from the repository root)
#   VOICE: voice engine voice_<name>.h (default: strisy.h)
#   PERF: alternative performance header in tools/harness (default performance.h)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../docker_env.sh"
TOOLS="$(cd "$HERE/.." && pwd -W 2>/dev/null || pwd)"
FW="$(cd "${FW_DIR:-$HERE/../..}" && pwd -W 2>/dev/null || pwd)"
mkdir -p "$HERE/out"
VDEF=""
[ -z "${VOICE:-}" ] || [ "$VOICE" = strisy ] || VDEF="-DSYNTH_VOICE_HEADER=\\\"voice_$VOICE.h\\\" -DSYNTH_VOICE_NS=$VOICE"
MSYS_NO_PATHCONV=1 docker run --rm -v "$FW:/fw:ro" -v "$TOOLS:/tools" -w /tmp "$IMAGE" bash -c "
set -e
g++ -O2 -ffp-contract=off ${2:-} $VDEF -DPERF_HEADER=\\\"${PERF:-performance.h}\\\" -std=c++14 -Wall -Wextra -I /fw/dsp/include/striso_dsp -I /tools/harness /tools/harness/render_strisy.cpp -o /tmp/r
/tmp/r /tools/harness/out/$1.f32
"
