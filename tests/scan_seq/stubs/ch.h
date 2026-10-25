/* A stand-in for ChibiOS (ch.h) for the key scan's sequence (tests/scan_seq): the locks, the
 * thread reference and the wakeup, which test_scan_seq.cpp records. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int id; } thread_t;
#define CH_STATE_SUSPENDED 3
void chSysLock(void);
void chSysUnlock(void);
void chSysLockFromISR(void);
void chSysUnlockFromISR(void);
thread_t *chThdGetSelfX(void);
thread_t *chSchReadyI(thread_t *tp);
void chSchGoSleepS(int state);
#ifdef __cplusplus
}
#endif
