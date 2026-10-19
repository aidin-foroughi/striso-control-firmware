#!/usr/bin/env bash
# Build the Striso firmware in Docker. Usage: [FW_DIR=<firmware checkout>] tools/build.sh [make args...]
# (default target: uf2; FW_DIR defaults to striso-control-firmware, e.g. FW_DIR=worktrees/fw-bowed)
# After building, sanity checks run on the ELF; if one fails the .uf2 is deleted so it can't be flashed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
MAIN="$(cd "$HERE/../striso-control-firmware" && pwd -W 2>/dev/null || pwd)"
FW="$(cd "${FW_DIR:-$HERE/../striso-control-firmware}" && pwd -W 2>/dev/null || pwd)"
docker build -q -t striso-build "$HERE/build-env" >/dev/null
TOOLS="$(cd "$HERE" && pwd -W 2>/dev/null || pwd)"
# git worktrees don't have the submodules checked out: use the main checkout's (read only)
MOUNTS=(-v "$FW:/src" -v "$TOOLS:/tools:ro")
[ -f "$FW/ChibiOS/os/rt/include/ch.h" ] || MOUNTS+=(-v "$MAIN/ChibiOS:/src/ChibiOS:ro")
[ -f "$FW/uf2/utils/uf2conv.py" ] || MOUNTS+=(-v "$MAIN/uf2:/src/uf2:ro")
run() { MSYS_NO_PATHCONV=1 docker run --rm -e FAUST_ARGS "${MOUNTS[@]}" striso-build "$@"; }
# version string from the host: inside the container a worktree's .git link points nowhere
FWVERSION="$(git -C "$FW" --no-pager show --date=short --format=%ad --name-only | head -n1)_$(git -C "$FW" --no-pager describe --tags --always --long --dirty)"
# the board reports this string: include the voice engine so it is clear what is running
for a in "$@"; do case "$a" in VOICE=*) [ "${a#VOICE=}" = strisy ] || FWVERSION="${FWVERSION}_${a#VOICE=}";; esac; done
run make -j"$(nproc)" FWVERSION="$FWVERSION" "${@:-uf2}"

ELF=build/striso_control.elf
[ -f "$FW/$ELF" ] || exit 0
fail() {
  echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" >&2
  echo "!! BUILD REJECTED: $1" >&2
  echo "!! build/striso_control.uf2 deleted so it cannot be flashed." >&2
  echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" >&2
  rm -f "$FW/build/striso_control.uf2"
  exit 1
}
# Software floating point helpers mean some file was compiled without FPU flags: the DSP would be
# many times too slow and starve the rest of the firmware.
SOFTFP=$(run sh -c "arm-none-eabi-nm $ELF | grep -E ' __aeabi_[fd](add|sub|mul|div)$' || true")
[ -z "$SOFTFP" ] || fail "software float helpers linked in (missing FPU flags?): $(echo $SOFTFP | tr '\n' ' ')"
# The synth thread's stack must hold the DSP's stack frames. Faust can put large arrays on the stack
# (e.g. -vec mode), which on the PC is harmless but on the board overflows into other threads' memory.
# Depth = worst call path from the synth thread through everything it calls (tools/stackcheck.py:
# conservative frame per function, fails on recursion and indirect calls), + 256 bytes for the
# exception context the hardware stacks on the thread stack (with FPU state).
# which engine was linked: Faust (mydsp::compute in synth.cpp) or strisy (synth_strisy.cpp)
if run sh -c "arm-none-eabi-nm $ELF | grep -q _ZN5mydsp7computeEiPPfS1_"; then SYNTH_SRC=synth.cpp; else SYNTH_SRC=synth_strisy.cpp; fi
WA=$(grep -oE 'THD_WORKING_AREA\(waSynthThread, *[0-9]+\)' "$FW/$SYNTH_SRC" | grep -oE '[0-9]+\)' | tr -d ')')
DEPTH=$(run sh -c "arm-none-eabi-objdump -d --no-show-raw-insn $ELF | python3 /tools/stackcheck.py _ZL11synthThreadPv") \
  || fail "synth thread stack check failed: $DEPTH"
NEED=$(( ${DEPTH%% *} + 256 ))
echo "synth thread stack ($SYNTH_SRC): deepest ${DEPTH%% *} bytes [${DEPTH#* }] + 256 margin = $NEED of $WA bytes"
[ -n "$WA" ] && [ "$NEED" -le "$WA" ] || fail "synth thread stack overflow: needs ~$NEED bytes, working area is ${WA:-unknown} bytes"
# Firmware must start at the application address, never in the bootloader region.
START=$(run sh -c "readelf -l $ELF | grep LOAD -m1 | awk '{print \$3}'")
[ "$START" = "0x08040000" ] || fail "unexpected load address $START (expected 0x08040000)"
echo "build checks passed (hardware FPU, load address $START)"
