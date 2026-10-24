# synth_control.cpp on the PC (test_synth_control.cpp): the firmware's key, MIDI and settings logic
# compiled unchanged with stubs, one scenario per test, and recorded key streams replayed through
# the message path against golden trace checksums.
#   synth_control.<scenario>          a scenario of the review (research/review_synth_control.md:
#                                     SC-xx, and the known bugs Bx); fails if one of its checks fails
#   synth_control.flags.<scenario>    the same binary built with the behaviour-change flags on
#                                     (SYNTHCTL_*, off on the board): tests the changed behaviour
#   control_replay.<take>.<mode>      tools/harness/rec/<take>.csv through synth_message in MPE,
#                                     Normal and Mono mode: no note left on, and the trace (MIDI out
#                                     and internal synth controls) matches tests/golden/control_replay.txt.
#                                     After an intended change: tools/update_goldens.sh control

function(_synth_control_test target)
  add_executable(${target} test_synth_control.cpp
    "${PROJECT_SOURCE_DIR}/tests/config_defaults.c"
    "${PROJECT_SOURCE_DIR}/src/app/config_store.c")
  # the stubs first: they stand in for ChibiOS, the HAL and the drivers
  target_include_directories(${target} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${PROJECT_SOURCE_DIR}/src/app"
    "${PROJECT_SOURCE_DIR}/src/synth")
  target_compile_definitions(${target} PRIVATE SYNTH_ENGINE_STRISY ${ARGN})
  target_link_libraries(${target} PRIVATE striso_host_options)
  # reproducible float results (the replay traces); Piers' C-style code warns a lot under -Wextra
  target_compile_options(${target} PRIVATE -O1 -ffp-contract=off
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-write-strings -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers>)
  target_link_options(${target} PRIVATE
    -Wl,--wrap=getConfigSetting -Wl,--wrap=getConfigFloat -Wl,--wrap=getConfigInt -Wl,--wrap=getConfigHex)
  set_source_files_properties("${PROJECT_SOURCE_DIR}/src/app/config_store.c" PROPERTIES COMPILE_OPTIONS "-fno-strict-aliasing;$<$<BOOL:${STRISO_SANITIZE}>:-fno-sanitize=alignment>")
endfunction()

_synth_control_test(test_synth_control)
_synth_control_test(test_synth_control_flags SYNTHCTL_NORMAL_BEND_LIKE_MPE=1 SYNTHCTL_MOTION_CC_FIX=1)

# the scenarios (test_synth_control list); the flags binary runs only those of the flags
set(STRISO_SYNTH_CONTROL_SCENARIOS
  b1_message_interval b2_cc126_count b3_transpose_one_key b4_channel_overflow b5_bend_range_zero
  b15_time_wrap
  sc01_dummy_voices sc02_poly_to_mpe sc02_mpe_to_poly sc02_switch_held sc03_mono_delay
  sc04_toff_stale sc05_pres_hyst sc05_pres_hyst_normal sc06_pingpong sc07_stale_cc74
  sc08_bend_default sc09_alt_light sc10_normal_internal sc10_normal_midi_unchanged
  sc11_midinote_knob sc11_midinote_preset sc12_flip_offsets sc13_ranges sc14_mcm_zero sc15_pb_in
  sc16_echo sc17_rpn_boot sc18_pedal_channel sc19_poly_pgm sc20_bend_range_key
  sc22_free_transpose_range sc22_octave_release sc23_panic sc24_knob_y0 sc25_midiparam_first
  sc27_dials sc28_cc1_lsb sc29_cc65 sc30_mode_messages sc31_steal_dup sc35_tuning_overflow
  sc37_blink sc38_no_synth preset_keys)
set(STRISO_SYNTH_CONTROL_FLAG_SCENARIOS sc08_bend_like_mpe sc21_motion_hyst)
# scenarios of bugs not fixed yet: expected to fail (each fix removes its entry)
set(STRISO_SYNTH_CONTROL_KNOWN_FAILING ${STRISO_SYNTH_CONTROL_SCENARIOS} ${STRISO_SYNTH_CONTROL_FLAG_SCENARIOS})
list(REMOVE_ITEM STRISO_SYNTH_CONTROL_KNOWN_FAILING sc08_bend_default sc10_normal_midi_unchanged preset_keys b1_message_interval sc25_midiparam_first sc24_knob_y0 b15_time_wrap)

foreach(s IN LISTS STRISO_SYNTH_CONTROL_SCENARIOS)
  add_test(NAME synth_control.${s} COMMAND test_synth_control ${s})
  set_tests_properties(synth_control.${s} PROPERTIES LABELS unit)
  if(s IN_LIST STRISO_SYNTH_CONTROL_KNOWN_FAILING)
    set_tests_properties(synth_control.${s} PROPERTIES WILL_FAIL TRUE)
  endif()
endforeach()
foreach(s IN LISTS STRISO_SYNTH_CONTROL_FLAG_SCENARIOS)
  add_test(NAME synth_control.flags.${s} COMMAND test_synth_control_flags ${s})
  set_tests_properties(synth_control.flags.${s} PROPERTIES LABELS unit)
  if(s IN_LIST STRISO_SYNTH_CONTROL_KNOWN_FAILING)
    set_tests_properties(synth_control.flags.${s} PROPERTIES WILL_FAIL TRUE)
  endif()
endforeach()

# recorded key streams through the message path
set(_golden_file "${PROJECT_SOURCE_DIR}/tests/golden/control_replay.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_golden_file}")
set(_golden_lines "")
if(EXISTS "${_golden_file}")
  file(STRINGS "${_golden_file}" _golden_lines REGEX "^[^#]")
endif()
foreach(l IN LISTS _golden_lines)
  string(REGEX MATCH "^([^ ]+) ([0-9a-f]+)$" _ "${l}")
  if(CMAKE_MATCH_1)
    set("_golden_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
file(GLOB _recordings "${PROJECT_SOURCE_DIR}/tools/harness/rec/*.csv")
list(SORT _recordings)
foreach(rec IN LISTS _recordings)
  get_filename_component(rn "${rec}" NAME_WE)
  string(REGEX REPLACE "^play_" "" rn "${rn}")
  foreach(mode mpe normal mono)
    set(name control_replay.${rn}.${mode})
    set(args replay "${rec}" --mode ${mode} --golden-name ${name})
    if(STRISO_GOLDEN)
      if(DEFINED "_golden_${name}")
        list(APPEND args --expect "${_golden_${name}}")
      else()
        list(APPEND args --expect missing)
      endif()
    endif()
    add_test(NAME ${name} COMMAND test_synth_control ${args})
    set_tests_properties(${name} PROPERTIES LABELS control_replay)
  endforeach()
endforeach()
