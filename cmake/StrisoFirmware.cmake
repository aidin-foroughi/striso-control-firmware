# The firmware: flags and link exactly as the ChibiOS make rules (rules.mk) did for the firmware
# Makefile, so the same toolchain made a byte-identical binary (verified when the build moved to
# CMake). Included from src/CMakeLists.txt, which lists the sources.
#
# Object order on the link line matters for the layout (and with LTO for the code): ChibiOS'
# assembler files, ChibiOS' C files, syscalls.c, the firmware's C files, then the C++ files, as in
# rules.mk (OBJS = ASMX ASM C CPP). Each group is an object library, linked in that order.

include(ChibiOS)
include(StrisoVersion)

# --- options --------------------------------------------------------------------------------------
set(STRISO_SYNTH_ENGINE "strisy" CACHE STRING "synth engine: strisy (C++ voice engines) or faust (synth.dsp)")
set_property(CACHE STRISO_SYNTH_ENGINE PROPERTY STRINGS strisy faust)
set(STRISO_VOICE "strisy" CACHE STRING "voice engine (strisy engine only): strisy, all, or <name> for dsp/include/striso_dsp/voice_<name>.h")
set(STRISO_OPT "-O1 -ggdb -fomit-frame-pointer -falign-functions=16" CACHE STRING "optimisation flags (USE_OPT)")
set(STRISO_SYNTH_OPT "-O3 -ggdb -fomit-frame-pointer -falign-functions=16" CACHE STRING "optimisation flags of the synth DSP file (SYNTH_OPT)")
option(STRISO_LTO "link time optimisation" ON)
option(STRISO_SYNTH_LTO "compile the synth DSP file with LTO too (OFF: a normal object, all per-file flags apply)" ON)
option(STRISO_LISTING "write an objdump -S listing next to the ELF" OFF)
option(STRISO_USB_AUDIO_PRE_VOLUME "USB audio gets the synth before the volume control, at a fixed gain (config.h)" OFF)
find_program(FAUST_EXECUTABLE faust)
if(FAUST_EXECUTABLE)
  set(_faust_default ON)
else()
  set(_faust_default OFF)
endif()
option(STRISO_FAUST_REGENERATE "faust engine: compile synth.dsp with Faust (OFF: the committed synth.cpp)" ${_faust_default})
set(STRISO_LOAD_ADDRESS "0x08040000" CACHE STRING "expected load address: the application region after the UF2 bootloader")

if(NOT STRISO_SYNTH_ENGINE MATCHES "^(strisy|faust)$")
  message(FATAL_ERROR "STRISO_SYNTH_ENGINE must be strisy or faust, not '${STRISO_SYNTH_ENGINE}'")
endif()
if(NOT STRISO_VOICE MATCHES "^(strisy|all)$" AND NOT EXISTS "${STRISO_DSP_INCLUDE_DIR}/striso_dsp/voice_${STRISO_VOICE}.h")
  message(FATAL_ERROR "STRISO_VOICE=${STRISO_VOICE}: no dsp/include/striso_dsp/voice_${STRISO_VOICE}.h")
endif()

# --- generated files ------------------------------------------------------------------------------
set(_gen "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_gen}")

# version: fwversion.h, rewritten at every build when the version changes
set(_suffix "")
if(NOT STRISO_VOICE STREQUAL "strisy")
  set(_suffix "_${STRISO_VOICE}")
endif()
set(_gitv "")
if(STRISO_GIT_VERSION)
  striso_git_version(_v)
  set(_gitv "-DGIT_VERSION=${_v}")
  message(STATUS "firmware version: ${_v}${_suffix}")
endif()
add_custom_target(striso_fwversion
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DOUT=${_gen}/fwversion.h"
          "-DSUFFIX=${_suffix}" ${_gitv} -P "${PROJECT_SOURCE_DIR}/cmake/StrisoVersion.cmake"
  BYPRODUCTS "${_gen}/fwversion.h"
  COMMENT "" VERBATIM)

# the config editor page embedded in the firmware
add_custom_command(OUTPUT "${_gen}/config_editor/config_editor.h"
  COMMAND "${Python3_EXECUTABLE}" "${STRISO_CONFIG_EDITOR_DIR}/config_editor.py"
          "${STRISO_CONFIG_EDITOR_DIR}/config.htm" "${_gen}/config_editor/config_editor.h"
  DEPENDS "${STRISO_CONFIG_EDITOR_DIR}/config.htm" "${STRISO_CONFIG_EDITOR_DIR}/config_editor.py"
  COMMENT "Generating config_editor.h" VERBATIM)
