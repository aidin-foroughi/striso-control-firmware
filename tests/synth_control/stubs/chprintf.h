/* Host stand-in for ChibiOS' chprintf.h (tests/synth_control) */
#include "hal.h"
extern "C" int chprintf(BaseSequentialStream* chp, const char* fmt, ...);
extern "C" int chsnprintf(char* str, size_t size, const char* fmt, ...);
