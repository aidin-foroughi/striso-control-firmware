# striso_dsp: the synth's DSP, platform independent (no HAL, no libm calls, no heap, no globals):
# the voice engines and the master effects, the small helpers they share, and the engine contract
# (docs/engines.md). Header only on purpose: the engines are static inline and compile into the one
# translation unit that runs them (src/synth/synth_strisy.cpp on the board at -O3, sdk/ on the
# host), so the compiler sees the whole DSP at once.
#   striso_dsp/engine.h           the contract: VoiceEngine, Effect, VoiceControls, EngineInfo
#   striso_dsp/adapter.h          an engine header in the voice_<name>.h shape as a VoiceEngine
#   striso_dsp/engine_set.h       a compile-time set of engines (the board's VOICE=all)
#   striso_dsp/engines/<name>.h   each engine as a VoiceEngine: striso::dsp::engines::<name>
#   striso_dsp/effects/fx.h       the master effects as an Effect: striso::dsp::effects::fx
#   striso_dsp/{controls,params,dspmath,svf}.h   the shared helpers
add_library(striso_dsp INTERFACE)
add_library(striso::dsp ALIAS striso_dsp)
target_include_directories(striso_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(striso_dsp INTERFACE cxx_std_20)

# The engines: every include/striso_dsp/engines/<name>.h (strisy first, then by name).
#   STRISO_VOICES                 the names
#   STRISO_VOICE_<name>_HEADER    the engine's own header (strisy.h, voice_<name>.h)
#   STRISO_VOICE_<name>_PRESETS   its NUM_PRESETS (empty: no presets)
set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp")
file(GLOB _engine_traits "${_dir}/engines/*.h")
list(SORT _engine_traits)
set(_voices "")
foreach(t IN LISTS _engine_traits)
  get_filename_component(v "${t}" NAME_WE)
  file(STRINGS "${t}" inc REGEX "^#include \"\\.\\./[a-z_0-9]+\\.h\"")
  list(GET inc 0 inc)
  string(REGEX REPLACE "^#include \"\\.\\./([a-z_0-9]+\\.h)\".*" "\\1" h "${inc}")
  if(NOT EXISTS "${_dir}/${h}")
    message(FATAL_ERROR "engines/${v}.h includes ../${h}, which doesn't exist")
  endif()
  set(np "")
  file(STRINGS "${_dir}/${h}" np_line REGEX "NUM_PRESETS = [0-9]+")
  if(np_line MATCHES "NUM_PRESETS = ([0-9]+)")
    set(np "${CMAKE_MATCH_1}")
  endif()
  set(STRISO_VOICE_${v}_HEADER "${h}" PARENT_SCOPE)
  set(STRISO_VOICE_${v}_PRESETS "${np}" PARENT_SCOPE)
  if(v STREQUAL "strisy")
    list(PREPEND _voices ${v})
  else()
    list(APPEND _voices ${v})
  endif()
endforeach()
set(STRISO_VOICES "${_voices}" PARENT_SCOPE)
# a new engine: configure again
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dir}" "${_dir}/engines")

# striso_voice_defines(<out-var> <voice>): the defines that select one engine where a single one is
# compiled (the board's VOICE=<name>, the target renders): SYNTH_ENGINE_HEADER, SYNTH_VOICE_NS
function(striso_voice_defines out voice)
  set(${out} "SYNTH_ENGINE_HEADER=\"striso_dsp/engines/${voice}.h\"" "SYNTH_VOICE_NS=${voice}" PARENT_SCOPE)
endfunction()
