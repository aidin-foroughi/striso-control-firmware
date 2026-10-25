cmake_minimum_required(VERSION 3.25)
project(striso_control
  DESCRIPTION "Striso board firmware: controller, MIDI/MPE, internal synth"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# --- layout (see README "Building") ---------------------------------------------------------------
# ChibiOS: the submodule (a git worktree may use its main checkout's: -DSTRISO_CHIBIOS_DIR=...)
set(STRISO_CHIBIOS_DIR "${PROJECT_SOURCE_DIR}/third_party/ChibiOS" CACHE PATH "ChibiOS checkout")
set(STRISO_UF2CONV "${PROJECT_SOURCE_DIR}/third_party/uf2/utils/uf2conv.py")
set(STRISO_CONF_DIR "${PROJECT_SOURCE_DIR}/cfg")
set(STRISO_BOARD_MK "${PROJECT_SOURCE_DIR}/board/board.mk")
set(STRISO_LDSCRIPT "${PROJECT_SOURCE_DIR}/ld/STM32H743xI_uf2.ld")
# the engines repository: a git submodule, pinned by commit (engines/README.md)
set(STRISO_ENGINES_DIR "${PROJECT_SOURCE_DIR}/engines")
if(NOT EXISTS "${STRISO_ENGINES_DIR}/dsp/CMakeLists.txt")
  message(FATAL_ERROR "no engines repository at ${STRISO_ENGINES_DIR}: git submodule update --init engines")
endif()
set(STRISO_DSP_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/dsp/include")           # the firmware's: strisy, ump_out
set(STRISO_ENGINES_INCLUDE_DIR "${STRISO_ENGINES_DIR}/dsp/include")      # the engines repo's
set(STRISO_DSP_INCLUDE_DIRS "${STRISO_DSP_INCLUDE_DIR}" "${STRISO_ENGINES_INCLUDE_DIR}")
set(STRISO_HARNESS_DIR "${STRISO_ENGINES_DIR}/tools/harness" CACHE PATH "the scripted performances, harness_util.h, rec/")
set(STRISO_STACKCHECK "${PROJECT_SOURCE_DIR}/tools/stackcheck.py")

# the DSP library (voice engines, effects): header only, for the board and the host
add_subdirectory(dsp)

enable_testing()

if(CMAKE_CROSSCOMPILING)
  # the firmware (presets firmware, firmware-debug)
  enable_language(ASM)
  add_subdirectory(src)
else()
  # the PC (presets host, host-asan): renderers and unit tests
  include(StrisoHost)
  option(STRISO_HOST_TESTS "host: the renderers and unit tests (tools/harness, tests)" ON)
  option(STRISO_VIRTUAL "host: the virtual board, striso_virtual (virtual/)" ON)
  if(STRISO_HOST_TESTS OR STRISO_VIRTUAL)
    add_subdirectory(engines/sdk sdk) # (the virtual board's tests compare the board with the SDK's shell)
  endif()
  if(STRISO_HOST_TESTS)
    add_subdirectory(tools/harness)
    add_subdirectory(tests)
  endif()
  if(STRISO_VIRTUAL)
    add_subdirectory(virtual)
  endif()
endif()
