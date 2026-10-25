# The firmware's own sources, by module. The C files are listed in the order the firmware Makefile
# linked them (the link order sets the memory layout): keep new files at the end of their list.
#   app/      main, control and messaging, configuration store, the config editor page
#   drivers/  codec (SAI), ADC key scanning, motion sensor, LEDs, aux jack, serial MIDI
#   usb/      USB descriptors and driver glue, bulk console, USB MIDI, USB audio
#   synth/    the synth thread and its controls; faust/ the Faust synth (synth.dsp)
# Sources in these directories that are not listed are hardware variants the board doesn't use
# (MPU6050.c, ws2812.c, adc_multi.c, exceptions.c), as in the Makefile.

set(STRISO_FW_C_SOURCES
  usb/usbcfg.c
  usb/usb_audio.c
  usb/bulk_usb.c
  usb/midi_usb.c
  drivers/midi_serial.c
  drivers/button_read.c
  drivers/motionsensor.c
  drivers/codec_tlv320aic3x_SAI.c
  app/start.c
  drivers/key_calibration.c)
set(STRISO_FW_CXX_SOURCES synth/synth_control.cpp drivers/key_scanner.cpp app/messaging.cpp app/control_thread.cpp app/console.cpp
  drivers/aux_jack.cpp drivers/led.cpp drivers/motion_filter.cpp app/config_store.cpp app/config_flash.cpp app/app.cpp)
set(STRISO_SYNTH_STRISY_SRC synth/synth_strisy.cpp)
set(STRISO_FAUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/synth/faust")
set(STRISO_CONFIG_EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app/config_editor")
set(STRISO_FW_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/app"
  "${CMAKE_CURRENT_SOURCE_DIR}/drivers"
  "${CMAKE_CURRENT_SOURCE_DIR}/usb"
  "${CMAKE_CURRENT_SOURCE_DIR}/synth")
foreach(v STRISO_FW_C_SOURCES STRISO_FW_CXX_SOURCES STRISO_SYNTH_STRISY_SRC)
  list(TRANSFORM ${v} PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
endforeach()

include(StrisoFirmware)

# board-arithmetic renders on QEMU (tools/target_render): ctest -L target_render
add_subdirectory("${PROJECT_SOURCE_DIR}/tools/target_render" "${CMAKE_BINARY_DIR}/target_render")
