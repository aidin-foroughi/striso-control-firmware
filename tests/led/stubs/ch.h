/* A stand-in for ChibiOS (ch.h) for led.cpp and the old led.c: the system time and a mutex, which
 * test_led.cpp implements (the time is the test's, the mutex only checks it is balanced). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t systime_t;
typedef uint32_t sysinterval_t;
typedef struct { int locked; } mutex_t;
systime_t chVTGetSystemTime(void);
void chMtxObjectInit(mutex_t *mp);
void chMtxLock(mutex_t *mp);
void chMtxUnlock(mutex_t *mp);
#ifdef __cplusplus
}
#endif
