cmake_minimum_required(VERSION 3.25)
project(striso_control
  DESCRIPTION "Striso board firmware: controller, MIDI/MPE, internal synth"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# --- layout (see README "Building") ---------------------------------------------------------------
set(STRISO_CHIBIOS_DIR "${PROJECT_SOURCE_DIR}/third_party/ChibiOS")
set(STRISO_UF2CONV "${PROJECT_SOURCE_DIR}/third_party/uf2/utils/uf2conv.py")
set(STRISO_CONF_DIR "${PROJECT_SOURCE_DIR}/cfg")
set(STRISO_BOARD_MK "${PROJECT_SOURCE_DIR}/board/board.mk")
set(STRISO_LDSCRIPT "${PROJECT_SOURCE_DIR}/ld/STM32H743xI_uf2.ld")
set(STRISO_DSP_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/dsp/include")
set(STRISO_STACKCHECK "${PROJECT_SOURCE_DIR}/tools/stackcheck.py")

# the DSP library (voice engines, effects): header only, for the board and the host
add_subdirectory(dsp)

if(CMAKE_CROSSCOMPILING)
  # the firmware (presets firmware, firmware-debug)
  enable_language(ASM)
  add_subdirectory(src)
endif()
