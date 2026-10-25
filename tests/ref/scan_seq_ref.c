/*
 * The key scan's sequence as button_read.c had it at 782a959, before the ScanSequencer and
 * ScanConsumer (src/drivers/scan_sequencer.hpp): the reference that tests/scan_seq compares them
 * with, call for call. The output channel tables, the interrupt's state and callback
 * (scan_isr_publish, scan_isr_start, adccallback) and the scanner thread's work per column are
 * copied unchanged; around them only what the test needs: the HAL (tests/scan_seq/stubs), the
 * KeyScanner and message hooks, one call per ADC callback (ref_adc_callback) and per column taken
 * (ref_thread_column, the thread loop's body after scan_link_wait).
 */
#include <stdbool.h>
#include <stdint.h>

#include "ch.h"
#include "hal.h"
#include "scan_ref.h"

#define DETECT_STUCK_NOTES
#define USE_AUX_BUTTONS
#define AUX_BUTTON_DEBOUNCE_TIME 5
#define KEY_DETECT 64
#define MIN_MEASURES 4
#define MULTISAMPLE 4
#define OUT_NUM_CHANNELS 51
#define ADC_GRP1_NUM_CHANNELS 4
#define ADC_GRP1_BUF_DEPTH 1
#define TRUE 1
#define scan_stats ref_scan_stats
#define ks_lost ref_ks_lost
#define ks_boot_column ref_ks_boot_column
#define ks_column ref_ks_column
#define event_send1 ref_event_send1

static const ioportid_t out_channels_port[51] = {
  GPIOC, GPIOC, GPIOC, GPIOG, GPIOG, GPIOG, GPIOG, GPIOG,
  GPIOG, GPIOG, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD,
  GPIOD, GPIOB, GPIOB, GPIOB, GPIOH, GPIOH, GPIOH, GPIOH,
  GPIOH, GPIOH, GPIOH, GPIOB, GPIOB, GPIOE, GPIOE, GPIOE, GPIOE,
  GPIOE, GPIOE, GPIOE, GPIOE, GPIOE, GPIOG, GPIOG, GPIOF,
  GPIOF, GPIOF, GPIOF, GPIOF, GPIOB, GPIOB, GPIOC, GPIOC, GPIOA,
};
static const iopadid_t out_channels_pad[51] = {
   8,  7,  6,  8,  7,  6,  5,  4,
   3,  2, 15, 14, 13, 12, 11, 10,  9,
   8, 15, 14, 13, 12, 11, 10,  9,
   8,  7,  6, 11, 10, 15, 14, 13, 12,
  11, 10,  9,  8,  7,  1,  0, 15,
  14, 13, 12, 11,  1,  0,  5,  4,  7,
};
static const ioportmask_t out_channels_portmask[51] = {
  1<< 8, 1<< 7, 1<< 6, 1<< 8, 1<< 7, 1<< 6, 1<< 5, 1<< 4,
  1<< 3, 1<< 2, 1<<15, 1<<14, 1<<13, 1<<12, 1<<11, 1<<10, 1<< 9,
  1<< 8, 1<<15, 1<<14, 1<<13, 1<<12, 1<<11, 1<<10, 1<< 9,
  1<< 8, 1<< 7, 1<< 6, 1<<11, 1<<10, 1<<15, 1<<14, 1<<13, 1<<12,
  1<<11, 1<<10, 1<< 9, 1<< 8, 1<< 7, 1<< 1, 1<< 0, 1<<15,
  1<<14, 1<<13, 1<<12, 1<<11, 1<< 1, 1<< 0, 1<< 5, 1<< 4, 1<< 7,
};

static int cur_channel = 0;
static int cur_conversion = 0;
static int cur_phase = 0;
static volatile int next_note_id = 0;

static ScanFrame *scan_cur;

#ifdef USE_AUX_BUTTONS
// #define LINE_BUTTON_PORT   PAL_LINE(GPIOI,  2U)
// #define LINE_BUTTON_UP     PAL_LINE(GPIOI,  1U)
// #define LINE_BUTTON_DOWN   PAL_LINE(GPIOA,  9U) // GPIOA_UART1_TX
// #define LINE_BUTTON_ALT    PAL_LINE(GPIOA, 10U) // GPIOA_UART1_RX

static const ioline_t aux_buttons_line[4] = {LINE_BUTTON_PORT, LINE_BUTTON_UP, LINE_BUTTON_DOWN, LINE_BUTTON_ALT};
static const bool aux_buttons_on[4] = {false, true, false, false};
static const int aux_buttons_msg[4] = {IDC_PORTAMENTO, IDC_OCT_UP, IDC_OCT_DOWN, IDC_ALT};
static uint32_t aux_buttons_state[4] = {0};
#endif

/*
 * ADC samples buffer.
 */
