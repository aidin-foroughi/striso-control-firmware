# striso_dsp: the synth's DSP, platform independent (no HAL, no libm calls, no heap): the strisy
# engine and its helpers (strisy.h, fastpow.h), the voice engines (voice_<name>.h), the master
# effects (fx.h). Header only on purpose: the engines are static inline and compile into the one
# translation unit that runs them (src/synth/synth_strisy.cpp on the board at -O3, the renderers
# and tests on the host), so the compiler sees the whole DSP at once.
#   #include "striso_dsp/strisy.h"   (engine headers include their siblings directly)
add_library(striso_dsp INTERFACE)
add_library(striso::dsp ALIAS striso_dsp)
target_include_directories(striso_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# The engines: strisy plus every include/striso_dsp/voice_<name>.h that defines namespace <name>.
#   STRISO_VOICES                 the names
#   STRISO_VOICE_<name>_PRESETS   its NUM_PRESETS (empty: no presets)
set(STRISO_VOICES strisy)
file(GLOB _voice_headers "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp/voice_*.h")
list(SORT _voice_headers)
foreach(h IN LISTS _voice_headers)
  string(REGEX REPLACE ".*/voice_(.*)\\.h$" "\\1" v "${h}")
  file(STRINGS "${h}" ns REGEX "^namespace ${v} {")
  if(ns)
    list(APPEND STRISO_VOICES ${v})
    set(np "")
    file(STRINGS "${h}" np_line REGEX "NUM_PRESETS = [0-9]+")
    if(np_line MATCHES "NUM_PRESETS = ([0-9]+)")
      set(np "${CMAKE_MATCH_1}")
    endif()
    set(STRISO_VOICE_${v}_PRESETS "${np}" PARENT_SCOPE)
  endif()
endforeach()
set(STRISO_VOICES "${STRISO_VOICES}" PARENT_SCOPE)
# a new engine header: configure again
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp")

# striso_voice_defines(<out-var> <voice>): the defines that select an engine in a renderer or test
# (SYNTH_VOICE_HEADER, SYNTH_VOICE_NS, SYNTH_VOICE_PRESETS if it has presets)
function(striso_voice_defines out voice)
  set(d "")
  if(NOT voice STREQUAL "strisy")
    list(APPEND d "SYNTH_VOICE_HEADER=\"striso_dsp/voice_${voice}.h\"" "SYNTH_VOICE_NS=${voice}")
    if(STRISO_VOICE_${voice}_PRESETS)
      list(APPEND d SYNTH_VOICE_PRESETS)
    endif()
  endif()
  set(${out} "${d}" PARENT_SCOPE)
endfunction()
