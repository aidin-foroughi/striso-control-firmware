# The synth's DSP: the engines repository (engines/, a git submodule pinned by commit: the voice
# engines, the master effects, the engine contract, the UMP profile, MIDI in, the voice allocator)
# and the firmware's own part here, in the same awva/ header directory:
#   striso_dsp/strisy.h, engines/strisy.h   strisy, the port of the upstream synth.dsp (GPL): the
#                                           board's default voice, first in the engine list
#   striso_dsp/fastpow.h                    the upstream copy of Paul Mineiro's fastapprox
#   striso_dsp/ump_out.h                    the board's MIDI 2.0 output edge
# awva_dsp (the INTERFACE target) sees both directories; AWVA_ENGINES and
# AWVA_ENGINE_<name>_HEADER / _PRESETS cover both (awva/dsp/CMakeLists.txt).
add_subdirectory("${AWVA_DIR}/dsp" "${CMAKE_BINARY_DIR}/awva/dsp")
target_include_directories(awva_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(_voices strisy ${AWVA_ENGINES})
foreach(v IN LISTS AWVA_ENGINES)
  set(AWVA_ENGINE_${v}_HEADER "${AWVA_ENGINE_${v}_HEADER}" PARENT_SCOPE)
  set(AWVA_ENGINE_${v}_PRESETS "${AWVA_ENGINE_${v}_PRESETS}" PARENT_SCOPE)
  set(AWVA_ENGINE_${v}_TRAITS "${AWVA_ENGINE_${v}_TRAITS}" PARENT_SCOPE)
endforeach()
set(AWVA_ENGINE_strisy_TRAITS "striso_dsp/engines/strisy.h" PARENT_SCOPE)
set(AWVA_ENGINE_strisy_HEADER strisy.h PARENT_SCOPE)
set(AWVA_ENGINE_strisy_PRESETS "" PARENT_SCOPE)
set(AWVA_ENGINES "${_voices}" PARENT_SCOPE)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp" "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp/engines")