/* Buffers are allocated with size and address aligned to the cache
   line size.*/
#if CACHE_LINE_SIZE > 0
#define CC_CACHE_ALIGN CC_ALIGN(CACHE_LINE_SIZE)
#else
#define CC_CACHE_ALIGN
#endif
//CC_SECTION(".ram3")
CC_CACHE_ALIGN static adcsample_t adc_samples[CACHE_SIZE_ALIGN(adcsample_t, ADC_GRP1_NUM_CHANNELS * ADC_GRP1_BUF_DEPTH)];
// static adcsample_t adc_samples[ADC_GRP1_NUM_CHANNELS * ADC_GRP1_BUF_DEPTH];

static int measure[20] = {0};
static int *measure_put = measure;
static int *measure_get = measure;

static thread_t *tpReadButtons = NULL;

// the scheduler for the hand-over (key_scanner_link.h)
#define SCAN_OS_LOCK() chSysLock()
#define SCAN_OS_UNLOCK() chSysUnlock()
#define SCAN_OS_SLEEP_S()                  \
  do {                                     \
    tpReadButtons = chThdGetSelfX();       \
    chSchGoSleepS(CH_STATE_SUSPENDED);     \
  } while (0)
#define SCAN_OS_WINDOW() do {} while (0)
#include "key_scanner_link.h"
static scan_link_t link;

// the interrupt published columns: wake the thread
static void scan_isr_publish(uint32_t ready) {
  scan_link_publish(&link, ready);
  chSysLockFromISR();
  if (tpReadButtons != NULL) {
    chSchReadyI(tpReadButtons);
    tpReadButtons = NULL;
  }
  chSysUnlockFromISR();
}

/*
 * Diagnostics (key_scanner.h, console StcE). The interrupt numbers and times the scans; the
 * thread counts what it couldn't take (key_scanner_link.h).
 */
volatile scan_stats_t scan_stats;
static uint32_t isr_scan_t0;
static uint32_t isr_callbacks;
static uint32_t isr_seq;

static void scan_isr_start(void) {
  const uint32_t now = DWT->CYCCNT;
  if (scan_stats.scans) {
    const uint32_t dt = now - isr_scan_t0;
    if (dt < scan_stats.cycles_min || scan_stats.cycles_n == 0) scan_stats.cycles_min = dt;
    if (dt > scan_stats.cycles_max) scan_stats.cycles_max = dt;
    scan_stats.cycles_sum += dt;
    scan_stats.cycles_n++;
    int bin = dt < SCAN_HIST_BASE ? 0 : (int)((dt - SCAN_HIST_BASE) / SCAN_HIST_STEP);
    scan_stats.cycles_hist[bin < SCAN_HIST_BINS ? bin : SCAN_HIST_BINS - 1]++;
    int cb = (int)isr_callbacks - SCAN_CB_BASE;
    scan_stats.callbacks_hist[cb < 0 ? 0 : cb < SCAN_CB_BINS ? cb : SCAN_CB_BINS - 1]++;
  }
  isr_scan_t0 = now;
  isr_callbacks = 0;
  isr_seq = scan_stats.scans;
  scan_stats.scans++;
  scan_cur = scan_link_begin(&link, isr_seq);
  scan_cur->seq = isr_seq;
  scan_cur->t_start = now;
}

