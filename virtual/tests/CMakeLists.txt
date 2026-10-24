# The virtual board's tests (ctest -L virtual): doctest cases on striso_virtual.
#   virtual.api.*        boot, console, voices, keys, LEDs, resets, the bootloader drive
#   virtual.render.*     every engine and preset of the firmware rendered through the board, bit
#                        for bit against the host goldens (tests/golden/renders.txt)
#   virtual.protocol.*   console sessions against transcripts (tests/transcripts)
include("${PROJECT_SOURCE_DIR}/third_party/doctest/cmake/doctest.cmake")

add_executable(striso_virtual_tests main.cpp test_api.cpp test_message_path.cpp test_protocol.cpp)
target_include_directories(striso_virtual_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/third_party/doctest"
  "${PROJECT_SOURCE_DIR}/tools/harness")
target_compile_definitions(striso_virtual_tests PRIVATE
  "STRISO_GOLDEN_FILE=\"${PROJECT_SOURCE_DIR}/tests/golden/renders.txt\""
  "STRISO_REC_DIR=\"${PROJECT_SOURCE_DIR}/tools/harness/rec\""
  "STRISO_TRANSCRIPTS=\"${CMAKE_CURRENT_SOURCE_DIR}/transcripts\"")
target_link_libraries(striso_virtual_tests PRIVATE striso_virtual striso::dsp striso_host_options)
target_compile_options(striso_virtual_tests PRIVATE -O2 -ffp-contract=off)
target_compile_definitions(striso_virtual_tests PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
if(APPLE)
  target_compile_options(striso_virtual_tests PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/../board/sv_macho_sections.h")
endif()
# next to the library (Windows finds DLLs there)
set_target_properties(striso_virtual_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/..")
doctest_discover_tests(striso_virtual_tests TEST_PREFIX "virtual." PROPERTIES LABELS virtual)

# --- every engine and preset of the VOICE=all firmware, rendered through the board -------------------
# virtual.render.<golden name>: the default, stale and recorded performances (and the two effects
# renders) through the firmware's synth thread, each bit for bit against its host golden. The
# firmware's engines: VOICE_LIST of the all-voices build in src/synth/synth_strisy.cpp.
add_executable(virtual_render virtual_render.cpp perf_default.cpp perf_stale.cpp)
target_include_directories(virtual_render PRIVATE "${PROJECT_SOURCE_DIR}/tools/harness")
target_link_libraries(virtual_render PRIVATE striso_virtual striso::dsp striso_host_options)
target_compile_options(virtual_render PRIVATE -O2 -ffp-contract=off)
target_compile_definitions(virtual_render PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
set_target_properties(virtual_render PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/..")
if(APPLE)
  target_compile_options(virtual_render PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/../board/sv_macho_sections.h")
endif()

file(STRINGS "${PROJECT_SOURCE_DIR}/src/synth/synth_strisy.cpp" _vl REGEX "^#define VOICE_LIST[(]X, XP[)] X[(]strisy[)] XP")
list(GET _vl 0 _vl)
string(REGEX MATCHALL "XP?[(][a-z_0-9]+[)]" _fw_voices "${_vl}")
set(_golden_file "${PROJECT_SOURCE_DIR}/tests/golden/renders.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_golden_file}"
  "${PROJECT_SOURCE_DIR}/src/synth/synth_strisy.cpp")
file(STRINGS "${_golden_file}" _golden_lines REGEX "^[^#]")
foreach(l IN LISTS _golden_lines)
  string(REGEX MATCH "^([^ ]+) ([0-9a-f]+)$" _ "${l}")
  if(CMAKE_MATCH_1)
    set("_vgolden_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
  endif()
endforeach()
file(GLOB _recordings "${PROJECT_SOURCE_DIR}/tools/harness/rec/*.csv")
list(SORT _recordings)

function(_virtual_render name)
  if(NOT DEFINED "_vgolden_${name}")
    return() # render.strisy.stale: the host test is disabled (strisy keeps synth.dsp's stale vpres)
  endif()
  add_test(NAME virtual.${name} COMMAND virtual_render ${ARGN} --expect "${_vgolden_${name}}")
  set_tests_properties(virtual.${name} PROPERTIES LABELS "virtual;virtual_render")
endfunction()

set(STRISO_VIRTUAL_ENGINES "")
foreach(x IN LISTS _fw_voices)
  string(REGEX REPLACE "^XP?[(]([a-z_0-9]+)[)]$" "\\1" v "${x}")
  list(APPEND STRISO_VIRTUAL_ENGINES ${v})
  set(np "${STRISO_VOICE_${v}_PRESETS}")
  if(np)
    math(EXPR last "${np} - 1")
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
    _virtual_render(render.${tag}.default --voice ${v} ${parg} --perf default)
    _virtual_render(render.${tag}.stale --voice ${v} ${parg} --perf stale)
    foreach(rec IN LISTS _recordings)
      get_filename_component(rn "${rec}" NAME_WE)
      string(REGEX REPLACE "^play_" "" rn "${rn}")
      _virtual_render(render.${tag}.rec_${rn} --voice ${v} ${parg} --rec "${rec}")
    endforeach()
  endforeach()
endforeach()
_virtual_render(render.strisy.fx --voice strisy --perf default --fx)
_virtual_render(render.va.p5.fx --voice va --preset 5 --perf default --fx)
message(STATUS "virtual board: engines ${STRISO_VIRTUAL_ENGINES}")

# voice switching: an engine renders its golden after the board ran another preset or engine
# (engine_init must leave nothing of what ran before)
foreach(sw "strisy;-1;1" "va;0;2" "va;3;1" "modal;0;10" "modal;2;33" "bowed;0;16" "bowed;3;1"
           "wind;0;25" "wind;4;47" "ep;0;34" "ep;9;15" "brass;0;48" "brass;5;24")
  list(GET sw 0 v)
  list(GET sw 1 p)
  list(GET sw 2 from)
  if(p EQUAL -1)
    set(g render.${v}.default)
    set(parg "")
  else()
    set(g render.${v}.p${p}.default)
    set(parg --preset ${p})
  endif()
  if(DEFINED "_vgolden_${g}")
    add_test(NAME virtual.switch.${v}.from${from} COMMAND virtual_render --voice ${v} ${parg} --perf default
      --switch-from ${from} --expect "${_vgolden_${g}}")
    set_tests_properties(virtual.switch.${v}.from${from} PROPERTIES LABELS "virtual;virtual_render")
  endif()
endforeach()

# main.c's and motionsensor.c's loops, copied into virtual/board: the copies must stay the same
add_test(NAME virtual.mirror COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_mirror.py")
set_tests_properties(virtual.mirror PROPERTIES LABELS virtual)
