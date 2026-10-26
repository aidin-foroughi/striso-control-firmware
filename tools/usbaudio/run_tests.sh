#!/usr/bin/env bash
# Host tests of the USB audio output: the ring/packet-sizing core (usb_audio_core.h; ctest
# usb_audio_core, preset host) and the configuration descriptor in the built ELF (ctest
# usb_descriptors, preset firmware). Both run in Docker.
# Usage: [FW_DIR=<firmware checkout>] tools/usbaudio/run_tests.sh   (after tools/build.sh)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
"$HERE/../test.sh" host -R '^usb_audio_core$'
source "$HERE/../docker_env.sh"
in_docker ctest --preset firmware -R '^usb_descriptors$'