add_custom_target(striso_config_editor DEPENDS "${_gen}/config_editor/config_editor.h")

# the synth: C++ engines, or synth.dsp through Faust
if(STRISO_SYNTH_ENGINE STREQUAL "strisy")
  set(_synth_src "${STRISO_SYNTH_STRISY_SRC}")
elseif(STRISO_FAUST_REGENERATE)
  if(NOT FAUST_EXECUTABLE)
    message(FATAL_ERROR "STRISO_FAUST_REGENERATE needs faust (or set it OFF to build the committed synth.cpp)")
  endif()
  set(_synth_src "${_gen}/faust/synth.cpp")
  add_custom_command(OUTPUT "${_synth_src}"
    COMMAND "${Python3_EXECUTABLE}" "${STRISO_FAUST_DIR}/faust2striso.py" --check-voicecount
            --dsp "${STRISO_FAUST_DIR}/synth.dsp" --template "${STRISO_FAUST_DIR}/faust_synth_template.cpp"
            --out "${_synth_src}" --synth-h "${PROJECT_SOURCE_DIR}/src/synth/synth.h"
    DEPENDS "${STRISO_FAUST_DIR}/synth.dsp" "${STRISO_FAUST_DIR}/fast.lib"
            "${STRISO_FAUST_DIR}/faust_synth_template.cpp" "${STRISO_FAUST_DIR}/faust2striso.py"
    COMMENT "Faust: synth.dsp -> synth.cpp" VERBATIM)
else()
  set(_synth_src "${STRISO_FAUST_DIR}/synth.cpp")
endif()

# --- ChibiOS --------------------------------------------------------------------------------------
chibios_import(CHIBIOS_DIR "${STRISO_CHIBIOS_DIR}" CONFDIR "${STRISO_CONF_DIR}" BOARDMK "${STRISO_BOARD_MK}")

# --- flags (rules.mk) -----------------------------------------------------------------------------
# no CMake defaults (e.g. -O3 -DNDEBUG for Release): every flag is set below
foreach(lang C CXX ASM)
  foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_${lang}_FLAGS${cfg} "")
  endforeach()
endforeach()
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <OBJECTS> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <LINK_LIBRARIES> -o <TARGET>")

separate_arguments(_use_opt UNIX_COMMAND "${STRISO_OPT}")
separate_arguments(_synth_opt UNIX_COMMAND "${STRISO_SYNTH_OPT}")
set(_mcflags -mcpu=cortex-m7 -mthumb)
set(_gc -ffunction-sections -fdata-sections -fno-common)
set(_fpu -mfloat-abi=softfp -mfpu=fpv5-d16)
set(_lto "")
if(STRISO_LTO)
  set(_lto -flto)
endif()
set(_opt ${_use_opt} ${_gc} ${_lto} ${_fpu})
set(_synth_lto ${_lto})
if(NOT STRISO_SYNTH_LTO)
  set(_synth_lto "")
endif()
set(_opt_synth ${_synth_opt} ${_gc} ${_synth_lto} ${_fpu})

set(_copt -std=gnu99)
set(_cwarn -Wall -Wextra -Wundef -Wstrict-prototypes)
set(_cppopt -fno-rtti -std=c++11)
set(_cppwarn -Wall -Wextra -Wundef)

set(_adefs CORTEX_USE_FPU=TRUE)
set(_defs CORTEX_USE_FPU=TRUE)
if(STRISO_SYNTH_ENGINE STREQUAL "strisy")
  list(APPEND _defs SYNTH_ENGINE_STRISY)
endif()
if(STRISO_VOICE STREQUAL "all")
  list(APPEND _defs SYNTH_VOICE_ALL)
elseif(NOT STRISO_VOICE STREQUAL "strisy")
  list(APPEND _defs "SYNTH_VOICE_HEADER=\"striso_dsp/voice_${STRISO_VOICE}.h\"" "SYNTH_VOICE_NS=${STRISO_VOICE}")
endif()
if(STRISO_USB_AUDIO_PRE_VOLUME)
  list(APPEND _defs USB_AUDIO_PRE_VOLUME=1)
endif()

set(_inc ${STRISO_FW_INCLUDE_DIRS} "${STRISO_DSP_INCLUDE_DIR}" "${_gen}" "${STRISO_CONF_DIR}" ${CHIBIOS_INC})

