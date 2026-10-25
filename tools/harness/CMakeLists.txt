# The master effects' host test (tools/harness/fx_test.sh, fx_suite.sh): fx_test.cpp with an engine
# and a performance compiled in, with the renderers' flags (-O2 -ffp-contract=off). The engine
# renders themselves are the SDK's (sdk/: striso_render, render_custom; tools/harness/render_strisy.sh).
#   fx_test        strisy, performance.h
#   fx_test_va     va, perf_fx.h
#   fx_custom      STRISO_RENDER_VOICE / _PERF / _PERF_DIR / _FLAGS (sdk/CMakeLists.txt; fx_test.sh)
#   render_wavs    a WAV of every engine's default performance: build/<preset>/renders/<engine>.wav

# striso_add_fx_test(<target> VOICE <v> PERF <header> [PERF_DIR <dir>] [OPTIONS ...])
function(striso_add_fx_test target)
  cmake_parse_arguments(PARSE_ARGV 1 A "" "VOICE;PERF" "OPTIONS;PERF_DIR")
  add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/fx_test.cpp")
  striso_voice_defines(defs ${A_VOICE})
  target_compile_definitions(${target} PRIVATE ${defs} "PERF_HEADER=\"${A_PERF}\"")
  target_include_directories(${target} PRIVATE ${A_PERF_DIR} "${CMAKE_CURRENT_SOURCE_DIR}" "${STRISO_HARNESS_DIR}")
  target_link_libraries(${target} PRIVATE striso::dsp striso_host_options)
  target_compile_options(${target} PRIVATE -O2 -ffp-contract=off ${A_OPTIONS})
endfunction()

striso_add_fx_test(fx_test VOICE strisy PERF performance.h)
striso_add_fx_test(fx_test_va VOICE va PERF perf_fx.h)
if(STRISO_RENDER_VOICE IN_LIST STRISO_VOICES)
  separate_arguments(_custom_flags UNIX_COMMAND "${STRISO_RENDER_FLAGS}")
  striso_add_fx_test(fx_custom VOICE ${STRISO_RENDER_VOICE} PERF ${STRISO_RENDER_PERF}
    PERF_DIR ${STRISO_RENDER_PERF_DIR} OPTIONS ${_custom_flags})
endif()

set(_wavs "")
foreach(v IN LISTS STRISO_VOICES)
  set(w "${CMAKE_BINARY_DIR}/renders/${v}.wav")
  add_custom_command(OUTPUT "${w}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/renders"
    COMMAND striso_render --engine ${v} --perf default --out "${CMAKE_BINARY_DIR}/renders/${v}.f32" --wav "${w}"
    DEPENDS striso_render VERBATIM)
  list(APPEND _wavs "${w}")
endforeach()
add_custom_target(render_wavs DEPENDS ${_wavs})
