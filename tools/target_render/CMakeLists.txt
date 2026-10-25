# Target renders (preset firmware; added by src/CMakeLists.txt): every engine compiled for the
# board's CPU with the synth DSP's flags (STRISO_SYNTH_OPT, FPU, LTO: the flags of
# synth_strisy.cpp) and run on QEMU's mps2-an500 (Cortex-M7 + FPv5-D16). The host renders use the
# host compiler, so they can't show what an arm toolchain change does to the board's arithmetic;
# these can. ctest -L target_render: finite, peak <= 8, and the golden checksum
# (tests/golden/target_renders.txt, tools/update_goldens.sh).
#   target_render_<voice>   the ELF; run: tools/target_render/run.cmake

find_program(QEMU_SYSTEM_ARM qemu-system-arm)
set(_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_tr_flags ${_mcflags} ${_opt_synth}
  # gnu++20: newlib's math.h shows M_PI (the performances use it) only outside strict ISO mode
  "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>" "$<$<COMPILE_LANGUAGE:C>:-std=gnu17>")
set(_targets "")

foreach(v IN LISTS STRISO_VOICES)
  add_executable(target_render_${v} "${_dir}/render_target.cpp" "${_dir}/startup.c")
  striso_voice_defines(defs ${v})
  target_compile_definitions(target_render_${v} PRIVATE ${defs})
  target_include_directories(target_render_${v} PRIVATE "${STRISO_DSP_INCLUDE_DIR}" "${PROJECT_SOURCE_DIR}/tools/harness")
  target_compile_options(target_render_${v} PRIVATE ${_tr_flags})
  target_link_options(target_render_${v} PRIVATE ${_mcflags} ${_opt_synth} --specs=rdimon.specs -nostartfiles
    "-T${_dir}/target.ld" -Wl,--gc-sections
    $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},12>:-Wl,--no-warn-rwx-segments>)
  set_target_properties(target_render_${v} PROPERTIES LINKER_LANGUAGE CXX SUFFIX ".elf"
    LINK_DEPENDS "${_dir}/target.ld" EXCLUDE_FROM_ALL ON)
  list(APPEND _targets target_render_${v})
endforeach()
# the motion filter against the old motion math (tests/motion/target_motion.cpp), each compiled
# with the firmware's flags for its language: the old as motionsensor.c, the filter as
# motion_filter.cpp
set(_tests "${PROJECT_SOURCE_DIR}/tests")
add_executable(target_motion "${_tests}/motion/target_motion.cpp" "${_tests}/ref/motion_ref.c" "${_dir}/startup.c")
target_include_directories(target_motion PRIVATE "${_tests}/motion" "${_tests}/ref" "${PROJECT_SOURCE_DIR}/src/drivers")
target_compile_options(target_motion PRIVATE ${_mcflags} ${_opt}
  "$<$<COMPILE_LANGUAGE:CXX>:${_cppopt}>" "$<$<COMPILE_LANGUAGE:C>:${_copt}>")
target_link_options(target_motion PRIVATE ${_mcflags} ${_opt} --specs=rdimon.specs -nostartfiles
  "-T${_dir}/target.ld" -Wl,--gc-sections
  $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},12>:-Wl,--no-warn-rwx-segments>)
set_target_properties(target_motion PROPERTIES LINKER_LANGUAGE CXX SUFFIX ".elf"
  LINK_DEPENDS "${_dir}/target.ld" EXCLUDE_FROM_ALL ON)
list(APPEND _targets target_motion)

# the key scan's interrupt, old against new (tests/scan_seq/target_scan_seq.cpp): the old
# button_read.c as the firmware compiles C, the sequencer as it compiles C++
add_executable(target_scan_seq "${_tests}/scan_seq/target_scan_seq.cpp" "${_tests}/ref/scan_seq_ref.c"
  "${PROJECT_SOURCE_DIR}/src/drivers/scan_pins.c" "${_dir}/startup.c")
