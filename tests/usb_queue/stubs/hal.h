/* Host stand-in for ChibiOS' hal.h: just enough OSAL to compile ChibiOS' own hal_buffers.c (the
 * output buffers queue of the bulk console and USB MIDI) on the PC. osalDbgAssert counts and
 * reports a failed assertion instead of halting: on the board (CH_DBG_ENABLE_ASSERTS TRUE) it is
 * chSysHalt, a frozen instrument. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int32_t msg_t;
typedef uint32_t sysinterval_t;
typedef struct {
  int dummy;
} threads_queue_t;
#define MSG_OK 0
#define MSG_TIMEOUT -1
#define MSG_RESET -2
#define TIME_IMMEDIATE ((sysinterval_t)0)
#define TIME_INFINITE ((sysinterval_t)-1)
extern int halts;
#define osalDbgAssert(c, r)                                                                   \
  do {                                                                                        \
    if (!(c)) {                                                                               \
      halts++;                                                                                \
      printf("  osalDbgAssert failed: \"%s\" (%s:%d): chSysHalt on the board\n", r, __FILE__, \
             __LINE__);                                                                       \
    }                                                                                         \
  } while (0)
#define osalDbgCheck(c) osalDbgAssert(c, #c)
#define osalDbgCheckClassI()
#define osalDbgCheckClassS()
#define osalSysLock()
#define osalSysUnlock()
static inline void osalThreadQueueObjectInit(threads_queue_t *q) { (void)q; }
static inline void osalThreadDequeueAllI(threads_queue_t *q, msg_t m) {
  (void)q;
  (void)m;
}
static inline void osalThreadDequeueNextI(threads_queue_t *q, msg_t m) {
  (void)q;
  (void)m;
}
/* no other thread frees a buffer: a write to a full queue returns what fit (TIME_IMMEDIATE) */
static inline msg_t osalThreadEnqueueTimeoutS(threads_queue_t *q, sysinterval_t t) {
  (void)q;
  (void)t;
  return MSG_TIMEOUT;
}
#include "hal_buffers.h"
