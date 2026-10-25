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
  dsp/include/striso_dsp/controls.h dsp/include/striso_dsp/dspmath.h dsp/include/striso_dsp/params.h
  dsp/include/striso_dsp/svf.h dsp/include/striso_dsp/engine.h dsp/include/striso_dsp/adapter.h
  dsp/include/striso_dsp/engine_set.h dsp/include/striso_dsp/engines/*.h dsp/include/striso_dsp/effects/*.h
  src/synth/synth_strisy.cpp src/synth/tuning.h
  src/drivers/key_scanner.h src/drivers/key_scanner.hpp src/drivers/key_scanner_capture.hpp src/drivers/key_scanner.cpp
  src/app/console.hpp src/app/console.cpp src/app/midi_in_parser.hpp src/app/out_stream.hpp virtual/board/fw_console.cpp
  src/app/app.cpp virtual/board/fw_app.cpp tests/console_sim/messaging_stub.cpp
  src/app/event_queue.hpp src/app/wire.hpp src/app/inbox.hpp src/app/messaging.hpp src/app/messaging.cpp
  src/app/control_thread.cpp
  src/usb/usb_audio.c src/usb/usb_audio.h src/usb/usb_audio_core.h
  sdk/include/striso_sdk/*.h sdk/src/*.h sdk/src/*.cpp sdk/tools/*.cpp sdk/tests/*.cpp
  tools/harness/harness_util.h tools/harness/fx_test.cpp
  tools/target_render/render_target.cpp tools/target_render/startup.c
  tests/*.cpp tests/*.hpp tests/*.c tests/synth_control/*.cpp tests/messaging/*.cpp'
FILES="${FILES//$'\n'/ }"  # one line: the list goes into a bash -c command (its globs expand there)
if [ "$MODE" = --check ]; then
  in_docker bash -c "clang-format --dry-run -Werror $FILES"
else
  in_docker bash -c "clang-format -i $FILES"
fi
