# Post-build step of the firmware (cmake -P, run by the striso_control target after linking):
# makes the .bin/.hex, checks the ELF and only then writes the .uf2. If a check fails the build
# fails and no .uf2 is left behind, so a bad firmware can't be flashed by accident.
#
# Checks (as tools/build.sh did):
#   1. no software floating point helpers (__aeabi_fadd etc.): some file was compiled without the
#      FPU flags, and the DSP would be many times too slow;
#   2. every thread's stack (below): the worst call path from its entry (tools/stackcheck.py: a
#      conservative frame per function; fails on recursion and on undeclared indirect calls) plus a
#      margin must fit the thread's stack;
#   3. the load address is the application's (0x08040000), never the bootloader's.
#
# Variables: ELF BIN HEX UF2 OBJCOPY OBJDUMP NM READELF SIZE PYTHON STACKCHECK UF2CONV SYNTH_SRC SRC_DIR
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

# 2. thread stacks: every thread's worst call path (tools/stackcheck.py) must fit its stack.
# The threads are src/app/app_threads.h's rows (app.cpp starts them from the same table): name,
# stack, priority, the source and working area variable, the entry function, a margin on top of
# the deepest path. First each row is checked against its source, so the table can't drift from
# the modules: the working area's size (THD_WORKING_AREA), the priority chThdCreateStatic gets,
# the name chRegSetThreadName gives.
# - THD_WORKING_AREA threads: ChibiOS adds the context switch frame (port_intctx), the exception
#   frame (port_extctx, with FPU state) and 64 bytes to the size the source asks for, so the path
#   (without _port_switch's frame: --no-frame) must fit the size itself: margin 0. The synth thread
#   keeps the 256 bytes it always had.
# - main runs on the process stack (__process_stack_size__, StrisoFirmware.cmake: the table's main
#   row), which has no such reserve: 256 bytes for the switch and exception frames (100 + 104).
# - the idle thread: PORT_IDLE_THREAD_STACK_SIZE (16, the ChibiOS default; cfg/ doesn't change it).
# Interrupts run on the main stack (__main_stack_size__) and are not checked here.
include("${CMAKE_CURRENT_LIST_DIR}/app_threads.cmake")
striso_read_threads("${SRC_DIR}/app/app_threads.h" _threads)
foreach(t IN LISTS _threads)
  foreach(f name stack priority source wa entry)
    striso_thread_field("${t}" ${f} ${f})
  endforeach()
  if(source STREQUAL "-")
    continue()
  endif()
  if(source STREQUAL "synth")
    set(file "${SYNTH_SRC}")
  else()
    set(file "${SRC_DIR}/${source}")
  endif()
  file(READ "${file}" src)
  string(REGEX MATCH "THD_WORKING_AREA\\(${wa}, *([0-9]+)\\)" _ "${src}")
  if(NOT CMAKE_MATCH_1 STREQUAL stack)
    reject("app_threads.h: thread ${name}: ${file} declares ${wa} of '${CMAKE_MATCH_1}' bytes, the table ${stack}")
  endif()
  string(REGEX MATCH "chThdCreateStatic\\(${wa}, *sizeof\\(${wa}\\), *([^,]+)," _ "${src}")
  string(REPLACE " " "" p "${CMAKE_MATCH_1}")
  if(NOT p STREQUAL priority)
    reject("app_threads.h: thread ${name}: ${file} creates it at priority '${p}', the table ${priority}")
  endif()
  string(FIND "${src}" "chRegSetThreadName(\"${name}\")" at)
  if(at LESS 0)
    reject("app_threads.h: thread ${name}: ${file} doesn't name it \"${name}\"")
  endif()
endforeach()
# indirect calls (stream vmt methods, buffer queue callbacks) and the functions they can reach
set(_indirect
  # chprintf(stream): the stream's put/write, a driver's or the console's reply stream (OutStream)
  "--indirect=chprintf=_put,_write,_ZN9OutStream.*E(9vmt_write|7vmt_put)E.*"
  "--indirect=chvprintf=_put,_write,_ZN9OutStream.*E(9vmt_write|7vmt_put)E.*" # (not inlined: chsnprintf)
  --indirect main=_readt                   # PExReceive (inlined): chnReadTimeout
  # the console (console.cpp): the command table's handlers, the reply stream's timed writes
  "--indirect=_ZN7console7ConsoleIN12_GLOBAL__N_113BoardCommandsEE3runERKNS_7CommandI9OutStreamI13BulkUSBDriverEEE=_ZN12_GLOBAL__N_113BoardCommands.*"
  --indirect _ZN9OutStreamI13BulkUSBDriverE3putEh=_putt
  --indirect _ZN9OutStreamI13BulkUSBDriverE5writeEPKhj=_writet
  --indirect obqPostFullBufferS=obnotify   # USB bulk/MIDI output queues
  --indirect ibqReleaseEmptyBufferS=ibnotify
  "--indirect=oqWriteTimeout=notify[0-9]*" # serial (DIN MIDI) queues
  "--indirect=oqPutTimeout=notify[0-9]*"
  "--indirect=iqReadTimeout=notify[0-9]*")
set(_roots "")
foreach(t IN LISTS _threads)
  striso_thread_field("${t}" entry root)
  list(APPEND _roots "${root}")
endforeach()
execute_process(COMMAND "${OBJDUMP}" -d --no-show-raw-insn "${ELF}"
                COMMAND "${PYTHON}" "${STACKCHECK}" --no-frame _port_switch ${_indirect} ${_roots}
                OUTPUT_VARIABLE depths OUTPUT_STRIP_TRAILING_WHITESPACE RESULTS_VARIABLE rcs)
list(GET rcs 1 rc)
if(NOT rc EQUAL 0)
  reject("thread stack check failed: ${depths}")
endif()
string(REPLACE "\n" ";" depths "${depths}")
foreach(t IN LISTS _threads)
  foreach(f name stack source wa entry margin)
    striso_thread_field("${t}" ${f} ${f})
  endforeach()
  set(root "${entry}")
  set(var "${wa}")
  set(wa "${stack}")
  if(source STREQUAL "-")
    set(where "fixed")
  elseif(source STREQUAL "synth")
    cmake_path(GET SYNTH_SRC FILENAME where)
  else()
    cmake_path(GET source FILENAME where)
  endif()
  set(deepest "")
  foreach(d IN LISTS depths)
    if(d MATCHES "^${root} ([0-9]+) (.*)$")
      set(deepest "${CMAKE_MATCH_1}")
      set(path "${CMAKE_MATCH_2}")
    endif()
  endforeach()
  if(deepest STREQUAL "" OR wa STREQUAL "")
    reject("thread stack check: no result for ${name} (${root}, ${where} ${var})")
  endif()
  math(EXPR need "${deepest} + ${margin}")
  message("thread stack ${name} (${where}): deepest ${deepest} bytes [${path}] + ${margin} margin = ${need} of ${wa} bytes")
  if(need GREATER wa)
    reject("thread ${name} stack overflow: needs ~${need} bytes, its stack is ${wa} bytes")
  endif()
endforeach()

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
