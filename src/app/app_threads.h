/*
 * Every thread of the firmware, in one table: app.cpp starts them from it (kThreads), and the
 * build reads it (cmake/firmware_checks.cmake, cmake/StrisoFirmware.cmake) to
 * - check each thread's source against its row: the working area's size (THD_WORKING_AREA), the
 *   priority it is created with (chThdCreateStatic) and its name (chRegSetThreadName), so the
 *   table and the modules can't drift apart;
 * - run the stack check (tools/stackcheck.py) of each thread with its row's size and margin;
 * - size main's stack (the linker's __process_stack_size__).
 *
 * A row: STRISO_THREAD(id, name, stack, priority, start, source, working area, entry, margin)
 *   id            app.cpp's name for it (Thread::id)
 *   name          the name it gives itself (StcI lists it)
 *   stack         its working area in bytes, as the source asks for it (ChibiOS adds the context
 *                 switch and exception frames on top), main's: the process stack
 *   priority      as chThdCreateStatic gets it, word for word
 *   start         the call that creates it (the module's start, after its own set-up); nullptr:
 *                 the kernel's own (main, idle)
 *   source        the file (under src/) with its working area and creation; synth: the synth
 *                 engine's (synth_strisy.cpp or the Faust synth); -: none (the kernel's)
 *   working area  the THD_WORKING_AREA variable (-: none)
 *   entry         its function as linked (mangled), the stack check's root
 *   margin        bytes the stack check adds to the deepest path: main runs on the process stack,
 *                 which has no reserve for the switch and exception frames (100 + 104); the synth
 *                 thread keeps the 256 bytes it always had
 * One row per line: the build reads the lines.
 */
#ifndef APP_THREADS_H
#define APP_THREADS_H

// clang-format off
#define STRISO_THREADS(X) \
  X(control,      "send messages", 1024, NORMALPRIO,     control_thread_start,         app/control_thread.cpp,  waControlThread,     _ZN12_GLOBAL__N_113ControlThreadEPv,  0) \
  X(blinker,      "blinker",       128,  NORMALPRIO,     blinker_start,                app/app.cpp,             waBlinker,           _ZN12_GLOBAL__N_17BlinkerEPv,         0) \
  X(synth,        "SYNTH",         4096, NORMALPRIO + 2, start_synth_thread,           synth,                   waSynthThread,       _ZL11synthThreadPv,                   256) \
  X(auxjack,      "auxjack",       320,  NORMALPRIO,     aux_jack_init,                drivers/aux_jack.c,      waAuxJack,           AuxJack,                              0) \
  X(scan_dump,    "scan_dump",     512,  LOWPRIO,        ks_capture_start_dump_thread, drivers/key_scanner.cpp, waScanDump,          _ZN12_GLOBAL__N_114ThreadScanDumpEPv, 0) \
  X(read_buttons, "read_buttons",  256,  NORMALPRIO,     ButtonReadStart,              drivers/button_read.c,   waThreadReadButtons, ThreadReadButtons,                    0) \
  X(motion,       "motion",        320,  NORMALPRIO,     MotionSensorStart,            drivers/motionsensor.c,  waThreadAccel,       ThreadAccel,                          0) \
  X(main,         "main",          1024, NORMALPRIO,     nullptr,                      -,                       -,                   main,                                 256) \
  X(idle,         "idle",          16,   IDLEPRIO,       nullptr,                      -,                       -,                   _idle_thread,                         0)
// clang-format on

#endif
