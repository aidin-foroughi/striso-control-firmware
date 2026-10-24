#!/usr/bin/env bash
# Instructions per 32-sample block of every engine and preset (default performance, 10 voices) and
# of the master effects after it, compiled for the board (the synth DSP's flags) and counted on
# QEMU (-icount shift=0: SysTick counts instructions / 40). Instructions, not cycles: caches, TCM
# and wait states are not modelled. The hashes are the target render hashes (tests/golden/
# target_renders.txt) and the hash of the stereo fx output, so a run also shows bit-exactness.
# Usage: [FW_DIR=<checkout>] tools/target_render/profile.sh [voice regex] [fx mode]
#   fx mode: 0 off, 1 defaults (default), 2 every stage on
# Output lines: <voice> <preset> <hash> <engine instr/block> <fxhash> <fx instr/block>
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
source "$HERE/docker_env.sh"
RE="${1:-.}"
FXMODE="${2:-1}"
in_docker bash -c "cmake --build --preset firmware --target target_profiles > build/profile-build.log 2>&1 \
    || { tail -30 build/profile-build.log; exit 1; }
  mkdir -p build/profile
  jobs=''
  for elf in build/firmware/target_render/target_profile_*.elf; do
    v=\${elf##*target_profile_}; v=\${v%.elf}
    echo \"\$v\" | grep -Eq -- '$RE' || continue
    np=\$(grep -Eo 'NUM_PRESETS = [0-9]+' dsp/include/striso_dsp/voice_\$v.h 2>/dev/null | grep -Eo '[0-9]+' || true)
    if [ -z \"\$np\" ]; then jobs=\"\$jobs \$v:-1\"; else for p in \$(seq 0 \$((np - 1))); do jobs=\"\$jobs \$v:\$p\"; done; fi
  done
  run() {
    v=\${1%%:*}; p=\${1#*:}
    o=\$(qemu-system-arm -M mps2-an500 -nographic -monitor none -serial none -icount shift=0 \
      -semihosting-config enable=on,target=native,arg=render_target,arg=build/profile/\$v.\$p.f32,arg=\$p,arg=10,arg=17.5,arg=$FXMODE \
      -kernel build/firmware/target_render/target_profile_\$v.elf)
    h=\$(echo \"\$o\" | sed -n 's/^hash \\([0-9a-f]*\\).*/\\1/p')
    d=\$(echo \"\$o\" | sed -n 's/^dsp_ticks [0-9]* per_block \\([0-9.]*\\)/\\1/p')
    fh=\$(echo \"\$o\" | sed -n 's/^fxhash \\([0-9a-f]*\\)/\\1/p')
    f=\$(echo \"\$o\" | sed -n 's/^fx_ticks [0-9]* per_block \\([0-9.]*\\)/\\1/p')
    echo \"\$v \$p \$h \$d \${fh:-none} \${f:-0}\" | awk '{printf \"%s %s %s %.0f %s %.0f\n\", \$1, \$2, \$3, \$4 * 40, \$5, \$6 * 40}'
  }
  export -f run
  printf '%s\n' \$jobs | xargs -P \$(nproc) -I{} bash -c 'run {}' | sort -k1,1 -k2,2n"
