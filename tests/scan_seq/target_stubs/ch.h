/* ChibiOS for the scan sequence on QEMU (target_scan_seq.cpp): the interrupt's locks and wakeup
 * as no-ops (as cheap as the board's, near enough). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int id; } thread_t;
#define CH_STATE_SUSPENDED 3
extern thread_t target_thread;
static inline void chSysLock(void) {}
static inline void chSysUnlock(void) {}
static inline void chSysLockFromISR(void) { __asm__ volatile("" ::: "memory"); }
static inline void chSysUnlockFromISR(void) { __asm__ volatile("" ::: "memory"); }
static inline thread_t *chThdGetSelfX(void) { return &target_thread; }
static inline thread_t *chSchReadyI(thread_t *tp) { return tp; }
static inline void chSchGoSleepS(int state) { (void)state; }
#ifdef __cplusplus
}
#endif
