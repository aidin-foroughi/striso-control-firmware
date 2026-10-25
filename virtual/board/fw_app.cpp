// src/app/app.cpp (the boot from the thread table, the blinker, the main loop) as the board builds
// it. Its start calls reach the virtual board's stand-ins where a module needs the hardware
// (buttons_host.c, motion_host.c, stubs_host.c); main() and the .fwinfo record (start.c) are the
// board's only: the virtual board's main thread calls app_run (main_host.c).
#include "app.cpp" // NOLINT(bugprone-suspicious-include): the board's own file, as it is