target_include_directories(target_scan_seq PRIVATE "${_tests}/scan_seq/target_stubs" "${_tests}/ref" "${PROJECT_SOURCE_DIR}/src/drivers")
target_compile_options(target_scan_seq PRIVATE ${_mcflags} ${_opt}
  "$<$<COMPILE_LANGUAGE:CXX>:${_cppopt}>" "$<$<COMPILE_LANGUAGE:C>:${_copt}>")
target_link_options(target_scan_seq PRIVATE ${_mcflags} ${_opt} --specs=rdimon.specs -nostartfiles
  "-T${_dir}/target.ld" -Wl,--gc-sections
  $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},12>:-Wl,--no-warn-rwx-segments>)
set_target_properties(target_scan_seq PROPERTIES LINKER_LANGUAGE CXX SUFFIX ".elf"
  LINK_DEPENDS "${_dir}/target.ld" EXCLUDE_FROM_ALL ON)
list(APPEND _targets target_scan_seq)
# the key path's instructions per block, before and after the UMP profile (key_path.cpp), with
# synth_control.cpp's flags; ctest target.key_path checks both give the same controls
add_executable(target_key_path "${_dir}/key_path.cpp" "${_dir}/startup.c")
target_include_directories(target_key_path PRIVATE "${STRISO_DSP_INCLUDE_DIR}" "${PROJECT_SOURCE_DIR}/tools/harness")
target_compile_options(target_key_path PRIVATE ${_mcflags} ${_opt}
  "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-fno-exceptions;-fno-rtti>" "$<$<COMPILE_LANGUAGE:C>:${_copt}>")
target_link_options(target_key_path PRIVATE ${_mcflags} ${_opt} --specs=rdimon.specs -nostartfiles
  "-T${_dir}/target.ld" -Wl,--gc-sections
  $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},12>:-Wl,--no-warn-rwx-segments>)
set_target_properties(target_key_path PROPERTIES LINKER_LANGUAGE CXX SUFFIX ".elf"
  LINK_DEPENDS "${_dir}/target.ld" EXCLUDE_FROM_ALL ON)
list(APPEND _targets target_key_path)

# the voice-switch block's instructions per engine (switch_block.cpp): EngineSet::start and the
# first block, with the synth DSP's flags; ctest target.switch_block prints them
add_executable(target_switch_block "${_dir}/switch_block.cpp" "${_dir}/startup.c")
target_include_directories(target_switch_block PRIVATE "${STRISO_DSP_INCLUDE_DIR}")
target_compile_options(target_switch_block PRIVATE ${_tr_flags})
target_link_options(target_switch_block PRIVATE ${_mcflags} ${_opt_synth} --specs=rdimon.specs -nostartfiles
  "-T${_dir}/target.ld" -Wl,--gc-sections
  $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},12>:-Wl,--no-warn-rwx-segments>)
set_target_properties(target_switch_block PROPERTIES LINKER_LANGUAGE CXX SUFFIX ".elf"
  LINK_DEPENDS "${_dir}/target.ld" EXCLUDE_FROM_ALL ON)
list(APPEND _targets target_switch_block)

# not part of the firmware build: cmake --build --preset firmware --target target_renders
add_custom_target(target_renders DEPENDS ${_targets})

# instruction counts per block of the engine and the master effects (tools/target_render/profile.sh):
# target_profile_<voice>, the same renderer with -DTARGET_PROFILE -DTARGET_FX
set(_ptargets "")
foreach(v IN LISTS STRISO_VOICES)
  add_executable(target_profile_${v} "${_dir}/render_target.cpp" "${_dir}/startup.c")
  striso_voice_defines(defs ${v})
  target_compile_definitions(target_profile_${v} PRIVATE ${defs} TARGET_PROFILE TARGET_FX)
  target_include_directories(target_profile_${v} PRIVATE "${STRISO_DSP_INCLUDE_DIR}" "${PROJECT_SOURCE_DIR}/tools/harness")
  target_compile_options(target_profile_${v} PRIVATE ${_tr_flags})
  target_link_options(target_profile_${v} PRIVATE ${_mcflags} ${_opt_synth} --specs=rdimon.specs -nostartfiles
    "-T${_dir}/target.ld" -Wl,--gc-sections
    $<$<VERSION_GREATER_EQUAL:${CMAKE_C_COMPILER_VERSION},12>:-Wl,--no-warn-rwx-segments>)
  set_target_properties(target_profile_${v} PROPERTIES LINKER_LANGUAGE CXX SUFFIX ".elf"
    LINK_DEPENDS "${_dir}/target.ld" EXCLUDE_FROM_ALL ON)
  list(APPEND _ptargets target_profile_${v})
