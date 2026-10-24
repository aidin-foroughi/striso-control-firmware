#!/usr/bin/env bash
# clang-format (15, in Docker) on our C++ and the fork's USB audio C code; .clang-format at the
# root (tabs) and in src/usb (ChibiOS style). Piers' files and third-party code (ChibiOS, uf2,
# doctest, fastpow.h) are not formatted, to keep their history readable.
# Usage: [FW_DIR=<checkout>] tools/format.sh [--check]   (--check: list what would change, exit 1)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"
MODE="${1:-}"
FILES='dsp/include/striso_dsp/strisy.h dsp/include/striso_dsp/fx.h dsp/include/striso_dsp/voice_*.h
  src/synth/synth_strisy.cpp src/synth/tuning.h
  src/usb/usb_audio.c src/usb/usb_audio.h src/usb/usb_audio_core.h
  tools/harness/render_strisy.cpp tools/harness/harness_util.h tools/harness/fx_test.cpp
  tools/target_render/render_target.cpp tools/target_render/startup.c
  tests/*.cpp tests/*.c tests/synth_control/*.cpp'
FILES="$(echo $FILES)"  # one line: the list goes into a bash -c command
if [ "$MODE" = --check ]; then
  in_docker bash -c "clang-format --dry-run -Werror $FILES"
else
  in_docker bash -c "clang-format -i $FILES"
fi
