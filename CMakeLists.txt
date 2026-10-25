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
# the engines repository: a git submodule, pinned by commit (awva/README.md)
set(AWVA_DIR "${PROJECT_SOURCE_DIR}/awva")
if(NOT EXISTS "${AWVA_DIR}/dsp/CMakeLists.txt")
  message(FATAL_ERROR "no awva at ${AWVA_DIR}: git submodule update --init awva")
endif()
set(STRISO_DSP_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/dsp/include")           # the firmware's: strisy, ump_out
set(AWVA_INCLUDE_DIR "${AWVA_DIR}/dsp/include")                          # awva's
set(STRISO_DSP_INCLUDE_DIRS "${STRISO_DSP_INCLUDE_DIR}" "${AWVA_INCLUDE_DIR}")
# the scripted performances, harness_util.h, rec/: the submodule's unless given (-DAWVA_HARNESS_DIR);
# not cached, so a build directory configured before a move can't keep a stale path
if(NOT AWVA_HARNESS_DIR)
  set(AWVA_HARNESS_DIR "${AWVA_DIR}/tools/harness")
elseif(NOT EXISTS "${AWVA_HARNESS_DIR}/performance.h")
  message(FATAL_ERROR "AWVA_HARNESS_DIR=${AWVA_HARNESS_DIR} has no performance.h (a stale cache? configure the build directory again)")
endif()
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
    add_subdirectory(awva/sdk sdk) # (the virtual board's tests compare the board with the SDK's shell)
  endif()
  if(STRISO_HOST_TESTS)
    add_subdirectory(tools/harness)
    add_subdirectory(tests)
  endif()
  if(STRISO_VIRTUAL)
    add_subdirectory(virtual)
  endif()
endif()