# --- object libraries in link order ---------------------------------------------------------------
function(_fw_objects name)
  cmake_parse_arguments(PARSE_ARGV 1 A "" "" "SOURCES;OPTIONS;DEFINES;INCLUDES")
  add_library(${name} OBJECT ${A_SOURCES})
  target_compile_options(${name} PRIVATE ${A_OPTIONS})
  target_compile_definitions(${name} PRIVATE ${A_DEFINES})
  target_include_directories(${name} PRIVATE ${_inc} ${A_INCLUDES})
  add_dependencies(${name} striso_config_editor striso_fwversion)
endfunction()

_fw_objects(fw_chibios_asm SOURCES ${CHIBIOS_XASMSRC} ${CHIBIOS_ASMSRC}
  OPTIONS ${_mcflags} ${_opt} DEFINES ${_adefs})
_fw_objects(fw_chibios_c SOURCES ${CHIBIOS_CSRC} "${STRISO_CHIBIOS_DIR}/os/various/syscalls.c"
  OPTIONS ${_mcflags} ${_opt} ${_copt} ${_cwarn} DEFINES ${_defs})
_fw_objects(fw_app_c SOURCES ${STRISO_FW_C_SOURCES}
  OPTIONS ${_mcflags} ${_opt} ${_copt} ${_cwarn} DEFINES ${_defs})
_fw_objects(fw_app_cxx SOURCES ${CHIBIOS_CPPSRC} ${STRISO_FW_CXX_SOURCES}
  OPTIONS ${_mcflags} ${_opt} ${_cppopt} ${_cppwarn} DEFINES ${_defs})
# the Faust synth includes fastpow.h without the striso_dsp/ prefix
_fw_objects(fw_synth SOURCES "${_synth_src}"
  OPTIONS ${_mcflags} ${_opt_synth} ${_cppopt} ${_cppwarn} DEFINES ${_defs}
  INCLUDES "${STRISO_DSP_INCLUDE_DIR}/striso_dsp")

# --- the firmware ---------------------------------------------------------------------------------
add_executable(striso_control
  $<TARGET_OBJECTS:fw_chibios_asm> $<TARGET_OBJECTS:fw_chibios_c> $<TARGET_OBJECTS:fw_app_c>
  $<TARGET_OBJECTS:fw_app_cxx> $<TARGET_OBJECTS:fw_synth>)
set_target_properties(striso_control PROPERTIES LINKER_LANGUAGE C SUFFIX ".elf"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" LINK_DEPENDS "${STRISO_LDSCRIPT}")
set(_out "${CMAKE_BINARY_DIR}/striso_control")
target_link_options(striso_control PRIVATE ${_mcflags} ${_opt} -nostartfiles
  "-Wl,-Map=${_out}.map,--cref,--no-warn-mismatch,--library-path=${CHIBIOS_STARTUPLD},--script=${STRISO_LDSCRIPT},--gc-sections,--print-memory-usage,--defsym=__process_stack_size__=0x400,--defsym=__main_stack_size__=0x400")
target_link_libraries(striso_control PRIVATE m)

set(_listing "")
if(STRISO_LISTING)
  set(_listing "${_out}.list")
endif()
add_custom_command(TARGET striso_control POST_BUILD
  COMMAND "${CMAKE_COMMAND}"
    "-DELF=$<TARGET_FILE:striso_control>" "-DBIN=${_out}.bin" "-DHEX=${_out}.hex" "-DUF2=${_out}.uf2"
    "-DOBJCOPY=${CMAKE_OBJCOPY}" "-DOBJDUMP=${CMAKE_OBJDUMP}" "-DNM=${CMAKE_NM}"
    "-DREADELF=${CMAKE_READELF}" "-DSIZE=${CMAKE_SIZE}" "-DPYTHON=${Python3_EXECUTABLE}"
    "-DSTACKCHECK=${STRISO_STACKCHECK}" "-DUF2CONV=${STRISO_UF2CONV}" "-DSYNTH_SRC=${_synth_src}"
    "-DLOAD_ADDR=${STRISO_LOAD_ADDRESS}" "-DUF2_FAMILY=0xa21e1295" "-DLISTING=${_listing}"
    -P "${PROJECT_SOURCE_DIR}/cmake/firmware_checks.cmake"
  BYPRODUCTS "${_out}.bin" "${_out}.hex" "${_out}.uf2"
  VERBATIM)

# USB descriptors as built (tools/usbaudio/check_descriptors.py reads the ELF): ctest --preset firmware
enable_testing()
add_test(NAME usb_descriptors
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/usbaudio/check_descriptors.py"
          --elf "$<TARGET_FILE:striso_control>")
