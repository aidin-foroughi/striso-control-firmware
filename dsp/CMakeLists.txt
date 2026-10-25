# The synth's DSP: the engines repository (engines/, a git submodule pinned by commit: the voice
# engines, the master effects, the engine contract, the UMP profile, MIDI in, the voice allocator)
# and the firmware's own part here, in the same striso_dsp/ header directory:
#   striso_dsp/strisy.h, engines/strisy.h   strisy, the port of the upstream synth.dsp (GPL): the
#                                           board's default voice, first in the engine list
#   striso_dsp/fastpow.h                    the upstream copy of Paul Mineiro's fastapprox
#   striso_dsp/ump_out.h                    the board's MIDI 2.0 output edge
# striso_dsp (the INTERFACE target) sees both directories; STRISO_VOICES and
# STRISO_VOICE_<name>_HEADER / _PRESETS cover both (engines/dsp/CMakeLists.txt).
add_subdirectory("${PROJECT_SOURCE_DIR}/engines/dsp" "${CMAKE_BINARY_DIR}/engines/dsp")
target_include_directories(striso_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(_voices strisy ${STRISO_VOICES})
foreach(v IN LISTS STRISO_VOICES)
  set(STRISO_VOICE_${v}_HEADER "${STRISO_VOICE_${v}_HEADER}" PARENT_SCOPE)
  set(STRISO_VOICE_${v}_PRESETS "${STRISO_VOICE_${v}_PRESETS}" PARENT_SCOPE)
endforeach()
set(STRISO_VOICE_strisy_HEADER strisy.h PARENT_SCOPE)
set(STRISO_VOICE_strisy_PRESETS "" PARENT_SCOPE)
set(STRISO_VOICES "${_voices}" PARENT_SCOPE)
set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp" "${CMAKE_CURRENT_SOURCE_DIR}/include/striso_dsp/engines")
