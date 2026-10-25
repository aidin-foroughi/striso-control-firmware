# The virtual board (striso_virtual): the firmware's logic as a shared library for the PC, built
# from the board's own source files on a ChibiOS/HAL stand-in (hal/), with host glue for the
# hardware-only parts (board/). See include/striso_virtual.h for the API, README "Testing without
# the hardware" for its use.
#
#   striso_virtual          the library (.dll / .dylib / .so)
#   striso_virtual_tests    doctest: the golden renders through the board, the console protocol
#                           against transcripts (tests/transcripts), the API
#   ctest -L virtual        runs them

set(_fw "${PROJECT_SOURCE_DIR}/src")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
# ChibiOS' HAL code the board's USB stack runs on (buffer queues, the USB driver, memory streams)
set(_hal "${STRISO_CHIBIOS_DIR}/os/hal")
if(NOT EXISTS "${_hal}/src/hal_buffers.c")
  message(FATAL_ERROR "virtual board: ChibiOS not found at ${STRISO_CHIBIOS_DIR} "
    "(git submodule update --init third_party/ChibiOS, or -DSTRISO_CHIBIOS_DIR=<a checkout's>)")
endif()

# the version string the board reports: the VOICE=all firmware's (cmake/StrisoVersion.cmake)
include(StrisoVersion)
add_custom_target(striso_virtual_fwversion
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DOUT=${_gen}/fwversion.h" "-DSUFFIX=_all"
          -P "${PROJECT_SOURCE_DIR}/cmake/StrisoVersion.cmake"
  BYPRODUCTS "${_gen}/fwversion.h"
  COMMENT "" VERBATIM)

add_library(striso_virtual SHARED
  # the library's front and the firmware instance
  src/api.cpp
  src/instance.cpp
  # ChibiOS / HAL stand-in: the kernel, the board's peripherals, the USB OTG core with the bus
  hal/sched.cpp
  hal/hal.c
  hal/usb_host.c
  hal/chprintf.c
  # ChibiOS' own HAL code
  "${_hal}/src/hal_buffers.c"
  "${_hal}/src/hal_usb.c"
  "${_hal}/lib/streams/memstreams.c"
  # the board without its hardware: main.c's threads, keys, motion, USB audio, stubs
  board/main_host.c
  board/buttons_host.c
  board/motion_host.c
  board/usb_audio_host.c
  board/stubs_host.c
  # the firmware's own files (fw_*: the file itself, compiled with the virtual board's flash)
  board/fw_config_store.c
  board/fw_pconnection.c
  board/fw_usbcfg.c
  board/fw_synth_strisy.cpp
  "${_fw}/usb/bulk_usb.c"
  "${_fw}/usb/midi_usb.c"
  "${_fw}/synth/synth_control.cpp"
  "${_fw}/app/messaging.cpp"
  "${_fw}/app/control_thread.cpp"
  "${_fw}/drivers/key_calibration.c"
  "${_fw}/drivers/led.cpp"
  "${_fw}/drivers/motion_filter.cpp")
add_dependencies(striso_virtual striso_virtual_fwversion)
target_include_directories(striso_virtual
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # the stand-in headers come first: they replace ChibiOS' kernel, hal.h and the OTG LLD's header
    "${CMAKE_CURRENT_SOURCE_DIR}/hal/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/hal"
    "${_hal}/include"
    "${_hal}/lib/streams"
    "${CMAKE_CURRENT_SOURCE_DIR}/board"
    "${_fw}/app" "${_fw}/drivers" "${_fw}/usb" "${_fw}/synth"
    "${PROJECT_SOURCE_DIR}/board"
    "${STRISO_DSP_INCLUDE_DIR}"
    "${_gen}")
# as the firmware build (cmake/StrisoFirmware.cmake) for VOICE=all with the C++ engines
target_compile_definitions(striso_virtual PRIVATE STRISO_VIRTUAL_BUILD SYNTH_ENGINE_STRISY SYNTH_VOICE_ALL
  CORTEX_USE_FPU=TRUE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
  # Windows UCRT: no POSIX-style names (its strset(char*, int) clashes with config_store.c strset)
  _CRT_DECLARE_NONSTDC_NAMES=0)
set_target_properties(striso_virtual PROPERTIES
  C_EXTENSIONS ON CXX_EXTENSIONS ON
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)
target_link_libraries(striso_virtual PRIVATE striso::dsp striso_host_options)
# the DSP as the host renderers build it (-O2, no FMA contraction): the goldens' arithmetic
target_compile_options(striso_virtual PRIVATE -O2 -ffp-contract=off -fno-strict-aliasing
  -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-unused-function
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-implicit-fallthrough
  # pconnection.c cmd_threads prints addresses as 32 bits (on the PC: the low half)
  -Wno-pointer-to-int-cast $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-void-pointer-to-int-cast>)
if(APPLE)
  target_compile_options(striso_virtual PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/board/sv_macho_sections.h")
elseif(UNIX)
  # a board's private copy of the library must bind to its own globals, not the first copy's
  target_link_options(striso_virtual PRIVATE -Wl,-Bsymbolic)
endif()
find_package(Threads REQUIRED)
target_link_libraries(striso_virtual PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(STRISO_SANITIZE)
  # ASan's own runtime must not be reloaded with each copy: the tests run with one copy at a time
  target_compile_definitions(striso_virtual PRIVATE STRISO_VIRTUAL_SANITIZE)
endif()

add_subdirectory(tests)
