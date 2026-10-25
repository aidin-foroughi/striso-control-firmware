#!/usr/bin/env bash
# clang-tidy (15, in Docker) with .clang-tidy on the striso_dsp headers (each compiled alone,
# tests/CMakeLists.txt: striso_dsp_headers), the host renderers, the tests and the message path
# (src/app/messaging.cpp, control_thread.cpp: built for the virtual board), using the host preset's
# compile_commands.json. Exit 1 on any finding.
# Usage: [FW_DIR=<checkout>] tools/tidy.sh [files...]   (default: all of the above)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/docker_env.sh"
image_build_dir build/host
q() { [ $# -eq 0 ] || printf "'%s' " "${@//\'/\'\\\'\'}"; }
in_docker bash -c "set -e
cmake --preset host > /dev/null
cmake --build --preset host --target striso_dsp_headers > /dev/null
F=\"$(q "$@")\"
[ -n \"\$F\" ] || F=\"\$(ls build/host/tests/generated/header_*.cpp) tools/harness/render_strisy.cpp tools/harness/fx_test.cpp tests/test_params.cpp tests/test_bowed_bounds.cpp tests/test_tuning.cpp tests/test_key_scanner.cpp tests/scan_replay.cpp tests/test_event_queue.cpp tests/messaging/test_messaging_model.cpp src/app/messaging.cpp src/app/control_thread.cpp tests/aux_jack/test_jack_detector.cpp tests/led/test_led.cpp src/drivers/led.cpp tests/motion/test_motion_filter.cpp src/drivers/motion_filter.cpp\"
run-clang-tidy -quiet -p build/host \$F 2>&1 | grep -v '^clang-tidy-15 \|^[0-9]* warnings\? generated\|Suppressed\|Use -header-filter\|^$' || true
run-clang-tidy -quiet -p build/host \$F > /dev/null 2>&1"
echo "clang-tidy: clean"
