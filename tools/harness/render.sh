#!/usr/bin/env bash
# Render the scripted test performance through a Faust synth on the host (in Docker).
# Usage: tools/harness/render.sh <synth.dsp> <out-name> [extra g++ flags]
#   <synth.dsp> path relative to the project root; output: tools/harness/out/<out-name>.f32
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd -W 2>/dev/null || pwd)"
docker build -q -t striso-build "$HERE/../build-env" >/dev/null
mkdir -p "$HERE/out"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/p" -w /tmp striso-build bash -c "
set -e
python3 -c \"import sys; sys.path.insert(0, '/p/striso-control-firmware'); from faust2striso import faust_replace_fixed_sliders as f; open('/tmp/s.dsp','w').write(f(open('/p/$1').read()))\"
faust ${FAUST_FLAGS:-} -I /p/striso-control-firmware -I \$(dirname /p/$1) -a /p/tools/harness/host_arch.cpp /tmp/s.dsp -lang ocpp -o /tmp/s.cpp
g++ -O2 -ffp-contract=off ${3:-} -std=c++14 -w -I /p/striso-control-firmware -I /p/tools/harness /tmp/s.cpp -o /tmp/render
cp /tmp/s.cpp /p/tools/harness/out/$2.cpp
/tmp/render /p/tools/harness/out/$2.f32
"
