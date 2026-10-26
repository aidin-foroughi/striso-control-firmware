#!/usr/bin/env bash
# Full host test of the master effects (fx.h): renders demos and stress tests, prints the checks.
# Usage: [FW_DIR=<checkout>] tools/harness/fx_suite.sh   (FW_DIR: relative to the current directory
#   or absolute; default this repository; e.g. FW_DIR=../worktrees/fw-fx from the repository root)
set -euo pipefail
if [ -n "${FW_DIR:-}" ]; then FW_DIR="$(cd "$FW_DIR" && pwd)"; export FW_DIR; fi
cd "$(dirname "$0")/../.."
T=tools/harness/fx_test.sh
P=tools/.venv/Scripts/python
A=tools/harness/fx_analyze.py
O=tools/harness/out
q() { grep "out/\|NON-FINITE" | sed 's|.*/out/||'; }

echo "### demos: VA 'keys' preset, musical performance (perf_fx.h): dry vs default vs each stage"
VOICE=va PERF=perf_fx.h $T fx_va_default preset=5 ';;' fx_va_off preset=5 bypass=1 \
  ';;' fx_va_drive preset=5 drive=0.6 tilt=0.3 \
  ';;' fx_va_phaser preset=5 phmix=1 phrate=0.4 phfb=0.5 \
  ';;' fx_va_chorus preset=5 chmix=0.8 chdepth=0.6 chens=0.4 \
  ';;' fx_va_trem preset=5 trdepth=0.8 trrate=5 \
  ';;' fx_va_delay preset=5 dlmix=0.45 dltime=333 dlfb=0.45 \
  ';;' fx_va_reverb preset=5 rvmix=0.45 rvdecay=4 rvpre=30 \
  ';;' fx_va_all preset=5 drive=0.3 phmix=0.6 chmix=0.5 trdepth=0.5 dlmix=0.3 rvmix=0.3 2>&1 | q
for f in default off drive phaser chorus trem delay reverb all; do
  echo -n "$f: "; $P $A level $O/fx_va_$f.f32 $O/fx_va_${f}_in.f32 | head -1
done

echo "### strisy, default performance: dry vs default fx"
$T fx_strisy_default ';;' fx_strisy_all drive=0.3 phmix=0.6 chmix=0.5 trdepth=0.5 dlmix=0.3 rvmix=0.3 2>&1 | q
$P $A level $O/fx_strisy_default.f32 $O/fx_strisy_default_in.f32

echo "### level / limiter: input x2.5 (peaks ~11) with reverb and delay"
$T fx_t_loud gain=2.5 rvmix=0.5 rvdecay=6 dlmix=0.6 dlfb=0.8 2>&1 | q
$P $A level $O/fx_t_loud.f32 $O/fx_t_loud_in.f32

echo "### reverb impulse responses (smoothness, ringing, RT60)"
$T fx_t_ir src=impulse seconds=0.1 tail=8 rvmix=1 chmix=0 \
  ';;' fx_t_ir0 src=impulse seconds=0.1 tail=8 rvmix=1 chmix=0 rvdamp=0 rvlowcut=20 \
  ';;' fx_t_ir6 src=impulse seconds=0.1 tail=14 rvmix=1 chmix=0 rvdecay=6 rvdamp=0.2 \
  ';;' fx_t_ir15 src=impulse seconds=0.1 tail=30 rvmix=1 chmix=0 rvdecay=15 rvdamp=0 rvmod=1 2>&1 | q
for f in ir ir0 ir6 ir15; do echo "== $f"; $P $A ir $O/fx_t_$f.f32; done

echo "### chorus zipper: 1 kHz sine, chorus only, static and with all chorus parameters ramped every block"
$T fx_t_chz src=sine freq=1000 amp=2 seconds=6 rvmix=0 chmix=1 chdepth=0.5 chens=0.3 \
  ';;' fx_t_chz2 src=sine freq=1000 amp=2 seconds=6 rvmix=0 ramp:chmix=0.2:1:0.5:3 ramp:chdepth=1:0.1:0.5:5.5 ramp:chrate=0.1:4:0.5:5.5 ramp:chens=0:1:0.5:5.5 2>&1 | q
