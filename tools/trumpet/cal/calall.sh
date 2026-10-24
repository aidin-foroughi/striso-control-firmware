#!/bin/bash
# Calibration of voice_trumpet.h (trumpet_cal.h): per preset and note, the lip setting, the slide trim the note settles at
# and the threshold mouth pressure, measured with the engine itself (tt.cpp, a single-note driver; cal.py does the search).
# Needs a dev container named $TRUMPET_DEV (default trumpet-dev) from the build image with this worktree at /src, e.g.
#   docker run -d --name trumpet-dev -v <worktree>:/src <striso-fw image> sleep infinity
# Run from this directory: tools/trumpet/cal/calall.sh   (PRESETS="3 4" for some presets; REUSE_PTHR=1 for the second
# pass, which re-runs the search at the measured thresholds). ~2 min per pass on 32 cores. Then rebuild + update goldens.
# Helpers: grid.py (notes x pressures x parameter sets), tab.py / score.py (the acceptance test: early level within 6 dB,
# within 30 c by 25 ms or 3.6 periods, no wrong partial, steady pitch).
set -e
cd "$(dirname "$0")"
DEV=${TRUMPET_DEV:-trumpet-dev}
build() { MSYS_NO_PATHCONV=1 docker exec -w /src/tools/trumpet/cal $DEV bash -c "g++ -O2 -std=c++20 -I/src/dsp/include -I/src/tools/harness tt.cpp -o /tmp/trumpet_tt"; }
build
LO=(40 40 42 52 40 40 40 40 40)
for p in ${PRESETS:-0 1 2 3 4 5 6 7 8}; do python cal.py $p ${LO[$p]} cal_p$p.json 2> cal_p$p.log; done
python gen_cal.py ../../../dsp/include/striso_dsp/trumpet_cal.h cal_p{0,1,2,3,4,5,6,7,8}.json
build
