/**
 * The key scanner's interface in C: the ADC interrupt (button_read.c) fills a ScanFrame per scan,
 * the scanner thread hands it to the KeyScanner (key_scanner.hpp, C++) column by column through
 * the ks_* functions (key_scanner.cpp), and the console reads the diagnostics.
 *
 * Counters since boot (scan_stats), read by the console command StcE:
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
	SCAN_BUTTONS = SCAN_COLUMNS * SCAN_ROWS, // button = column + 17 * row
	// cycles per scan histogram: bin i counts scans of SCAN_HIST_BASE + i * SCAN_HIST_STEP cycles
	// and up (the first and last bins also take what is below and above)
	SCAN_HIST_BINS = 64,
	SCAN_HIST_BASE = 400000, // 1 ms at 400 MHz (an idle scan is about 1.6 ms)
	SCAN_HIST_STEP = 8192,   // 20.48 us
	// callbacks per scan histogram: bin i counts scans of SCAN_CB_BASE + i callbacks
	SCAN_CB_BINS = 48,
	SCAN_CB_BASE = 24,
};

// one measured key: pressure (sum of 4 samples) and the three corner samples (x 4), 4095 - adc
typedef struct {
	uint16_t p;
	uint16_t s0;
	uint16_t s1;
	uint16_t s2;
} scan_meas_t;

// The samples of one scan, as the ADC interrupt takes them.
typedef struct {
	uint32_t seq;                // scan number
	uint32_t t_start;            // DWT cycle counter at the scan's first conversion
	uint8_t n;                   // measured columns (the interrupt measures at least one)
	uint8_t cols[SCAN_COLUMNS];  // the measured columns, in order
	uint16_t on[SCAN_BUTTONS];   // key detection sample per button, 4095 - adc
	scan_meas_t m[SCAN_BUTTONS]; // the measured columns' buttons; the others keep old values
} ScanFrame;

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

// a button's calibration, set by button_read.c (boot, StcC, the live key factors); the scanner
// thread takes it over at the start of the next scan after ks_calibration_changed()
typedef struct {
	int32_t c_force;
	int32_t c_breakpoint;
	int32_t c_force2;
} scan_cal_t;

extern scan_cal_t ks_cal[SCAN_BUTTONS];

// --- key_scanner.cpp ------------------------------------------------------------------------
void ks_init(void);                                  // the KeyScanner's initial state; ks_cal from it
void ks_calibration_changed(void);                   // after writing ks_cal
void ks_set_zero_offset(int b, int32_t zero_offset); // before the scanner thread starts
// the scanner thread: the zero level pass of the first scans, then the normal processing. The
// thread passes the interrupt's frame; column col of it must be complete.
void ks_boot_column(const ScanFrame* f, int col);
void ks_column(const ScanFrame* f, int col);
// a button's state for the console (StcZ)
void ks_key_state(int b, int* status, int32_t* c_force, int32_t* zero_offset, int32_t* pres);

// Raw recordings (StcR, key_scanner_capture.hpp). ks_capture(seconds > 0) records the next
// scans for that long (or until the buffer is full), ks_capture(0) sends the last recording
// over the bulk console (the StcS stream must be off). Both print their reply.
void ks_capture(int seconds);
void ks_capture_start_dump_thread(void);
// one line for StcE: the recording's state
void ks_capture_print(void* stream);

#ifdef __cplusplus
}
#endif

#endif