endforeach()
add_custom_target(target_profiles DEPENDS ${_ptargets})

if(NOT QEMU_SYSTEM_ARM)
  message(STATUS "qemu-system-arm not found: no target render tests")
  return()
endif()

add_test(NAME target.motion COMMAND "${QEMU_SYSTEM_ARM}" -M mps2-an500 -nographic -monitor none -serial none
  -semihosting-config "enable=on,target=native,arg=target_motion" -kernel "$<TARGET_FILE:target_motion>")
set_tests_properties(target.motion PROPERTIES LABELS target_render TIMEOUT 900
  PASS_REGULAR_EXPRESSION "motion: [0-9]+ readings, 0 differences")
add_test(NAME target.scanseq COMMAND "${QEMU_SYSTEM_ARM}" -M mps2-an500 -nographic -monitor none -serial none -icount shift=0
  -semihosting-config "enable=on,target=native,arg=target_scan_seq" -kernel "$<TARGET_FILE:target_scan_seq>")
set_tests_properties(target.scanseq PROPERTIES LABELS target_render TIMEOUT 900
  PASS_REGULAR_EXPRESSION "scanseq: [0-9]+ callbacks, outputs equal")

add_test(NAME target.key_path COMMAND "${QEMU_SYSTEM_ARM}" -M mps2-an500 -nographic -monitor none -serial none
  -icount shift=0 -semihosting-config "enable=on,target=native,arg=key_path" -kernel "$<TARGET_FILE:target_key_path>")
set_tests_properties(target.key_path PROPERTIES LABELS target_render TIMEOUT 900
  PASS_REGULAR_EXPRESSION "before [0-9]+ after [0-9]+ differences 0")

add_test(NAME target.switch_block COMMAND "${QEMU_SYSTEM_ARM}" -M mps2-an500 -nographic -monitor none -serial none
  -icount shift=0 -semihosting-config "enable=on,target=native,arg=switch_block" -kernel "$<TARGET_FILE:target_switch_block>")
set_tests_properties(target.switch_block PROPERTIES LABELS target_render TIMEOUT 900
  PASS_REGULAR_EXPRESSION "switch trumpet start [0-9]+ process [0-9]+ block [0-9]+")

set(_golden_file "${PROJECT_SOURCE_DIR}/tests/golden/target_renders.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_golden_file}")
if(EXISTS "${_golden_file}")
  file(STRINGS "${_golden_file}" _lines REGEX "^[^#]")
  foreach(l IN LISTS _lines)
    if(l MATCHES "^([^ ]+) ([0-9a-f]+)$")
      set("_tgolden_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
  endforeach()
endif()

foreach(v IN LISTS STRISO_VOICES)
  set(np "${STRISO_VOICE_${v}_PRESETS}")
  set(presets -1)
  if(np)
    math(EXPR last "${np} - 1")
    set(presets "")
    foreach(p RANGE ${last})
      list(APPEND presets ${p})
    endforeach()
  endif()
  foreach(p IN LISTS presets)
    if(p EQUAL -1)
      set(name "target.${v}.default")
    else()
      set(name "target.${v}.p${p}.default")
    endif()
    set(expect "missing")
    if(DEFINED "_tgolden_${name}")
      set(expect "${_tgolden_${name}}")
    endif()
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DQEMU=${QEMU_SYSTEM_ARM}"
      "-DELF=$<TARGET_FILE:target_render_${v}>" "-DOUT=${CMAKE_BINARY_DIR}/target_renders/${name}.f32"
      "-DPRESET=${p}" "-DNAME=${name}" "-DEXPECT=${expect}" -P "${_dir}/run.cmake")
    set_tests_properties(${name} PROPERTIES LABELS target_render)
  endforeach()
endforeach()
