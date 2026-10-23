# ChibiOS as CMake object libraries, built from ChibiOS' own make fragments.
#
# chibios_import(CHIBIOS_DIR <dir> CONFDIR <dir> BOARDMK <file>)
#   Runs cmake/chibios_lists.mk through make at configure time (the fragments decide which HAL
#   drivers to build from halconf.h when USE_SMART_BUILD is on, as the firmware Makefile did) and
#   sets, in the caller's scope, absolute paths:
#     CHIBIOS_CSRC CHIBIOS_CPPSRC CHIBIOS_ASMSRC CHIBIOS_XASMSRC CHIBIOS_INC CHIBIOS_STARTUPLD
#   in the fragments' order. The configuration reruns when halconf.h or a fragment changes.

function(chibios_import)
  cmake_parse_arguments(PARSE_ARGV 0 A "" "CHIBIOS_DIR;CONFDIR;BOARDMK" "")
  find_program(STRISO_MAKE NAMES make gmake REQUIRED)
  foreach(v CHIBIOS_DIR CONFDIR BOARDMK)
    cmake_path(RELATIVE_PATH A_${v} BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE rel_${v})
  endforeach()
  if(NOT EXISTS "${A_CHIBIOS_DIR}/os/rt/include/ch.h")
    message(FATAL_ERROR "ChibiOS not found in ${A_CHIBIOS_DIR}: run git submodule update --init")
  endif()
  execute_process(
    COMMAND "${STRISO_MAKE}" -s -C "${PROJECT_SOURCE_DIR}" -f "${PROJECT_SOURCE_DIR}/cmake/chibios_lists.mk"
            "CHIBIOS=./${rel_CHIBIOS_DIR}" "CONFDIR=./${rel_CONFDIR}" "BOARDMK=${rel_BOARDMK}" print
    OUTPUT_VARIABLE lists RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "reading the ChibiOS make fragments failed:\n${err}")
  endif()
  file(WRITE "${CMAKE_BINARY_DIR}/chibios_lists.cmake" "${lists}")
  include("${CMAKE_BINARY_DIR}/chibios_lists.cmake")
  # rerun when the inputs of the smart build change
  file(GLOB_RECURSE frags "${A_CHIBIOS_DIR}/os/*.mk")
  set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${A_CONFDIR}/halconf.h" "${A_BOARDMK}" "${PROJECT_SOURCE_DIR}/cmake/chibios_lists.mk" ${frags})
  foreach(kind ALLCSRC ALLCPPSRC ALLASMSRC ALLXASMSRC ALLINC STARTUPLD)
    set(abs "")
    foreach(p IN LISTS CHIBIOS_${kind})
      cmake_path(ABSOLUTE_PATH p BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE a)
      string(REGEX REPLACE "/$" "" a "${a}")
      list(APPEND abs "${a}")
    endforeach()
    string(REPLACE "ALL" "" name "${kind}")
    set(CHIBIOS_${name} "${abs}" PARENT_SCOPE)
  endforeach()
endfunction()
