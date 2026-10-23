#!/usr/bin/env bash
# Render the scripted test performance through a Faust synth on the host (in Docker).
# Usage: [FW_DIR=<checkout>] tools/harness/render.sh <synth.dsp> <out-name> [extra g++ flags]
#   <synth.dsp>: path relative to the current directory or absolute (e.g. synth.dsp or
#   tools/harness/synth_orig.dsp from the repository root); output: tools/harness/out/<out-name>.f32
#   FW_DIR: firmware checkout for faust2striso.py and the Faust libraries (default: this repository)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../docker_env.sh"
TOOLS="$(cd "$HERE/.." && pwd -W 2>/dev/null || pwd)"
FW="$(cd "${FW_DIR:-$HERE/../..}" && pwd -W 2>/dev/null || pwd)"
DSP_DIR="$(cd "$(dirname "$1")" && pwd -W 2>/dev/null || pwd)"
DSP="$(basename "$1")"

mkdir -p "$HERE/out"
MSYS_NO_PATHCONV=1 docker run --rm -v "$FW:/fw:ro" -v "$TOOLS:/tools" -v "$DSP_DIR:/dsp:ro" -w /tmp "$IMAGE" bash -c "
set -e
python3 -c \"import sys; sys.path.insert(0, '/fw/src/synth/faust'); from faust2striso import faust_replace_fixed_sliders as f; open('/tmp/s.dsp','w').write(f(open('/dsp/$DSP').read()))\"
faust ${FAUST_FLAGS:-} -I /fw/src/synth/faust -I /dsp -a /tools/harness/host_arch.cpp /tmp/s.dsp -lang ocpp -o /tmp/s.cpp
g++ -O2 -ffp-contract=off ${3:-} -std=c++14 -w -I /fw/dsp/include/striso_dsp -I /tools/harness /tmp/s.cpp -o /tmp/render
cp /tmp/s.cpp /tools/harness/out/$2.cpp
/tmp/render /tools/harness/out/$2.f32
"
