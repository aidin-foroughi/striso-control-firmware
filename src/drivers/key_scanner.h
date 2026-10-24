/**
 * Key scanner diagnostics (C, shared by the ADC interrupt, the scanner thread and the console).
 *
 * Counters since boot, read by the console command StcE:
 *   scans        scans the ADC interrupt started (the scan sequence number)
 *   frames       scans the scanner thread finished processing (all 17 columns)
 *   seq_gaps     scans the thread did not get to process as a scan of their own: it fell a whole
 *                scan or more behind, and the data of several scans got merged
 *   overruns     scans in which the interrupt overwrote a column the thread had not processed yet
 *                (the thread then reads a mix of two scans)
 *   lost_wakeups the interrupt published new columns between the thread's last check and its
 *                sleep: the thread slept with work waiting, until the next wakeup
 *   queue_full   key messages the message queue refused (msgSend failed)
 *   off_waits    of those, retries of a note-off: each one stalls the scanner thread for a tick
 *   cycles       DWT cycles from one scan's start to the next: min, max, sum and a histogram
 *   callbacks    ADC callbacks per scan: a histogram (17 detection + 7 per measured column +
 *                the delay phase)
 */
#ifndef KEY_SCANNER_H_
#define KEY_SCANNER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SCAN_COLUMNS = 17,
  SCAN_ROWS = 4,
  SCAN_BUTTONS = SCAN_COLUMNS * SCAN_ROWS,
  // cycles per scan histogram: bin i counts scans of SCAN_HIST_BASE + i * SCAN_HIST_STEP cycles
  // and up (the first and last bins also take what is below and above)
  SCAN_HIST_BINS = 64,
  SCAN_HIST_BASE = 160000,  // 0.4 ms at 400 MHz
  SCAN_HIST_STEP = 4096,    // 10.24 us
  // callbacks per scan histogram: bin i counts scans of SCAN_CB_BASE + i callbacks
  SCAN_CB_BINS = 48,
  SCAN_CB_BASE = 24,
};

typedef struct {
  uint32_t scans;
  uint32_t frames;
  uint32_t seq_gaps;
  uint32_t overruns;
  uint32_t lost_wakeups;
  uint32_t queue_full;
  uint32_t off_waits;
  uint32_t cycles_min;
  uint32_t cycles_max;
  uint64_t cycles_sum;
  uint32_t cycles_n;
  uint32_t cycles_hist[SCAN_HIST_BINS];
  uint32_t callbacks_hist[SCAN_CB_BINS];
} scan_stats_t;

extern volatile scan_stats_t scan_stats;

#ifdef __cplusplus
}
#endif

#endif
