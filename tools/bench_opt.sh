#!/usr/bin/env bash
# Build, flash and measure synth DSP load for each SYNTH_OPT given as an argument.
# Stops at the first failure. Engine: SYNTH_ENGINE env (default strisy).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; FW="$HERE/../striso-control-firmware"
ENGINE="${SYNTH_ENGINE:-strisy}"
SRC=$([ "$ENGINE" = faust ] && echo synth || echo synth_strisy)
for opt in "$@"; do
  rm -f "$FW/build/obj/$SRC.o"; [ "$ENGINE" = faust ] && touch "$FW/synth.dsp"
  "$HERE/build.sh" uf2 SYNTH_ENGINE="$ENGINE" SYNTH_OPT="$opt" SYNTH_LTO="${SYNTH_LTO:-yes}" USE_VERBOSE_COMPILE=yes > "$HERE/.last_build.log" 2>&1 || { tail -30 "$HERE/.last_build.log"; exit 1; }
  grep -q -- "$opt.*$SRC.cpp" "$HERE/.last_build.log" || { echo "flags not applied for: $opt"; exit 1; }
  grep -E 'synth thread stack' "$HERE/.last_build.log"
  if ! "$HERE/.venv/Scripts/python" "$HERE/striso.py" flash "$FW/build/striso_control.uf2" --no-backup > "$HERE/.last_flash.log" 2>&1; then
    cat "$HERE/.last_flash.log"; echo "!! FLASH FAILED OR BOARD UNHEALTHY: stopping"; exit 1
  fi
  grep -E 'never used' "$HERE/.last_flash.log" || true
  echo "[$ENGINE $opt]  $("$HERE/.venv/Scripts/python" "$HERE/striso.py" load 3)"
done