static void adccallback(ADCDriver *adcp) {
  // invalidate buffer after DMA transfer
  cacheBufferInvalidate(adc_samples, sizeof (adc_samples));  // bytes (S12: was the element count)

  isr_callbacks++;
  if (cur_conversion < 17) { // key press detection phase
    if (cur_conversion == 0) scan_isr_start();
    int next_conversion = cur_conversion + 1;
    /* Open old channels */
    palSetPort(out_channels_port[cur_channel+0], out_channels_portmask[cur_channel+0]);
    palSetPort(out_channels_port[cur_channel+1], out_channels_portmask[cur_channel+1]);
    palSetPort(out_channels_port[cur_channel+2], out_channels_portmask[cur_channel+2]);
    if (next_conversion == 17) {
      // switch to measure phase
      for (int n=0; n<OUT_NUM_CHANNELS; n++) {
        palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_OPENDRAIN | PAL_STM32_OSPEED_HIGHEST);
      }
      measure_get = measure;
      next_note_id = *measure_get;
      cur_channel = next_note_id * 3;
      // the measured columns (one, stale, when no key was detected)
      int nm = measure_put - measure;
      scan_cur->n = nm ? nm : 1;
      for (int n = 0; n < scan_cur->n; n++) scan_cur->cols[n] = measure[n];
      // the columns before the first measured one are complete (no wakeup: as before)
      scan_link_publish(&link, next_note_id);
    } else {
      cur_channel = next_conversion * 3;
    }
    /* Drain new channels */
    palClearPort(out_channels_port[cur_channel+0], out_channels_portmask[cur_channel+0]);
    palClearPort(out_channels_port[cur_channel+1], out_channels_portmask[cur_channel+1]);
    palClearPort(out_channels_port[cur_channel+2], out_channels_portmask[cur_channel+2]);

    // store values
    scan_cur->on[cur_conversion   ] = 4095 - adc_samples[0];
    scan_cur->on[cur_conversion+17] = 4095 - adc_samples[1];
    scan_cur->on[cur_conversion+34] = 4095 - adc_samples[2];
    scan_cur->on[cur_conversion+51] = 4095 - adc_samples[3];
    if (adc_samples[0] < (4095-KEY_DETECT) ||
        adc_samples[1] < (4095-KEY_DETECT) ||
        adc_samples[2] < (4095-KEY_DETECT) ||
        adc_samples[3] < (4095-KEY_DETECT)) {
          *measure_put++ = cur_conversion;
    }

    cur_conversion = next_conversion;
  } else if (cur_conversion == 17) { // key measurement phase
    switch (cur_phase) {
    case 0: { // read whole button, 4x multisampled (MULTISAMPLE hardcoded)
      scan_cur->m[next_note_id   ].p = 4095 - adc_samples[0];
      scan_cur->m[next_note_id+17].p = 4095 - adc_samples[1];
      scan_cur->m[next_note_id+34].p = 4095 - adc_samples[2];
      scan_cur->m[next_note_id+51].p = 4095 - adc_samples[3];

      cur_phase++;
    } break;
    case 1:
    case 2: {
      scan_cur->m[next_note_id   ].p += 4095 - adc_samples[0];
      scan_cur->m[next_note_id+17].p += 4095 - adc_samples[1];
      scan_cur->m[next_note_id+34].p += 4095 - adc_samples[2];
      scan_cur->m[next_note_id+51].p += 4095 - adc_samples[3];

      cur_phase++;
    } break;
    case 3: {
      /* Open old channels */
      palSetPort(out_channels_port[cur_channel+1], out_channels_portmask[cur_channel+1]);
      palSetPort(out_channels_port[cur_channel+2], out_channels_portmask[cur_channel+2]);

      scan_cur->m[next_note_id   ].p = scan_cur->m[next_note_id   ].p + 4095 - adc_samples[0];
      scan_cur->m[next_note_id+17].p = scan_cur->m[next_note_id+17].p + 4095 - adc_samples[1];
      scan_cur->m[next_note_id+34].p = scan_cur->m[next_note_id+34].p + 4095 - adc_samples[2];
      scan_cur->m[next_note_id+51].p = scan_cur->m[next_note_id+51].p + 4095 - adc_samples[3];

      cur_phase = 100;
    } break;
    case 100: { // read s0
      /* Open old channels */
      palSetPort(out_channels_port[cur_channel+0], out_channels_portmask[cur_channel+0]);
      /* Drain new channels */
      palClearPort(out_channels_port[cur_channel+1], out_channels_portmask[cur_channel+1]);

      scan_cur->m[next_note_id   ].s0 = (4095 - adc_samples[0]) * MULTISAMPLE;
      scan_cur->m[next_note_id+17].s0 = (4095 - adc_samples[1]) * MULTISAMPLE;
      scan_cur->m[next_note_id+34].s0 = (4095 - adc_samples[2]) * MULTISAMPLE;
      scan_cur->m[next_note_id+51].s0 = (4095 - adc_samples[3]) * MULTISAMPLE;

      cur_phase = 101;
    } break;
    case 101: { // read s1
      /* Open old channels */
      palSetPort(out_channels_port[cur_channel+1], out_channels_portmask[cur_channel+1]);
      /* Drain new channels */
      palClearPort(out_channels_port[cur_channel+2], out_channels_portmask[cur_channel+2]);

      scan_cur->m[next_note_id   ].s1 = (4095 - adc_samples[0]) * MULTISAMPLE;
      scan_cur->m[next_note_id+17].s1 = (4095 - adc_samples[1]) * MULTISAMPLE;
      scan_cur->m[next_note_id+34].s1 = (4095 - adc_samples[2]) * MULTISAMPLE;
      scan_cur->m[next_note_id+51].s1 = (4095 - adc_samples[3]) * MULTISAMPLE;

      cur_phase = 102;
    } break;
    case 102: { // read s2
      /* Open old channels */
      palSetPort(out_channels_port[cur_channel+2], out_channels_portmask[cur_channel+2]);

      scan_cur->m[next_note_id   ].s2 = (4095 - adc_samples[0]) * MULTISAMPLE;
      scan_cur->m[next_note_id+17].s2 = (4095 - adc_samples[1]) * MULTISAMPLE;
      scan_cur->m[next_note_id+34].s2 = (4095 - adc_samples[2]) * MULTISAMPLE;
      scan_cur->m[next_note_id+51].s2 = (4095 - adc_samples[3]) * MULTISAMPLE;

      // Next channel
      measure_get++;
      if (measure_get < measure_put) {
        next_note_id = *measure_get;
        cur_channel = next_note_id * 3;
      } else {
        if (measure_put - measure < MIN_MEASURES) {
          // switch to delay phase
          cur_conversion = 20 + 4 * (measure_put - measure);
          next_note_id = 16; // process all but last notes to keep from hanging at note_id 0
        } else {
          // switch to detection phase
          cur_conversion = 0;
          next_note_id = 0;
        }
        measure_put = measure;
        cur_channel = 0;
        for (int n=0; n<OUT_NUM_CHANNELS; n++) {
          palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
        }
      }

      /* Drain new channels */
      palClearPort(out_channels_port[cur_channel+0], out_channels_portmask[cur_channel+0]);
      palClearPort(out_channels_port[cur_channel+1], out_channels_portmask[cur_channel+1]);
      palClearPort(out_channels_port[cur_channel+2], out_channels_portmask[cur_channel+2]);

      cur_phase = 0;

      // Wake up processing thread: the columns before next_note_id are complete (0: all of them)
      scan_isr_publish(next_note_id ? next_note_id : SCAN_COLUMNS);
    } break;
    }
  } else { // delay phase
    /* When less than MIN_MEASURES buttons are measured delay a bit so the
       total conversion time and velocity sensitivity stay constant.
       When more than MIN_MEASURES buttons are measured it slows down, increasing
       the velocity sensitivity */
    cur_conversion++;
    if (cur_conversion >= 20 + (MULTISAMPLE + 3) * MIN_MEASURES) {
      // switch to detection phase
      cur_conversion = 0;
      next_note_id = 0;

      // Wake up processing thread: the scan is complete
      scan_isr_publish(SCAN_COLUMNS);
    }
  }

  // start next ADC conversion
