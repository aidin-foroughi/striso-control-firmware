/* chprintf for the virtual board: ChibiOS' chprintf.c (os/hal/lib/streams), ported so 'l' means
 * 32 bits as on the board's Cortex-M7 (see chprintf.c). */
#ifndef SV_HAL_CHPRINTF_H
#define SV_HAL_CHPRINTF_H

#include <stdarg.h>

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif
int chvprintf(BaseSequentialStream* chp, const char* fmt, va_list ap);
int chprintf(BaseSequentialStream* chp, const char* fmt, ...);
int chsnprintf(char* str, size_t size, const char* fmt, ...);
int chvsnprintf(char* str, size_t size, const char* fmt, va_list ap);
#ifdef __cplusplus
}
#endif

#endif
