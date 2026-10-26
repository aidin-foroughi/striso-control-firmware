#!/usr/bin/env bash
# Build, flash and measure synth DSP load for each SYNTH_OPT given as an argument (needs the board).
# Stops at the first failure. Engine: SYNTH_ENGINE env (default strisy); voice: VOICE env (default
# strisy); SYNTH_LTO env (yes|no, default yes). Firmware: FW_DIR (default this repository), as for
# build.sh. Example: tools/bench_opt.sh "-O2 -ggdb" "-O3 -ggdb -funroll-loops"
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; FW="$(cd "${FW_DIR:-$HERE/..}" && pwd)"; export FW_DIR="$FW"
ENGINE="${SYNTH_ENGINE:-strisy}"
SRC=$([ "$ENGINE" = faust ] && echo synth || echo synth_strisy)
for opt in "$@"; do
  "$HERE/build.sh" SYNTH_ENGINE="$ENGINE" VOICE="${VOICE:-strisy}" SYNTH_OPT="$opt" SYNTH_LTO="${SYNTH_LTO:-yes}" verbose > "$HERE/.last_build.log" 2>&1 || { tail -30 "$HERE/.last_build.log"; exit 1; }
  # the synth's compile command as built (Ninja recompiles it when its flags change)
  grep '"command"' "$FW/build/firmware/compile_commands.json" | grep -F -- "/$SRC.cpp" | grep -qF -- "$opt" \
    || { echo "flags not applied for: $opt"; exit 1; }
  grep -E 'synth thread stack' "$HERE/.last_build.log"
  if ! "$HERE/.venv/Scripts/python" "$HERE/striso.py" flash "$FW/build/firmware/striso_control.uf2" --no-backup > "$HERE/.last_flash.log" 2>&1; then
    cat "$HERE/.last_flash.log"; echo "!! FLASH FAILED OR BOARD UNHEALTHY: stopping"; exit 1
  fi
  grep -E 'never used' "$HERE/.last_flash.log" || true
  echo "[$ENGINE $opt]  $("$HERE/.venv/Scripts/python" "$HERE/striso.py" load 3)"
done