for f in chz chz2; do $P $A tone $O/fx_t_$f.f32 1000; done

echo "### half-rate resampler: echo of 1 kHz / 9 kHz / 18 kHz sines (delay 20 ms, no feedback)"
$T fx_t_hb1 src=sine freq=1000 amp=2 seconds=3 rvmix=0 chmix=0 dlmix=1 dlfb=0 dltime=20 dlpp=0 \
  ';;' fx_t_hb9 src=sine freq=9000 amp=2 seconds=3 rvmix=0 chmix=0 dlmix=1 dlfb=0 dltime=20 dlpp=0 \
  ';;' fx_t_hb18 src=sine freq=18000 amp=2 seconds=3 rvmix=0 chmix=0 dlmix=1 dlfb=0 dltime=20 dlpp=0 2>&1 | q
for f in hb1:1000 hb9:9000 hb18:18000; do $P $A tone $O/fx_t_${f%%:*}.f32 ${f##*:} | head -1; done

echo "### stability: maximum feedback / decay, 10 s of loud input then 50 s of silence"
$T fx_t_maxfb seconds=10 tail=50 gain=2 dlmix=1 dlfb=0.95 dltone=1 dltime=650 rvmix=1 rvdecay=15 rvdamp=0 rvmod=1 phmix=1 phfb=0.85 chmix=1 \
  ';;' fx_t_maxfb2 src=noise amp=4 seconds=10 tail=50 dlmix=1 dlfb=0.95 dltone=1 dltime=20 dlpp=0 rvmix=1 rvdecay=15 rvdamp=0 rvpre=0 2>&1 | q
$P $A decay $O/fx_t_maxfb.f32 10
$P $A decay $O/fx_t_maxfb2.f32 10

echo "### stress: random parameter writes every block (1-4 per block, 20% extremes, bypass toggles), 60 s"
$T fx_t_random seconds=60 random=1 seed=1 ';;' fx_t_random2 seconds=60 random=1 seed=7 gain=2 \
  ';;' fx_t_random3 seconds=60 random=1 seed=99 tail=10 2>&1 | q

echo "### parameter jumps (min <-> max) on a steady tone, all stages on: HF burst over steady state"
mapfile -t ARGS < <($P tools/harness/fx_clicks.py args | tr -d '\r')
$T "${ARGS[@]}" > /dev/null 2>&1
$P tools/harness/fx_clicks.py | head -8

echo "### demo with live parameter moves: delay time glide + jumps, reverb and chorus ramps (VA keys)"
VOICE=va PERF=perf_fx.h $T fx_va_moves preset=5 dlmix=0.4 dlfb=0.5 dltime=300 @6.5:dltime=450 ramp:dltime=450:200:9.6:12.4 \
  ramp:rvmix=0.1:0.5:0:12 ramp:chdepth=0.2:0.9:6:12 ramp:phmix=0:0.8:9.6:12 2>&1 | q
$P $A level $O/fx_va_moves.f32 $O/fx_va_moves_in.f32

echo "### stereo: L/R correlation (1 = mono)"
$P -c "
import numpy as np
for f in ['fx_va_off', 'fx_va_default', 'fx_va_chorus', 'fx_va_reverb', 'fx_va_delay', 'fx_va_all', 'fx_strisy_default']:
    x = np.fromfile('$O/' + f + '.f32', dtype=np.float32).astype(float).reshape(-1, 2)
    c = np.sum(x[:, 0] * x[:, 1]) / np.sqrt(np.sum(x[:, 0] ** 2) * np.sum(x[:, 1] ** 2))
    print(f'{f}: {c:.3f}')
"
