# Host tests (ctest --preset host, or host-asan under ASan + UBSan).
#
#   unit.*      doctest cases (striso_tests): the tuning math of the StcN command (every key's
#               pitch matches its name), the bowed engine's seed bounds, the config record format
#               and its schema (config_store.cpp, config_schema.hpp), the DSP headers compiling on
#               their own. The engines' and the
#               effects' contract suites (parameters, fuzz, ...) are the SDK's: contract.<name>.*
#   usb_audio_core   tools/usbaudio/test_core.c: the USB audio ring and clock servo simulation
#   striso_py   tools/striso.py's UF2 and config record handling (no board needed)
#   config_schema   tools/config_schema.py against the schema and tests/golden/firmware_bytes.txt
#   render.<voice>[.p<preset>].<performance>   every engine and preset renders the default
#               performance, the stale-vpres performance and the recorded key streams
#               (tools/harness/rec) through the SDK's renderer (sdk/: striso_render, the engines'
#               contract): finite, peak <= 8, a silent idle tail after the stale release, and the
#               golden checksum (tests/golden/renders.txt) so any change of the DSP's output is
#               caught. After an intended change: tools/update_goldens.sh

include("${PROJECT_SOURCE_DIR}/third_party/doctest/cmake/doctest.cmake")

# --- doctest unit tests ----------------------------------------------------------------------------
find_package(Threads REQUIRED)
add_executable(striso_tests
  main.cpp
  test_bowed_bounds.cpp
  test_config_store.cpp
  test_preset_loader.cpp
  test_settings_keys.cpp
  test_tuning.cpp
  test_key_calibration.cpp
  test_event_queue.cpp
  config_defaults.c
  config_page.cpp
  "${PROJECT_SOURCE_DIR}/src/app/config_store.cpp"
  "${PROJECT_SOURCE_DIR}/src/drivers/key_calibration.c")
target_include_directories(striso_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/tools/harness"
  "${PROJECT_SOURCE_DIR}/src/app"
  "${PROJECT_SOURCE_DIR}/src/drivers"
  "${PROJECT_SOURCE_DIR}/src/synth")
target_compile_definitions(striso_tests PRIVATE "STRISO_REC_DIR=\"${PROJECT_SOURCE_DIR}/tools/harness/rec\""
  "STRISO_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/tests/golden\"")
target_link_libraries(striso_tests PRIVATE striso::dsp striso_host_options Threads::Threads)
# the DSP as the renderers build it (reproducible float results)
target_compile_options(striso_tests PRIVATE -O2 -ffp-contract=off)
doctest_discover_tests(striso_tests TEST_PREFIX "unit." PROPERTIES LABELS unit)

# every DSP header compiles on its own (voice_wind_tune.h is a fragment of voice_wind.h)
file(GLOB _dsp_headers RELATIVE "${STRISO_DSP_INCLUDE_DIR}" "${STRISO_DSP_INCLUDE_DIR}/striso_dsp/*.h"
  "${STRISO_DSP_INCLUDE_DIR}/striso_dsp/engines/*.h" "${STRISO_DSP_INCLUDE_DIR}/striso_dsp/effects/*.h")
list(REMOVE_ITEM _dsp_headers striso_dsp/voice_wind_tune.h)
set(_hdr_srcs "")
foreach(h IN LISTS _dsp_headers)
  string(MAKE_C_IDENTIFIER "${h}" id)
  set(src "${CMAKE_CURRENT_BINARY_DIR}/generated/header_${id}.cpp")
  file(CONFIGURE OUTPUT "${src}" CONTENT "#include \"${h}\"\n")
  list(APPEND _hdr_srcs "${src}")
endforeach()
add_library(striso_dsp_headers OBJECT ${_hdr_srcs})
target_link_libraries(striso_dsp_headers PRIVATE striso::dsp striso_host_options)
target_compile_options(striso_dsp_headers PRIVATE -Wno-unused-function -Wno-unused-variable)

