# Host renderers of the voice engines (preset host): render_strisy.cpp compiled per engine and
# performance, with the flags the harness always used (-O2 -ffp-contract=off: no FMA contraction,
# so renders are reproducible and comparable).
#   render_<voice>         performance.h (the default scripted performance)
#   render_<voice>_stale   perf_stale.h (a released voice with its last vpres stuck)
#   render_<voice>_perf    perf_<voice>.h, the engine's own performance, if there is one
#   render_custom          STRISO_RENDER_VOICE / _PERF / _PERF_DIR / _FLAGS (tools/harness/render_strisy.sh)
#   fx_test, fx_test_va    master effects host test (tools/harness/fx_test.sh)
#   render_wavs            renders every engine's default performance to build/<preset>/renders/*.wav

set(STRISO_RENDER_FLAGS_BASE -O2 -ffp-contract=off)

# the engines (STRISO_VOICES, STRISO_VOICE_<v>_PRESETS) and striso_voice_defines(): dsp/CMakeLists.txt

# striso_add_render(<target> VOICE <v> PERF <header> [PERF_DIR <dir>] [OPTIONS ...])
function(striso_add_render target)
  cmake_parse_arguments(PARSE_ARGV 1 A "" "VOICE;PERF;SOURCE" "OPTIONS;PERF_DIR")
  if(NOT A_SOURCE)
    set(A_SOURCE render_strisy.cpp)
  endif()
  add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${A_SOURCE}")
  striso_voice_defines(defs ${A_VOICE})
  target_compile_definitions(${target} PRIVATE ${defs} "PERF_HEADER=\"${A_PERF}\"")
  target_include_directories(${target} PRIVATE ${A_PERF_DIR} "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${target} PRIVATE striso::dsp striso_host_options)
  target_compile_options(${target} PRIVATE ${STRISO_RENDER_FLAGS_BASE} ${A_OPTIONS})
endfunction()

foreach(v IN LISTS STRISO_VOICES)
  striso_add_render(render_${v} VOICE ${v} PERF performance.h)
  striso_add_render(render_${v}_stale VOICE ${v} PERF perf_stale.h)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/perf_${v}.h")
    striso_add_render(render_${v}_perf VOICE ${v} PERF perf_${v}.h)
  endif()
endforeach()

# the script-driven renderer (tools/harness/render_strisy.sh, fx_test.sh configure these)
set(STRISO_RENDER_VOICE "strisy" CACHE STRING "render_custom/fx_custom: engine")
set(STRISO_RENDER_PERF "performance.h" CACHE STRING "render_custom/fx_custom: performance header")
set(STRISO_RENDER_PERF_DIR "" CACHE PATH "render_custom/fx_custom: extra directory searched for the performance header")
set(STRISO_RENDER_FLAGS "" CACHE STRING "render_custom/fx_custom: extra compiler flags")
separate_arguments(_custom_flags UNIX_COMMAND "${STRISO_RENDER_FLAGS}")
if(STRISO_RENDER_VOICE IN_LIST STRISO_VOICES)
  striso_add_render(render_custom VOICE ${STRISO_RENDER_VOICE} PERF ${STRISO_RENDER_PERF}
    PERF_DIR ${STRISO_RENDER_PERF_DIR} OPTIONS ${_custom_flags})
  striso_add_render(fx_custom SOURCE fx_test.cpp VOICE ${STRISO_RENDER_VOICE} PERF ${STRISO_RENDER_PERF}
    PERF_DIR ${STRISO_RENDER_PERF_DIR} OPTIONS ${_custom_flags})
else()
  message(WARNING "STRISO_RENDER_VOICE=${STRISO_RENDER_VOICE} is not an engine (${STRISO_VOICES})")
endif()

striso_add_render(fx_test SOURCE fx_test.cpp VOICE strisy PERF performance.h)
striso_add_render(fx_test_va SOURCE fx_test.cpp VOICE va PERF perf_fx.h)

set(_wavs "")
foreach(v IN LISTS STRISO_VOICES)
  set(w "${CMAKE_BINARY_DIR}/renders/${v}.wav")
  add_custom_command(OUTPUT "${w}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/renders"
    COMMAND render_${v} "${CMAKE_BINARY_DIR}/renders/${v}.f32" --nv 10 --wav "${w}"
    DEPENDS render_${v} VERBATIM)
  list(APPEND _wavs "${w}")
endforeach()
add_custom_target(render_wavs DEPENDS ${_wavs})
