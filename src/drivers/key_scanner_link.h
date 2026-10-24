/**
 * The hand-over of the scans from the ADC interrupt to the scanner thread (button_read.c), in C,
 * without the hardware: the PC tests (tests/test_scan_link.cpp) run it against a simulated
 * interrupt and scheduler.
 *
 * The interrupt numbers the scans; it starts scan seq with scan_link_begin (which gives it the
 * frame to write) and publishes how many leading columns of it are complete with
 * scan_link_publish (and then wakes the thread). The thread takes the columns one by one, as they
 * are complete, into its own copy (scan_link_take), and sleeps when there is none
 * (scan_link_wait), so a key's messages leave right after its column is measured.
 *
 * The includer defines the scheduler hooks: SCAN_OS_LOCK() / SCAN_OS_UNLOCK() (interrupts off and
 * on), SCAN_OS_SLEEP_S() (sleep until the interrupt's next wakeup; called locked, returns locked)
 * and SCAN_OS_WINDOW() (a point where an interrupt may run: nothing on the board, the tests run
 * the simulated interrupt there).
 */
#ifndef KEY_SCANNER_LINK_H_
#define KEY_SCANNER_LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "key_scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

// the compiler must not move the frame's loads and stores across the scan number's
#define SCAN_BARRIER() __asm__ volatile("" ::: "memory")

typedef struct {
  ScanFrame frame[2];
  volatile uint32_t seq;    // the scan the interrupt is writing
  volatile uint32_t ready;  // its complete leading columns, 0..SCAN_COLUMNS
} scan_link_t;

// the consumer's place: the next column col of scan k
typedef struct {
  uint32_t k;
  int col;
  uint32_t lost;     // times the interrupt overwrote a column before the thread took it
  uint32_t skipped;  // scans the thread didn't finish because of it
} scan_pos_t;

// --- the interrupt --------------------------------------------------------------------------
static inline ScanFrame *scan_link_begin(scan_link_t *l, uint32_t seq) {
  l->ready = 0;
  l->seq = seq;
  SCAN_BARRIER();
  return &l->frame[0];
}

static inline void scan_link_publish(scan_link_t *l, uint32_t ready) { l->ready = ready; }

// --- the thread -------------------------------------------------------------------------------
// the columns of scan k that are complete (SCAN_COLUMNS once the interrupt moved on)
static inline int scan_link_avail(const scan_link_t *l, uint32_t k) {
  const uint32_t s = l->seq;
  if (s == k) return (int)l->ready;
  return SCAN_COLUMNS;
}

static inline bool scan_link_intact(const scan_link_t *l, uint32_t k) {
  (void)l;
  (void)k;
  return true;
}

// Takes the next column into work (and at column 0 the scan's number, time and measured
// columns). Returns the column, -1 if it isn't complete yet, or -2 if the interrupt overwrote it
// first: then the place moves to the scan being written (pos->lost, pos->skipped).
static inline int scan_link_take(scan_link_t *l, scan_pos_t *pos, ScanFrame *work) {
  const int avail = scan_link_avail(l, pos->k);
  if (avail >= 0 && avail <= pos->col) return -1;
  if (avail >= 0) {
    const ScanFrame *f = &l->frame[0];
    const int col = pos->col;
    if (col == 0) {
      work->seq = f->seq;
      work->t_start = f->t_start;
      work->n = f->n;
      for (int i = 0; i < SCAN_COLUMNS; i++) work->cols[i] = f->cols[i];
    }
    for (int n = 0; n < SCAN_ROWS; n++) {
      const int b = col + n * SCAN_COLUMNS;
      work->on[b] = f->on[b];
      work->m[b] = f->m[b];
    }
    SCAN_BARRIER();
    if (scan_link_intact(l, pos->k)) {
      if (++pos->col == SCAN_COLUMNS) {
        pos->col = 0;
        pos->k++;
      }
      return col;
    }
  }
  const uint32_t s = l->seq;
  pos->lost++;
  pos->skipped += s - pos->k;
  pos->k = s;
  pos->col = 0;
  return -2;
}

// The next column, sleeping until the interrupt has one (or -2, see scan_link_take).
static inline int scan_link_wait(scan_link_t *l, scan_pos_t *pos, ScanFrame *work, uint32_t *caught) {
  (void)caught;
  for (;;) {
    const int c = scan_link_take(l, pos, work);
    if (c != -1) return c;
    SCAN_OS_WINDOW();
    SCAN_OS_LOCK();
    SCAN_OS_SLEEP_S();
    SCAN_OS_UNLOCK();
  }
}

#ifdef __cplusplus
}
#endif

#endif
