cmake_minimum_required(VERSION 3.25)
project(striso_control LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# --- source layout ----------------------------------------------------------------------------------
set(S "${PROJECT_SOURCE_DIR}")
set(STRISO_CHIBIOS_DIR "${S}/ChibiOS")
set(STRISO_CONF_DIR "${S}/cfg")
set(STRISO_BOARD_MK "${S}/board/board.mk")
set(STRISO_LDSCRIPT "${S}/STM32H743xI_uf2.ld")
set(STRISO_DSP_DIR "${S}")
set(STRISO_FW_INCLUDE_DIRS "${S}")
set(STRISO_FW_C_SOURCES
  usbcfg.c usb_audio.c pconnection.c bulk_usb.c midi_usb.c midi_serial.c button_read.c
  messaging.c motionsensor.c codec_tlv320aic3x_SAI.c led.c config_store.c aux_jack.c main.c)
list(TRANSFORM STRISO_FW_C_SOURCES PREPEND "${S}/")
set(STRISO_FW_CXX_SOURCES "${S}/synth_control.cpp")
set(STRISO_SYNTH_STRISY_SRC "${S}/synth_strisy.cpp")
set(STRISO_SYNTH_FAUST_SRC "${S}/synth.cpp")
set(STRISO_CONFIG_EDITOR_DIR "${S}/config_editor")
set(STRISO_CONFIG_EDITOR_H "${S}/config_editor/config_editor.h")
set(STRISO_CONFIG_EDITOR_CMD "${S}/config_editor/config_editor.py")
set(STRISO_CONFIG_EDITOR_WD "${S}")
set(STRISO_STACKCHECK "${S}/tools/stackcheck.py")
set(STRISO_UF2CONV "${S}/uf2/utils/uf2conv.py")

if(CMAKE_CROSSCOMPILING)
  enable_language(ASM)
  include(StrisoFirmware)
endif()
