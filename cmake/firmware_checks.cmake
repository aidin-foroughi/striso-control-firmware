# Post-build step of the firmware (cmake -P, run by the striso_control target after linking):
# makes the .bin/.hex, checks the ELF and only then writes the .uf2. If a check fails the build
# fails and no .uf2 is left behind, so a bad firmware can't be flashed by accident.
#
# Checks (as tools/build.sh did):
#   1. no software floating point helpers (__aeabi_fadd etc.): some file was compiled without the
#      FPU flags, and the DSP would be many times too slow;
#   2. the synth thread's stack: worst call path from synthThread (tools/stackcheck.py: a
#      conservative frame per function; fails on recursion and indirect calls) + 256 bytes for the
#      exception frame the hardware stacks (with FPU state) must fit the working area;
#   3. the load address is the application's (0x08040000), never the bootloader's.
#
# Variables: ELF BIN HEX UF2 OBJCOPY OBJDUMP NM READELF SIZE PYTHON STACKCHECK UF2CONV SYNTH_SRC
#            LOAD_ADDR UF2_FAMILY LISTING(optional: write <elf>.list with objdump -S)

file(REMOVE "${UF2}")

function(reject msg)
  message("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
  message("!! BUILD REJECTED: ${msg}")
  message("!! ${UF2} not written, so it cannot be flashed.")
  message("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
  message(FATAL_ERROR "firmware check failed")
endfunction()

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    reject("command failed (${rc}): ${ARGN}")
  endif()
endfunction()

run("${OBJCOPY}" -O ihex "${ELF}" "${HEX}")
run("${OBJCOPY}" -O binary "${ELF}" "${BIN}")
if(LISTING)
  execute_process(COMMAND "${OBJDUMP}" -S "${ELF}" OUTPUT_FILE "${LISTING}")
endif()
execute_process(COMMAND "${SIZE}" "${ELF}")

# 1. software floating point helpers
execute_process(COMMAND "${NM}" "${ELF}" OUTPUT_VARIABLE syms RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  reject("nm failed")
endif()
string(REGEX MATCHALL " __aeabi_[fd](add|sub|mul|div)\n" softfp "${syms}")
if(softfp)
  string(REPLACE "\n" "" softfp "${softfp}")
  reject("software float helpers linked in (missing FPU flags?):${softfp}")
endif()

# 2. synth thread stack
file(READ "${SYNTH_SRC}" src)
string(REGEX MATCH "THD_WORKING_AREA\\(waSynthThread, *([0-9]+)\\)" _ "${src}")
set(wa "${CMAKE_MATCH_1}")
execute_process(COMMAND "${OBJDUMP}" -d --no-show-raw-insn "${ELF}"
                COMMAND "${PYTHON}" "${STACKCHECK}" _ZL11synthThreadPv
                OUTPUT_VARIABLE depth OUTPUT_STRIP_TRAILING_WHITESPACE RESULTS_VARIABLE rcs)
list(GET rcs 1 rc)
if(NOT rc EQUAL 0)
  reject("synth thread stack check failed: ${depth}")
endif()
string(REGEX MATCH "^([0-9]+) (.*)$" _ "${depth}")
set(deepest "${CMAKE_MATCH_1}")
set(path "${CMAKE_MATCH_2}")
math(EXPR need "${deepest} + 256")
cmake_path(GET SYNTH_SRC FILENAME synth_name)
message("synth thread stack (${synth_name}): deepest ${deepest} bytes [${path}] + 256 margin = ${need} of ${wa} bytes")
if(wa STREQUAL "" OR need GREATER wa)
  reject("synth thread stack overflow: needs ~${need} bytes, working area is ${wa} bytes")
endif()

# 3. load address
execute_process(COMMAND "${READELF}" -l "${ELF}" OUTPUT_VARIABLE phdrs RESULT_VARIABLE rc)
# the first LOAD segment's virtual address (readelf -l: LOAD Offset VirtAddr PhysAddr ...)
string(REGEX MATCH "\n *LOAD +0x[0-9a-f]+ +(0x[0-9a-f]+)" _ "${phdrs}")
set(start "${CMAKE_MATCH_1}")
if(NOT start STREQUAL LOAD_ADDR)
  reject("unexpected load address '${start}' (expected ${LOAD_ADDR})")
endif()

# all checks passed: the UF2 for the UF2 bootloader
run("${PYTHON}" "${UF2CONV}" -c -f "${UF2_FAMILY}" -b "${start}" "${BIN}" -o "${UF2}")
message("build checks passed (hardware FPU, load address ${start})")
message("firmware: ${UF2}")
