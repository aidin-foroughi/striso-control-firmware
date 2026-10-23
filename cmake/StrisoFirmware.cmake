# The firmware: flags and link exactly as the ChibiOS make rules (rules.mk) did for the firmware
# Makefile, so the same toolchain makes a byte-identical binary.
#
# Object order on the link line matters for the layout (and with LTO for the code): ChibiOS'
# assembler files, ChibiOS' C files, syscalls.c, the firmware's C files, then the C++ files, as in
# rules.mk (OBJS = ASMX ASM C CPP). Each group is an object library, linked in that order.

include(ChibiOS)
include(StrisoVersion)

# --- options --------------------------------------------------------------------------------------
set(STRISO_SYNTH_ENGINE "strisy" CACHE STRING "synth engine: strisy (C++ voice engines) or faust (synth.dsp)")
set_property(CACHE STRISO_SYNTH_ENGINE PROPERTY STRINGS strisy faust)
set(STRISO_VOICE "strisy" CACHE STRING "voice engine (strisy engine only): strisy, all, or <name> for voice_<name>.h")
set(STRISO_OPT "-O1 -ggdb -fomit-frame-pointer -falign-functions=16" CACHE STRING "optimisation flags (USE_OPT)")
set(STRISO_SYNTH_OPT "-O3 -ggdb -fomit-frame-pointer -falign-functions=16" CACHE STRING "optimisation flags of the synth DSP file (SYNTH_OPT)")
option(STRISO_LTO "link time optimisation" ON)
option(STRISO_SYNTH_LTO "compile the synth DSP file with LTO too (OFF: a normal object, all per-file flags apply)" ON)
option(STRISO_LISTING "write an objdump -S listing next to the ELF" OFF)
set(STRISO_LOAD_ADDRESS "0x08040000" CACHE STRING "expected load address: the application region after the UF2 bootloader")

if(NOT STRISO_SYNTH_ENGINE MATCHES "^(strisy|faust)$")
  message(FATAL_ERROR "STRISO_SYNTH_ENGINE must be strisy or faust, not '${STRISO_SYNTH_ENGINE}'")
endif()
if(NOT STRISO_VOICE MATCHES "^(strisy|all)$" AND NOT EXISTS "${STRISO_DSP_DIR}/voice_${STRISO_VOICE}.h")
  message(FATAL_ERROR "STRISO_VOICE=${STRISO_VOICE}: no ${STRISO_DSP_DIR}/voice_${STRISO_VOICE}.h")
endif()

striso_git_version(_git_version)
set(STRISO_FWVERSION "${_git_version}")
if(NOT STRISO_VOICE STREQUAL "strisy")
  string(APPEND STRISO_FWVERSION "_${STRISO_VOICE}")
endif()
message(STATUS "firmware version: ${STRISO_FWVERSION}")

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
set(_defs CORTEX_USE_FPU=TRUE "FWVERSION=\"${STRISO_FWVERSION}\"")
if(STRISO_SYNTH_ENGINE STREQUAL "strisy")
  list(APPEND _defs SYNTH_ENGINE_STRISY)
  set(_synth_src "${STRISO_SYNTH_STRISY_SRC}")
else()
  set(_synth_src "${STRISO_SYNTH_FAUST_SRC}")
endif()
if(STRISO_VOICE STREQUAL "all")
  list(APPEND _defs SYNTH_VOICE_ALL)
elseif(NOT STRISO_VOICE STREQUAL "strisy")
  list(APPEND _defs "SYNTH_VOICE_HEADER=\"voice_${STRISO_VOICE}.h\"" "SYNTH_VOICE_NS=${STRISO_VOICE}")
endif()

set(_inc ${STRISO_FW_INCLUDE_DIRS} "${STRISO_CONF_DIR}" ${CHIBIOS_INC})

# --- generated: the config editor page --------------------------------------------------------------
add_custom_command(OUTPUT "${STRISO_CONFIG_EDITOR_H}"
  COMMAND "${Python3_EXECUTABLE}" ${STRISO_CONFIG_EDITOR_CMD}
  WORKING_DIRECTORY "${STRISO_CONFIG_EDITOR_WD}"
  DEPENDS "${STRISO_CONFIG_EDITOR_DIR}/config.htm" "${STRISO_CONFIG_EDITOR_DIR}/config_editor.py"
  COMMENT "Generating config_editor.h")
add_custom_target(striso_config_editor DEPENDS "${STRISO_CONFIG_EDITOR_H}")

# --- object libraries in link order ---------------------------------------------------------------
function(_fw_objects name)
  cmake_parse_arguments(PARSE_ARGV 1 A "" "" "SOURCES;OPTIONS;DEFINES")
  add_library(${name} OBJECT ${A_SOURCES})
  target_compile_options(${name} PRIVATE ${A_OPTIONS})
  target_compile_definitions(${name} PRIVATE ${A_DEFINES})
  target_include_directories(${name} PRIVATE ${_inc})
  add_dependencies(${name} striso_config_editor)
endfunction()

_fw_objects(fw_chibios_asm SOURCES ${CHIBIOS_XASMSRC} ${CHIBIOS_ASMSRC}
  OPTIONS ${_mcflags} ${_opt} DEFINES ${_adefs})
_fw_objects(fw_chibios_c SOURCES ${CHIBIOS_CSRC} "${STRISO_CHIBIOS_DIR}/os/various/syscalls.c"
  OPTIONS ${_mcflags} ${_opt} ${_copt} ${_cwarn} DEFINES ${_defs})
_fw_objects(fw_app_c SOURCES ${STRISO_FW_C_SOURCES}
  OPTIONS ${_mcflags} ${_opt} ${_copt} ${_cwarn} DEFINES ${_defs})
_fw_objects(fw_app_cxx SOURCES ${CHIBIOS_CPPSRC} ${STRISO_FW_CXX_SOURCES}
  OPTIONS ${_mcflags} ${_opt} ${_cppopt} ${_cppwarn} DEFINES ${_defs})
_fw_objects(fw_synth SOURCES "${_synth_src}"
  OPTIONS ${_mcflags} ${_opt_synth} ${_cppopt} ${_cppwarn} DEFINES ${_defs})

# --- the firmware ---------------------------------------------------------------------------------
add_executable(striso_control
  $<TARGET_OBJECTS:fw_chibios_asm> $<TARGET_OBJECTS:fw_chibios_c> $<TARGET_OBJECTS:fw_app_c>
  $<TARGET_OBJECTS:fw_app_cxx> $<TARGET_OBJECTS:fw_synth>)
set_target_properties(striso_control PROPERTIES LINKER_LANGUAGE C SUFFIX ".elf"
  LINK_DEPENDS "${STRISO_LDSCRIPT}")
set(_map "${CMAKE_CURRENT_BINARY_DIR}/striso_control.map")
target_link_options(striso_control PRIVATE ${_mcflags} ${_opt} -nostartfiles
  "-Wl,-Map=${_map},--cref,--no-warn-mismatch,--library-path=${CHIBIOS_STARTUPLD},--script=${STRISO_LDSCRIPT},--gc-sections,--print-memory-usage,--defsym=__process_stack_size__=0x400,--defsym=__main_stack_size__=0x400")
target_link_libraries(striso_control PRIVATE m)

set(_out "${CMAKE_CURRENT_BINARY_DIR}/striso_control")
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
