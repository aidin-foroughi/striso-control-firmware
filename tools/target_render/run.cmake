# One target render on QEMU (ctest target.*): cmake -DQEMU=<qemu-system-arm> -DELF=<elf>
#   -DOUT=<out.f32> -DPRESET=<n or -1> -DNAME=<test name> -DEXPECT=<hash or missing> -P run.cmake
# Fails on non-finite output, a peak over 8 or a hash other than EXPECT. With the environment
# variable STRISO_GOLDEN_OUT=<dir> it writes <dir>/<NAME>.hash instead of comparing.
get_filename_component(dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${dir}")
execute_process(
  COMMAND "${QEMU}" -M mps2-an500 -nographic -monitor none -serial none
          -semihosting-config "enable=on,target=native,arg=render_target,arg=${OUT},arg=${PRESET},arg=10"
          -kernel "${ELF}"
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT 900)
if(NOT rc EQUAL 0 OR NOT out MATCHES "hash ([0-9a-f]+) peak ([0-9.]+) nonfinite ([0-9]+)")
  message(FATAL_ERROR "QEMU run failed (${rc}): ${out}${err}")
endif()
set(hash "${CMAKE_MATCH_1}")
set(peak "${CMAKE_MATCH_2}")
set(nonfinite "${CMAKE_MATCH_3}")
message("${NAME}: hash ${hash} peak ${peak} nonfinite ${nonfinite}")
if(NOT nonfinite EQUAL 0)
  message(FATAL_ERROR "${nonfinite} non-finite samples")
endif()
if(peak GREATER 8)
  message(FATAL_ERROR "peak ${peak} over 8")
endif()
if(DEFINED ENV{STRISO_GOLDEN_OUT} AND NOT "$ENV{STRISO_GOLDEN_OUT}" STREQUAL "")
  file(WRITE "$ENV{STRISO_GOLDEN_OUT}/${NAME}.hash" "${NAME} ${hash}\n")
elseif(NOT hash STREQUAL EXPECT)
  message(FATAL_ERROR "target render hash ${hash}, expected ${EXPECT} (the board's arithmetic changed)")
endif()
