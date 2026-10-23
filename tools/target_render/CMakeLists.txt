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
# not part of the firmware build: cmake --build --preset firmware --target target_renders
add_custom_target(target_renders DEPENDS ${_targets})

if(NOT QEMU_SYSTEM_ARM)
  message(STATUS "qemu-system-arm not found: no target render tests")
  return()
endif()

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