# --- the key scanner (src/drivers/key_scanner*.hpp) ----------------------------------------------
# scanner.*: the KeyScanner against the original key processing (tests/ref, compiled with -fwrapv:
# the board's int32 products wrap), the raw recording format and its replay
add_executable(test_key_scanner main.cpp test_key_scanner.cpp test_scan_link.cpp ref/button_read_ref.c)
target_include_directories(test_key_scanner PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/src/drivers"
  "${CMAKE_CURRENT_SOURCE_DIR}/ref")
target_link_libraries(test_key_scanner PRIVATE striso_host_options)
target_compile_options(test_key_scanner PRIVATE -O2 -ffp-contract=off)
set_source_files_properties(ref/button_read_ref.c PROPERTIES COMPILE_OPTIONS "-fwrapv")
doctest_discover_tests(test_key_scanner TEST_PREFIX "scanner." PROPERTIES LABELS unit)

# --- the USB console's grammar and USB MIDI in (src/app/console.hpp, midi_in_parser.hpp) --------
# console.*: the command table, the parser against a reference on random streams (the fuzz), the
# MIDI event checks; the board's handlers end to end: console_sim, console_fuzz below
add_executable(test_console main.cpp test_console.cpp)
target_include_directories(test_console PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/src/app")
target_link_libraries(test_console PRIVATE striso_host_options)
target_compile_options(test_console PRIVATE -O2)
doctest_discover_tests(test_console TEST_PREFIX "console." PROPERTIES LABELS unit)

# scan_replay: the board's raw recordings (tests/scan/<name>.ksr, with the StcS stream recorded in
# the same session, <name>.csv: tools/record_raw.py) replay to exactly the board's messages
add_executable(scan_replay scan_replay.cpp)
target_include_directories(scan_replay PRIVATE "${PROJECT_SOURCE_DIR}/src/drivers")
target_link_libraries(scan_replay PRIVATE striso_host_options)
target_compile_options(scan_replay PRIVATE -O2 -ffp-contract=off)
file(GLOB _scan_recordings "${CMAKE_CURRENT_SOURCE_DIR}/scan/*.ksr")
list(SORT _scan_recordings)
foreach(r IN LISTS _scan_recordings)
  get_filename_component(rn "${r}" NAME_WE)
  get_filename_component(rd "${r}" DIRECTORY)
  set(_stream "")
  if(EXISTS "${rd}/${rn}.csv")
    set(_stream "${rd}/${rn}.csv")
  endif()
  add_test(NAME scan_replay.${rn} COMMAND scan_replay "${r}" ${_stream})
  set_tests_properties(scan_replay.${rn} PROPERTIES LABELS unit)
endforeach()

# --- USB audio core (C) -------------------------------------------------------------------------------
add_executable(test_usb_audio_core "${PROJECT_SOURCE_DIR}/tools/usbaudio/test_core.c")
target_include_directories(test_usb_audio_core PRIVATE "${PROJECT_SOURCE_DIR}/src/usb")
target_compile_options(test_usb_audio_core PRIVATE -O2 -Werror)
target_link_libraries(test_usb_audio_core PRIVATE m striso_host_options)
add_test(NAME usb_audio_core COMMAND test_usb_audio_core)
set_tests_properties(usb_audio_core PROPERTIES LABELS unit)
# usb_audio_wrap: the ring across the wrap of its free-running 32-bit frame counters (24.85 h)
add_executable(test_ua_wrap test_ua_wrap.c)
target_include_directories(test_ua_wrap PRIVATE "${PROJECT_SOURCE_DIR}/src/usb")
target_compile_options(test_ua_wrap PRIVATE -O2 -Werror)
target_link_libraries(test_ua_wrap PRIVATE striso_host_options)
add_test(NAME usb_audio_wrap COMMAND test_ua_wrap)
set_tests_properties(usb_audio_wrap PROPERTIES LABELS unit)

# --- drivers and USB (C) ------------------------------------------------------------------------------
# usb_obq_discard: the console's 'V' drops the bulk console's pending output while an IN transfer
# may be reading it (src/usb/obq_discard.h, D1), against ChibiOS' own hal_buffers.c
if(EXISTS "${STRISO_CHIBIOS_DIR}/os/hal/src/hal_buffers.c")
  add_executable(test_obq_discard usb_queue/test_obq_discard.c "${STRISO_CHIBIOS_DIR}/os/hal/src/hal_buffers.c")
  target_include_directories(test_obq_discard PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/usb_queue/stubs"
    "${STRISO_CHIBIOS_DIR}/os/hal/include"
    "${PROJECT_SOURCE_DIR}/src/usb")
  target_link_libraries(test_obq_discard PRIVATE striso_host_options)
  target_compile_options(test_obq_discard PRIVATE -O1 -Wno-unused-parameter)
  add_test(NAME usb_obq_discard COMMAND test_obq_discard)
  set_tests_properties(usb_obq_discard PROPERTIES LABELS unit)
  # console_sim: the USB console and USB MIDI in end to end (R1, R3-R10, B14): the real console.cpp,
  # bulk_usb.c, midi_usb.c, config_store.cpp and ChibiOS hal_buffers.c/chprintf.c against a fake USB
  # controller and a host that parses like Striso Live. chprintf.c gets a larger digit buffer:
  # on the 64-bit host "%lx" of a uint32_t reads 64 bits (on the board long is 32 bits).
  # console_fuzz: the same code under random hosts (console_sim/fuzz_console_sim.c).
  set(_streams "${STRISO_CHIBIOS_DIR}/os/hal/lib/streams")
  file(READ "${_streams}/chprintf.c" _chprintf)
  string(REPLACE "#define MAX_FILLER 11" "#define MAX_FILLER 23" _chprintf "${_chprintf}")
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/chprintf_lp64.c" CONTENT "${_chprintf}" @ONLY)
  foreach(_t IN ITEMS "test_console_sim;console_sim/test_console_sim.c;console_sim"
                      "test_console_fuzz;console_sim/fuzz_console_sim.c;console_fuzz")
    list(GET _t 0 _exe)
    list(GET _t 1 _main)
    list(GET _t 2 _name)
    add_executable(${_exe}
      ${_main} console_sim/sim.c console_sim/messaging_stub.cpp config_defaults.c config_page.cpp
      "${PROJECT_SOURCE_DIR}/src/app/console.cpp"
      "${PROJECT_SOURCE_DIR}/src/usb/bulk_usb.c"
      "${PROJECT_SOURCE_DIR}/src/usb/midi_usb.c"
      "${PROJECT_SOURCE_DIR}/src/app/config_store.cpp"
      "${STRISO_CHIBIOS_DIR}/os/hal/src/hal_buffers.c"
      "${CMAKE_CURRENT_BINARY_DIR}/generated/chprintf_lp64.c"
      "${_streams}/memstreams.c")
    target_include_directories(${_exe} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/console_sim/stubs"
      "${CMAKE_CURRENT_SOURCE_DIR}/console_sim"
      "${PROJECT_SOURCE_DIR}/src/app"
      "${PROJECT_SOURCE_DIR}/src/usb"
      "${PROJECT_SOURCE_DIR}/src/synth"
      "${PROJECT_SOURCE_DIR}/src/drivers"
      "${STRISO_CHIBIOS_DIR}/os/hal/include"
      "${_streams}")
    target_link_libraries(${_exe} PRIVATE striso_host_options)
    target_compile_options(${_exe} PRIVATE -O1 -Wno-unused-parameter -Wno-unused-function
      -Wno-sign-compare -Wno-missing-field-initializers -Wno-format -Wno-unused-but-set-variable
      -fno-strict-aliasing $<$<BOOL:${STRISO_SANITIZE}>:-fno-sanitize=alignment>)
    add_test(NAME ${_name} COMMAND ${_exe})
    set_tests_properties(${_name} PROPERTIES LABELS unit)
  endforeach()
else()
  message(WARNING "ChibiOS not found (git submodule update --init): tests usb_obq_discard, console_sim, console_fuzz skipped")
endif()

# jack.*: the aux jack (src/drivers/aux_jack.cpp, its JackDetector) against the old aux_jack.c
# (tests/ref/aux_jack_ref.c), call for call on a stand-in HAL (tests/aux_jack/stubs): an electrical
# model of the jack (D3, D9), every mode's transitions, the autodetect thresholds, random runs
add_executable(test_jack_detector main.cpp aux_jack/test_jack_detector.cpp aux_jack/ref_wrap.c)
target_include_directories(test_jack_detector PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/aux_jack/stubs"
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/src/drivers")
target_link_libraries(test_jack_detector PRIVATE striso_host_options)
target_compile_options(test_jack_detector PRIVATE -O1 -Wno-unused-parameter -Wno-unused-variable -Wno-switch)
doctest_discover_tests(test_jack_detector TEST_PREFIX "jack." PROPERTIES LABELS unit)

# led.*: the LEDs (src/drivers/led.cpp over its presenter) against the old led.c (tests/ref/led_ref.c),
# call for call on a stand-in HAL (tests/led/stubs): every gamma input, up/down state and dial
# angle, blinks around their end and the time's wrap, random calls
add_executable(test_led main.cpp led/test_led.cpp led/ref_wrap.c "${PROJECT_SOURCE_DIR}/src/drivers/led.cpp")
target_include_directories(test_led PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/led/stubs"
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/src/drivers")
target_link_libraries(test_led PRIVATE striso_host_options)
doctest_discover_tests(test_led TEST_PREFIX "led." PROPERTIES LABELS unit)

# motion.*: the MotionFilter (src/drivers/motion_filter.hpp) against the old motion math
# (tests/ref/motion_ref.c), bit for bit (on the board's CPU too: tools/target_render, target.motion)
add_executable(test_motion_filter main.cpp motion/test_motion_filter.cpp ref/motion_ref.c)
target_include_directories(test_motion_filter PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/motion"
  "${CMAKE_CURRENT_SOURCE_DIR}/ref"
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/src/drivers")
target_link_libraries(test_motion_filter PRIVATE striso_host_options m)
doctest_discover_tests(test_motion_filter TEST_PREFIX "motion." PROPERTIES LABELS unit)

# messaging_model[_small], messaging_threads: src/app/inbox.hpp (the control thread's event queue)
# never loses, corrupts or reorders an accepted event, never misses a wakeup, refuses only from a
# full lane and counts it (R2, B7): exhaustively on a small inbox, under key traffic on the board's,
# and on real threads
add_executable(test_messaging_model messaging/test_messaging_model.cpp)
target_include_directories(test_messaging_model PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/messaging/stubs"
  "${PROJECT_SOURCE_DIR}/src/app")
target_link_libraries(test_messaging_model PRIVATE striso_host_options Threads::Threads)
target_compile_options(test_messaging_model PRIVATE -O2)
foreach(_m IN ITEMS "messaging_model_small;small" "messaging_model;board" "messaging_threads;threads")
  list(GET _m 0 _name)
  list(GET _m 1 _part)
  add_test(NAME ${_name} COMMAND test_messaging_model ${_part})
  set_tests_properties(${_name} PROPERTIES LABELS unit)
endforeach()

# codec_linein: the codec's line-in writes (src/drivers/codec_aic3x_linein.h) open and close the
# analog path LINE2 -> ADC PGA -> line out, in a model of the TLV320AIC3104 registers (D4)
add_executable(test_codec_linein test_codec_linein.c)
target_include_directories(test_codec_linein PRIVATE "${PROJECT_SOURCE_DIR}/src/drivers")
target_link_libraries(test_codec_linein PRIVATE striso_host_options)
add_test(NAME codec_linein COMMAND test_codec_linein)
set_tests_properties(codec_linein PROPERTIES LABELS unit)

# --- tools/striso.py: UF2 and config records ----------------------------------------------------------
add_test(NAME striso_py COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_striso_py.py")
# tools/config_schema.py (striso.py's checks, the config editor's key list) against the schema:
# the default records byte for byte as the golden, the editor's tables, the record checks
add_test(NAME config_schema COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_config_schema.py")
set_tests_properties(striso_py config_schema PROPERTIES LABELS unit)

# tools/stackcheck.py (the build's thread stack check) on small hand-written disassemblies
add_test(NAME stackcheck COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_stackcheck.py")
set_tests_properties(stackcheck PROPERTIES LABELS unit)

# tools/check_scan_board.py (the board check: every scan processed exactly once) on StcE replies
add_test(NAME check_scan_board COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_check_scan_board.py")
set_tests_properties(check_scan_board PROPERTIES LABELS unit)

# button_read.c source checks (the ADC interrupt can't run on the PC): S12 cache invalidate size
add_test(NAME button_read_src COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_button_read_src.py")
set_tests_properties(button_read_src PROPERTIES LABELS unit)

# src/usb source checks: endpoints re-armed after a USB suspend and resume (D2), bulk_out (D16),
# GET_STATUS self powered (D18)
add_test(NAME usb_src COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_usb_src.py")
set_tests_properties(usb_src PROPERTIES LABELS unit)

# --- renders ---------------------------------------------------------------------------------------
set(_golden_file "${CMAKE_CURRENT_SOURCE_DIR}/golden/renders.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_golden_file}")
file(STRINGS "${_golden_file}" _golden_lines REGEX "^[^#]")
foreach(l IN LISTS _golden_lines)
  string(REGEX MATCH "^([^ ]+) ([0-9a-f]+)$" _ "${l}")
  if(CMAKE_MATCH_1)
    set("_golden_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
file(GLOB _recordings "${PROJECT_SOURCE_DIR}/tools/harness/rec/*.csv")
list(SORT _recordings)

# a released voice's stale positive vpres must not keep it sounding (synth_control leaves it):
# silent and idle 12 s after the release at 1 s (the longest preset release, va pad, ends ~9.5 s)
set(STRISO_STALE_SILENT_AFTER 12)

function(_render_test name)
  set(args --nv 10 --check --golden-name ${name} ${ARGN})
  if(STRISO_GOLDEN)
    if(DEFINED "_golden_${name}")
      list(APPEND args --expect "${_golden_${name}}")
    else()
      list(APPEND args --expect missing)
    endif()
  endif()
  add_test(NAME ${name} COMMAND striso_render ${args})
  set_tests_properties(${name} PROPERTIES LABELS render)
endfunction()

foreach(v IN LISTS STRISO_VOICES)
  set(npresets "${STRISO_VOICE_${v}_PRESETS}")
  if(npresets)  # (not "n": if(n) is the constant N, false)
    math(EXPR last "${npresets} - 1")
    set(presets "")
    foreach(p RANGE ${last})
      list(APPEND presets ${p})
    endforeach()
  else()
    set(presets none)
  endif()
  foreach(p IN LISTS presets)
    if(p STREQUAL "none")
      set(tag "${v}")
      set(parg "")
    else()
      set(tag "${v}.p${p}")
      set(parg --preset ${p})
    endif()
    _render_test(render.${tag}.default --engine ${v} --perf default ${parg})
    _render_test(render.${tag}.stale --engine ${v} --perf stale ${parg} --silent-after ${STRISO_STALE_SILENT_AFTER})
    foreach(rec IN LISTS _recordings)
      get_filename_component(rn "${rec}" NAME_WE)
      string(REGEX REPLACE "^play_" "" rn "${rn}")
      _render_test(render.${tag}.rec_${rn} --engine ${v} ${parg} --rec "${rec}")
    endforeach()
  endforeach()
  if(v IN_LIST STRISO_PERFORMANCES) # its own performance, tools/harness/perf_<v>.h
    _render_test(render.${v}.perf --engine ${v} --perf ${v})
  endif()
endforeach()
# master effects on the default performance (stereo)
_render_test(render.strisy.fx --engine strisy --perf default --fx)
_render_test(render.va.p5.fx --engine va --perf default --preset 5 --fx)

# Known: strisy (the port of the original synth.dsp) keeps sounding a released voice whose last
# vpres stayed positive, at a constant level (0.25 for vpres 0.25), as synth.dsp does. The voice
# engines ignore positive vpres while pres == 0; strisy is kept bit-identical to synth.dsp.
set_tests_properties(render.strisy.stale PROPERTIES DISABLED TRUE)

# synth_control.cpp on the PC: review scenarios and recorded key streams through the message path
add_subdirectory(synth_control)
