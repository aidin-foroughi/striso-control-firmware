# Toolchain file for the Striso board (STM32H743, Cortex-M7) with the GNU Arm Embedded toolchain.
#
# The compilers are found on PATH, or in ARM_TOOLCHAIN_DIR (the toolchain's bin directory, as a
# cache variable or environment variable). The project sets every compiler and linker flag itself
# (cmake/StrisoFirmware.cmake mirrors ChibiOS' rules.mk), so nothing is added here.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT ARM_TOOLCHAIN_DIR AND DEFINED ENV{ARM_TOOLCHAIN_DIR})
  set(ARM_TOOLCHAIN_DIR "$ENV{ARM_TOOLCHAIN_DIR}")
endif()
set(ARM_TOOLCHAIN_DIR "${ARM_TOOLCHAIN_DIR}" CACHE PATH "bin directory of the arm-none-eabi toolchain (empty: PATH)")
if(ARM_TOOLCHAIN_DIR)
  set(_arm_prefix "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-")
else()
  set(_arm_prefix "arm-none-eabi-")
endif()

set(CMAKE_C_COMPILER "${_arm_prefix}gcc")
set(CMAKE_CXX_COMPILER "${_arm_prefix}g++")
set(CMAKE_ASM_COMPILER "${_arm_prefix}gcc")
set(CMAKE_OBJCOPY "${_arm_prefix}objcopy" CACHE FILEPATH "")
set(CMAKE_OBJDUMP "${_arm_prefix}objdump" CACHE FILEPATH "")
set(CMAKE_SIZE "${_arm_prefix}size" CACHE FILEPATH "")
set(CMAKE_NM "${_arm_prefix}nm" CACHE FILEPATH "")
set(CMAKE_READELF "${_arm_prefix}readelf" CACHE FILEPATH "")
set(CMAKE_AR "${_arm_prefix}ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_arm_prefix}ranlib" CACHE FILEPATH "")

# no executables can run here: test the compilers by building a static library
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