#if defined(STM32F4XX)
  adcp->adc->CR2 |= ADC_CR2_SWSTART;
#elif defined(STM32H7XX)
  adcp->adcm->CR |= ADC_CR_ADSTART;
#endif

}

void ref_adc_callback(const adcsample_t s[4]) {
  for (int i = 0; i < 4; i++) adc_samples[i] = s[i];
  adccallback(&ADCD1);
}

// the interrupt's state as at boot, then ButtonReadStart's first scan
void ref_isr_first_scan(void) {
  cur_channel = cur_conversion = cur_phase = next_note_id = 0;
  for (int i = 0; i < 20; i++) measure[i] = 0;
  measure_put = measure_get = measure;
  isr_scan_t0 = isr_callbacks = isr_seq = 0;
  tpReadButtons = NULL;
  scan_link_t zero = {0};
  link = zero;
  scan_cur = scan_link_begin(&link, 0);
}
const ScanFrame *ref_frame(int i) { return &link.frame[i]; }
uint32_t ref_link_seq(void) { return link.seq; }
uint32_t ref_link_ready(void) { return link.ready; }
void ref_set_waiting(void) { tpReadButtons = chThdGetSelfX(); }
bool ref_waiting(void) { return tpReadButtons != NULL; }

static int count = 0;
void ref_thread_column(int note_id) {
  do {
    if (note_id < 0) {  // the interrupt overwrote it first: on with the scan it is writing
      ks_lost();
      continue;
    }
#ifdef DETECT_STUCK_NOTES
    if (count < 100) {
      // the zero level pass: keys touched at boot get a zero_offset of their pressure
      ks_boot_column(note_id);
      // Once per cycle, after the last buttons
      if (note_id == 16) {
        count++;
        scan_stats.frames++;
      }
      continue;
    }
#endif // DETECT_STUCK_NOTES
    (void)count;

    // the keys of this column, then the cross talk correction they give the other columns
    ks_column(note_id);

    // Once per cycle, after the last buttons
    if (note_id == 16) {
      scan_stats.frames++;
#ifdef USE_AUX_BUTTONS
      for (int n = 0; n < 4; n++) {
        if (aux_buttons_state[n] & 0xff) {
          aux_buttons_state[n]--;
        } else if (palReadLine(aux_buttons_line[n]) == !aux_buttons_state[n]) {
          int on;
          if (aux_buttons_state[n]) {
            on = !aux_buttons_on[n];
            aux_buttons_state[n] = AUX_BUTTON_DEBOUNCE_TIME;
          } else {
            on = aux_buttons_on[n];
            aux_buttons_state[n] = AUX_BUTTON_DEBOUNCE_TIME | 0x100;
          }
          event_send1(ID_CONTROL, aux_buttons_msg[n], on);
        }
      }
#endif // USE_AUX_BUTTONS
    }
  } while (0);
}
