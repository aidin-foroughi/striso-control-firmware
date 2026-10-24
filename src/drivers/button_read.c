/**
 * Copyright (C) 2019 Piers Titus van der Torren
 *
 * This file is part of Striso Control.
 *
 * Striso Control is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Striso Control is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Striso Control. If not, see <http://www.gnu.org/licenses/>.
 */
#include "button_read.h"
#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#include "ccportab.h"

#include "config.h"
#include "striso.h"
#include "synth.h"
#include "usbcfg.h"
#include "led.h"

#include "messaging.h"
#include "key_scanner.h"
#ifdef STM32F4XX
#include "adc_multi.h"
#endif

#define INTERNAL_ONE (1<<24)
#define ADC_BITS 12
#define ADC_MAX ((1<<ADC_BITS)-1)
#define ADCFACT (INTERNAL_ONE >> ADC_BITS)  // factor from adc sample to INTERNAL_ONE
#define VELOFACT 32      // extra factor for velocity
#define MSGFACT (1<<11)  // factor from 14 bit message to INTERNAL_ONE
#define MSGFACT_VELO (MSGFACT/VELOFACT)
#define FILT 8  // min: 1 (no filter), max: 64 (1<<32 / INTERNAL_ONE)
#define FILTV 8 // min: 1 (no filter), max: 64 (1<<32 / INTERNAL_ONE)
#define ZERO_LEVEL_FACT 300 / 256  // safety factor for zero level. Without brackets so multiplication goes before division
#define ZERO_LEVEL_TIME 500
#define ZERO_LEVEL_MAX_PRES (INTERNAL_ONE/32)
#define ZERO_LEVEL_MAX_VELO 500
#define COMMON_CHANNEL_FILT 0.5
#define KEY_DETECT 64   // key_detect threshold
#define KEY_DETECT2 (256-KEY_DETECT)  // additional threshold when another key in the column is pressed
#define KEY_DETECT3 (320-KEY_DETECT2-KEY_DETECT) // additional threshold when 3 or 4 corners are pressed
#define MIN_MEASURES 4 // minimum notes to measure, must be >= 2
#define MULTISAMPLE 4  // multisampling of pressure, also hardcoded in some places

#define INTEGRATED_PRES_TRESHOLD (INTERNAL_ONE/8)
#define SENDFACT    config.message_interval

#ifdef STM32F4XX
//#define ADC_SAMPLE_DEF ADC_SAMPLE_3   // 0.05 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_15  // 0.11 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_28  // 0.18 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_56  // 0.33 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_84  // 0.50 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_112 // 0.65 ms per cycle
#define ADC_SAMPLE_DEF ADC_SAMPLE_144 // 0.83 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_480 // 2.7 ms per cycle

/* Number of ADCs used in multi ADC mode (2 or 3) */
#define ADC_N_ADCS 3

/* Total number of channels to be sampled by a single ADC operation.*/
#define ADC_GRP1_NUM_CHANNELS_PER_ADC   2

/* Depth of the conversion buffer, channels are sampled one time each.*/
#define ADC_GRP1_BUF_DEPTH      1 // must be 1 or even

#define ADC_GRP1_NUM_CHANNELS (ADC_GRP1_NUM_CHANNELS_PER_ADC * ADC_N_ADCS)

#elif defined(STM32H7XX)
// timing calculation: ADCCLK/(SMP+6.5)/51/4 (*2 for dual ADC)
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_1P5   //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_2P5   //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_8P5   //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_16P5  //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_32P5 // 1174 Hz @10MHz single ADC measured (1257 Hz calculated)
#define ADC_SAMPLE_DEF ADC_SMPR_SMP_64P5  // 1289 Hz @10MHz dual ADC measured (1380 Hz calculated)
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_384P5 // 123.6 Hz @10MHz single ADC measured (125.4 Hz calculated) 245 Hz dual ADC
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_810P5 //

/* Total number of channels to be sampled by a single ADC operation.*/
#define ADC_GRP1_NUM_CHANNELS   4

/* Depth of the conversion buffer, channels are sampled one time each.*/
#define ADC_GRP1_BUF_DEPTH      1 // must be 1 or even. Strange behaviour when it is 2.

#endif

#define ADC_OFFSET (16>>1)

#define OUT_NUM_CHANNELS        51
#define N_BUTTONS               68
#define N_BUTTONS_BAS           51

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

#ifdef USE_BAS
static const ioportid_t out_channels_bas_port[51] = {
  GPIOA, GPIOC, GPIOC, GPIOC, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD,
  GPIOD, GPIOD, GPIOD, GPIOG, GPIOG, GPIOG, GPIOG, GPIOG,
  GPIOG, GPIOG, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB,
  GPIOE, GPIOE, GPIOI, GPIOI, GPIOI, GPIOI, GPIOE, GPIOE,
  GPIOE, GPIOE, GPIOE, GPIOI, GPIOI, GPIOI, GPIOF, GPIOF, GPIOF,
  GPIOF, GPIOF, GPIOF, GPIOF, GPIOF, GPIOF, GPIOF, GPIOF,
};
static const iopadid_t out_channels_bas_pad[51] = {
  15, 10, 11, 12,  0,  1,  2,  3,  4,
   5,  6,  7,  9, 10, 11, 12, 13,
  14, 15,  3,  4,  5,  6,  7,  8,  9,
   0,  1,  4,  5,  6,  7,  2,  3,
   4,  5,  6,  9, 10, 11,  0,  1,  2,
   3,  4,  5,  6,  7,  8,  9, 10,
};
static const ioportmask_t out_channels_bas_portmask[51] = {
  1<<15, 1<<10, 1<<11, 1<<12, 1<< 0, 1<< 1, 1<< 2, 1<< 3, 1<< 4,
  1<< 5, 1<< 6, 1<< 7, 1<< 9, 1<<10, 1<<11, 1<<12, 1<<13,
  1<<14, 1<<15, 1<< 3, 1<< 4, 1<< 5, 1<< 6, 1<< 7, 1<< 8, 1<< 9,
  1<< 0, 1<< 1, 1<< 4, 1<< 5, 1<< 6, 1<< 7, 1<< 2, 1<< 3,
  1<< 4, 1<< 5, 1<< 6, 1<< 9, 1<<10, 1<<11, 1<< 0, 1<< 1, 1<< 2,
  1<< 3, 1<< 4, 1<< 5, 1<< 6, 1<< 7, 1<< 8, 1<< 9, 1<<10,
};
#endif // USE_BAS

static int cur_channel = 0;
static int cur_conversion = 0;
static int cur_phase = 0;
static volatile int next_note_id = 0;

enum button_status {
  OFF = 0,
  STARTING = 1,
  ON = 2,
};

typedef struct struct_button button_t;
struct struct_button {
  int32_t on;
  int32_t p;
  int32_t s0;
  int32_t s1;
  int32_t s2;
  int32_t pres;
  int32_t velo;
  int32_t c_force;
  int32_t c_breakpoint;
  int32_t c_force2;
  int32_t zero_offset;
  int32_t zero_time;
  int32_t zero_max;
  int32_t key_detect3;
  float fact;
  enum button_status status;
  int timer;
  int but_id;
  int src_id;
};

#ifdef USE_BAS
typedef struct struct_slider {
  int32_t s[27];
  int32_t v[27];
  int timer;
  int dbtimer;
  int pres[4];
  int pos[4];
  int velo[4];
  int sort[4];
  int n_press;
  int move;
  int zoom;
} slider_t;

static slider_t sld;
#endif

#ifdef USE_BAS
static button_t buttons_bas[N_BUTTONS_BAS];
#endif
// the samples of the scan the interrupt is taking (key_scanner.h), handed to the scanner thread
// through link (key_scanner_link.h); the key processing, which update_button and the cross talk
// correction did here, is the KeyScanner's (key_scanner.hpp)
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

#ifdef STM32F4XX
/*
 * ADC conversion group for ADC0 as multi ADC mode master.
 * Mode:        Circular buffer, triple ADC mode master, SW triggered.
 * Channels:    PA0, PA3
 */
static const ADCConversionGroup adcgrpcfg1 = {
  TRUE, // Circular conversion
  ADC_GRP1_NUM_CHANNELS_PER_ADC * ADC_N_ADCS,
  adccallback, /* end of conversion callback */
  NULL, /* error callback */
  /* HW dependent part.*/
  0, // CR1
  ADC_CR2_SWSTART, // CR2
  0, // SMPR1
  ADC_SMPR2_SMP_AN0(ADC_SAMPLE_DEF)
   | ADC_SMPR2_SMP_AN3(ADC_SAMPLE_DEF), // SMPR2
  0, // HTR
  0, // LTR
  ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS_PER_ADC), // SQR1
  0, // SQR2
  ADC_SQR3_SQ1_N(ADC_CHANNEL_IN0)
   | ADC_SQR3_SQ2_N(ADC_CHANNEL_IN3) // SQR3
};

/*
 * ADC conversion group for ADC2.
 * Mode:        triple ADC mode slave.
 * Channels:    PA1, PC0
 */
static const ADCConversionGroup adcgrpcfg2 = {
  TRUE,
  0,
  NULL, /* end of conversion callback */
  NULL, /* error callback */
  /* HW dependent part.*/
  0, // CR1
  0, // CR2
  ADC_SMPR1_SMP_AN12(ADC_SAMPLE_DEF), // SMPR1
  ADC_SMPR2_SMP_AN1(ADC_SAMPLE_DEF), // SMPR2
  0, // HTR
  0, // LTR
  ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS_PER_ADC), // SQR1
  0, // SQR2
  ADC_SQR3_SQ1_N(ADC_CHANNEL_IN1)
   | ADC_SQR3_SQ2_N(ADC_CHANNEL_IN12) // SQR3
};

/*
 * ADC conversion group for ADC3.
 * Mode:        triple ADC mode slave.
 * Channels:    PA2, PC2
 */
static const ADCConversionGroup adcgrpcfg3 = {
  TRUE,
  0,
  NULL, /* end of conversion callback */
  NULL, /* error callback */
  /* HW dependent part.*/
  0, // CR1
  0, // CR2
  ADC_SMPR1_SMP_AN10(ADC_SAMPLE_DEF), // SMPR1
  ADC_SMPR2_SMP_AN2(ADC_SAMPLE_DEF), // SMPR2
  0, // HTR
  0, // LTR
  ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS_PER_ADC), // SQR1
  0, // SQR2
  ADC_SQR3_SQ1_N(ADC_CHANNEL_IN2)
   | ADC_SQR3_SQ2_N(ADC_CHANNEL_IN10) // SQR3
};
#endif

#ifdef STM32H7XX
const ADCConfig adccfg1 = {
  .difsel       = 0U,
  .calibration  = 0U
};
#if STM32_ADC_DUAL_MODE == TRUE
/*
 * ADC conversion group 1.
 * Mode:        One shot, 2 channels, SW triggered.
 * Channels:    IN0, IN5.
 */
const ADCConversionGroup adcgrpcfg1 = {
  .circular     = TRUE,
  .num_channels = ADC_GRP1_NUM_CHANNELS,
  .end_cb       = adccallback,
  .error_cb     = NULL,
  .cfgr         = ADC_CFGR_RES_12BITS,
  .cfgr2        = 0U,
  .ccr          = ADC_CCR_DUAL_REG_SIMULT, // 6U
  .pcsel        = ADC_SELMASK_IN3 | ADC_SELMASK_IN19 | ADC_SELMASK_IN18 | ADC_SELMASK_IN15,
  .ltr1         = 0x00000000U,
  .htr1         = 0x03FFFFFFU,
  .ltr2         = 0x00000000U,
  .htr2         = 0x03FFFFFFU,
  .ltr3         = 0x00000000U,
  .htr3         = 0x03FFFFFFU,
  .smpr         = {
    0U,
    ADC_SMPR2_SMP_AN15(ADC_SAMPLE_DEF) | ADC_SMPR2_SMP_AN19(ADC_SAMPLE_DEF)
  },
  .sqr          = {
    ADC_SQR1_SQ1_N(ADC_CHANNEL_IN15) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN19),
    0U,
    0U,
    0U
  },
  .ssmpr        = {
    ADC_SMPR1_SMP_AN3(ADC_SAMPLE_DEF),
    ADC_SMPR2_SMP_AN18(ADC_SAMPLE_DEF)
  },
  .ssqr         = {
    ADC_SQR1_SQ1_N(ADC_CHANNEL_IN18) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN3),
    0U,
    0U,
    0U
  }
};
#else // STM32_ADC_DUAL_MODE == FALSE
const ADCConversionGroup adcgrpcfg1 = {
  .circular     = TRUE,
  .num_channels = ADC_GRP1_NUM_CHANNELS,
  .end_cb       = adccallback,
  .error_cb     = NULL,
  .cfgr         = ADC_CFGR_RES_12BITS,
  .cfgr2        = 0U,
  .ccr          = 0U,
  .pcsel        = ADC_SELMASK_IN3 | ADC_SELMASK_IN19 | ADC_SELMASK_IN18 | ADC_SELMASK_IN15,
  .ltr1         = 0x00000000U,
  .htr1         = 0x03FFFFFFU,
  .ltr2         = 0x00000000U,
  .htr2         = 0x03FFFFFFU,
  .ltr3         = 0x00000000U,
  .htr3         = 0x03FFFFFFU,
  .smpr         = {
    ADC_SMPR1_SMP_AN3(ADC_SAMPLE_DEF),
    ADC_SMPR2_SMP_AN19(ADC_SAMPLE_DEF) |
    ADC_SMPR2_SMP_AN18(ADC_SAMPLE_DEF) |
    ADC_SMPR2_SMP_AN15(ADC_SAMPLE_DEF)
  },
  .sqr          = {
    ADC_SQR1_SQ1_N(ADC_CHANNEL_IN15) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN18) |
    ADC_SQR1_SQ3_N(ADC_CHANNEL_IN19) | ADC_SQR1_SQ4_N(ADC_CHANNEL_IN3),
    0U,
    0U,
    0U
  }
};
#endif // STM32_ADC_DUAL_MODE
#endif // STM32H7XX


#ifdef USE_BAS
/*
typedef struct struct_slider {
  int32_t s[27];
  int32_t v[27];
  int timer;
  int pres[4];
  int pos[4];
  int velo[4];
  int sort[4];
  int n_press;
  int move;
  int zoom;
} slider_t;
*/
#define SLD_MAX_DIST 2
#define N_PEAKS 8
#define N_PRESS 4
#define N_SENS 27
#define SLD_STEP (1<<8)
int slider_interp(int n) {
  int a = sld.s[n-1];
  int b = sld.s[n];
  int c = sld.s[n+1];
  if (a > c) {
    return n * SLD_STEP + (c-a) / ((b-c)/SLD_STEP) / 2;
  } else {
    return n * SLD_STEP + (c-a) / ((b-a)/SLD_STEP) / 2;
  }
}

void update_slider(void) {
  int n;
  int np = 0;
  int peaks[N_PEAKS];
  int msg[8];
  msg[0] = ID_CONTROL;

  // make slider less sensitive when buttons are pressed, because of crosstalk
  int min_pres = 0;
  for (n=18; n<(18+17); n+=2) {
    if (min_pres < buttons_bas[n].s0)
      min_pres = buttons_bas[n].s0;
    if (min_pres < buttons_bas[n].s1)
      min_pres = buttons_bas[n].s1;
    if (min_pres < buttons_bas[n].s2)
      min_pres = buttons_bas[n].s2;
  }

  // find peaks
  for (n=1; n<27-1; n++) {
    if (sld.s[n] > min_pres + (INTERNAL_ONE/64) && sld.s[n-1] <= sld.s[n] && sld.s[n] > sld.s[n+1]) {
      peaks[np++] = n;
    }
  }

  // debug output
  #ifdef DEBUG_SDU1
  if (SDU1.state == SDU_READY) {
    if (sld.dbtimer <= 0) {
      for (n=0; n<27; n++) {
        chprintf((BaseSequentialStream *)&SDU1, " %4d", sld.s[n]>>10);
      }
      chprintf((BaseSequentialStream *)&SDU1, "\r\n    ");
      for (n=1; n<27-1; n++) {
        chprintf((BaseSequentialStream *)&SDU1, " %4d", sld.s[n] > 0 && sld.s[n-1] <= sld.s[n] && sld.s[n] > sld.s[n+1]);
      }
      chprintf((BaseSequentialStream *)&SDU1, "     npeaks: %d\r\n",np);
      sld.dbtimer = 100;
    } else {
      sld.dbtimer--;
    }
  }
  #endif

  /*
  signals:
  slide
  zoom
  2up
  2down

   */
  if (np != sld.n_press) {
    msg[1] = IDC_SLD_NPRESS;
    msg[2] = np;
    msgSend(3, msg);
  }

  // single slide (volume)
  if (np == 1) {
    int pos = slider_interp(peaks[0]);
    if (sld.n_press == 1) {
      if (sld.timer <= 0) {
        msg[1] = IDC_SLD_SLIDE;
        msg[2] = -(pos - sld.pos[0]);
        msgSend(3, msg);
        sld.timer = 16 * SENDFACT;
        sld.pos[0] = pos;
      } else {
        sld.timer--;
      }
    } else {
      // new press
      sld.pos[0] = pos;
      sld.timer = 16 * SENDFACT;
    }
  }
  // slide/zoom (tuning)
  else if (np == 2) {
    int pos0 = slider_interp(peaks[0]);
    int pos1 = slider_interp(peaks[1]);
    if (sld.n_press == 2) {
      if (sld.timer <= 0) {
        msg[1] = IDC_SLD_SLIDEZOOM;
        msg[2] = ((pos0 + pos1) - (sld.pos[0] + sld.pos[1]))/2;
        msg[3] = (pos1 - pos0) - (sld.pos[1] - sld.pos[0]);
        msgSend(4, msg);
        sld.timer = 16 * SENDFACT;
        sld.pos[0] = pos0;
        sld.pos[1] = pos1;
      } else {
        sld.timer--;
      }
    } else {
      // new press
      sld.pos[0] = pos0;
      sld.pos[1] = pos1;
      sld.timer = 16 * SENDFACT;
    }
  }
  sld.n_press = np;
  // 1 up/down
  // 2 up/down/middle
  // n press


/*
  // match to presses
  for (n = 0; n<np; n++) {
    int dp = INTERNAL_ONE;
    for (k = 0; k<N_PRESS; k++) {
      if (sld.pres[k] > 0) {
        d = abs(sld.pos[k] - peaks[n]);
        if (d < SLD_MAX_DIST && d < dp && dists[k] == INTERNAL_ONE) {
          dp = d;
          pp[n] = k;
          //if (dists[k] < INTERNAL_ONE) {
          //  dists[k] = INTERNAL_ONE;
          //  for (m = 0; m<n; m++)
          //    if pp[m]
          //}
        }
      }
    }
    if (dp != INTERNAL_ONE) {
      dists[pp[n]] = dp;
    }
  }
  // add new presses
  for (n = 0; n<np; n++) {
    if (pp[n] == -1) {
      for (k = 0; k<N_PRESS; k++) {
        if (sld.pres[k] == 0) {
          pp[n] = k;
          dists[k] = 0;
          break;
        }
      }
    }
  }
  // delete ended presses
  for (k = 0; k<N_PRESS; k++) {
    if (sld.pres[k] > 0 && dists[k]== INTERNAL_ONE) {
      sld.pres[k] = 0;
    }
  }

  // update
  for (n = 0; n<N_PRESS; n++) {
    if (sld.pres[n] > 0) {
      update_peak(n);
      // remove doubles
    }
  }*/
}
#endif

/*
 * Read out buttons and create messages.
 */
static THD_WORKING_AREA(waThreadReadButtons, 256);
static void ThreadReadButtons(void *arg) {
  (void)arg;

  chRegSetThreadName("read_buttons");
  scan_pos_t pos = {0};
  pos.k = link.seq;
  ScanFrame *work = ks_work();
  uint32_t caught = 0;
#ifdef DETECT_STUCK_NOTES
  int count = 0;
#else
  const int count = 100;
#endif

  while (TRUE) {
    // the next complete column of the scan, sleeping until there is one
    const int note_id = scan_link_wait(&link, &pos, work, &caught);
    scan_stats.overruns = pos.lost;
    scan_stats.seq_gaps = pos.skipped;
    scan_stats.lost_wakeups = caught;
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
      int msg[8];
      for (int n = 0; n < 4; n++) {
        if (aux_buttons_state[n] & 0xff) {
          aux_buttons_state[n]--;
        } else if (palReadLine(aux_buttons_line[n]) == !aux_buttons_state[n]) {
          msg[0] = ID_CONTROL;
          msg[1] = aux_buttons_msg[n];
          if (aux_buttons_state[n]) {
            msg[2] = !aux_buttons_on[n];
            aux_buttons_state[n] = AUX_BUTTON_DEBOUNCE_TIME;
          } else {
            msg[2] = aux_buttons_on[n];
            aux_buttons_state[n] = AUX_BUTTON_DEBOUNCE_TIME | 0x100;
          }
          msgSend(3, msg);
        }
      }
#endif // USE_AUX_BUTTONS
    }
  }
}

void ButtonBoardTest(void) {
  // test for shorted pins
  for (int n=1; n<OUT_NUM_CHANNELS; n++) {
    /* Drain channel */
    palClearPad(out_channels_port[n-1], out_channels_pad[n-1]);
#ifdef USE_BAS
    palClearPad(out_channels_bas_port[n-1], out_channels_bas_pad[n-1]);
#endif

    palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_INPUT_PULLUP);
#ifdef USE_BAS
    palSetPadMode(out_channels_bas_port[n], out_channels_bas_pad[n], PAL_MODE_INPUT_PULLUP);
#endif

    chThdSleep(1);
    if (!palReadPad(out_channels_port[n], out_channels_pad[n])) {
      // pin n and n-1 shorted

    }

    palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_OPENDRAIN);
#ifdef USE_BAS
    palSetPadMode(out_channels_bas_port[n], out_channels_bas_pad[n], PAL_MODE_OUTPUT_OPENDRAIN);
#endif
    /* Open channel */
    palSetPad(out_channels_port[n-1], out_channels_pad[n-1]);
#ifdef USE_BAS
    palSetPad(out_channels_bas_port[n-1], out_channels_bas_pad[n-1]);
#endif
  }
}

// Live per-key sensitivity (Striso Live, parameter target 3): a factor on top of the factory
// calibration, c_force = factory c_force * factor. Initialised from the config (fCfac<key>) at boot;
// changes made live apply immediately and are saved to the config by the PC tools.
static int32_t c_force_factory[N_BUTTONS];
static float key_factor[61];

int buttonKeyCount(void) {
  return 61;
}

float buttonGetKeyFactor(int n) {
  return (n >= 0 && n < 61) ? key_factor[n] : 0.0f;
}

int buttonSetKeyFactor(int n, float f) {
  if (n < 0 || n >= 61 || !(f >= 0.1f && f <= 10.0f)) return 0;
  int but = button_number_map[n];
  key_factor[n] = f;
  ks_cal[but].c_force = (int32_t)(c_force_factory[but] * f + 0.5f);
  ks_calibration_changed();
  return 1;
}

void buttonSetCalibration(void) {
  uint16_t base_calib_force = devspec_id->base_calib_force;
  // use default when not device flash not yet initialized
  if (base_calib_force == 0xffff) base_calib_force = CALIB_FORCE;
  // fall back to old default when info not available (old struct)
  if (base_calib_force == 0x0000) base_calib_force = ((1<<18)/64);

  for (int n=0; n<N_BUTTONS; n++) {
    int status;
    int32_t c_force, zero_offset, pres;
    ks_key_state(n, &status, &c_force, &zero_offset, &pres);
    chprintf((BaseSequentialStream *)&BDU1, "but[%d] c_force: %d zero_offset: %d\r\n", n, c_force, zero_offset);
  }
  ks_reset_calibration(ks_cal, base_calib_force);
  ks_calibration_changed();
  chprintf((BaseSequentialStream *)&BDU1, "c_force: %d\r\n", base_calib_force);
}

/*
 * StcE: the scanner's counters since boot (key_scanner.h), and the message queue's (messaging.c).
 * Read only. Lines: "scan <name> <value>..."; histograms list their non-empty bins as <bin>:<count>.
 */
static scan_stats_t stats_copy;  // static: the console runs on main's 1 KB stack

void buttonPrintStats(BaseSequentialStream *out) {
  chSysLock();
  stats_copy = *(scan_stats_t *)&scan_stats;
  const int msg_underruns = underruns;
  chSysUnlock();
  const scan_stats_t *st = &stats_copy;
  chprintf(out, "scan scans %lu frames %lu seq_gaps %lu overruns %lu lost_wakeups %lu\r\n",
           st->scans, st->frames, st->seq_gaps, st->overruns, st->lost_wakeups);
  chprintf(out, "scan queue_full %lu off_retries %lu msg_underruns %d\r\n",
           st->queue_full, st->off_retries, msg_underruns);
  chprintf(out, "scan cycles n %lu min %lu max %lu mean %lu (%lu cycles/us)\r\n", st->cycles_n,
           st->cycles_min, st->cycles_max,
           st->cycles_n ? (uint32_t)(st->cycles_sum / st->cycles_n) : 0, STM32_SYS_CK / 1000000U);
  chprintf(out, "scan cycles_hist base %d step %d:", SCAN_HIST_BASE, SCAN_HIST_STEP);
  for (int i = 0; i < SCAN_HIST_BINS; i++) {
    if (st->cycles_hist[i]) chprintf(out, " %d:%lu", i, st->cycles_hist[i]);
  }
  chprintf(out, "\r\nscan callbacks_hist base %d:", SCAN_CB_BASE);
  for (int i = 0; i < SCAN_CB_BINS; i++) {
    if (st->callbacks_hist[i]) chprintf(out, " %d:%lu", SCAN_CB_BASE + i, st->callbacks_hist[i]);
  }
  chprintf(out, "\r\n");
  ks_capture_print(out);
}

/*
 * StcZ: every button's calibration state, read only (StcC, by contrast, resets c_force).
 * One line per button: index, key name (--- for the 7 positions without a key), status (0 off,
 * 1 starting, 2 on), c_force, the factory c_force, the key's sensitivity factor (x1000),
 * zero_offset (the learned zero level, 1<<24 = full scale) and the filtered pressure.
 */
void buttonPrintKeys(BaseSequentialStream *out) {
  chprintf(out, "keys %d: but key status c_force factory factor_milli zero_offset pres\r\n", N_BUTTONS);
  for (int b = 0; b < N_BUTTONS; b++) {
    char name[4] = "---";
    int factor = 0;
    for (int n = 0; n < 61; n++) {
      if (button_number_map[n] == b) {
        put_button_name(n, name);
        name[3] = 0;
        factor = (int)(key_factor[n] * 1000.0f + 0.5f);
      }
    }
    int status;
    int32_t c_force, zero_offset, pres;
    ks_key_state(b, &status, &c_force, &zero_offset, &pres);
    chprintf(out, "%d %s %d %ld %ld %d %ld %ld\r\n", b, name, status, c_force, c_force_factory[b], factor,
             zero_offset, pres);
  }
}

void ButtonReadStart(void) {

#if defined(USE_AUX_BUTTONS) && defined(STM32F4XX)
  palSetLineMode(LINE_BUTTON_PORT, PAL_MODE_INPUT_PULLDOWN);
  palSetLineMode(LINE_BUTTON_UP,   PAL_MODE_INPUT_PULLDOWN);
  palSetLineMode(LINE_BUTTON_DOWN, PAL_MODE_INPUT_PULLDOWN);
  palSetLineMode(LINE_BUTTON_ALT,  PAL_MODE_INPUT_PULLDOWN);
#endif

  for (int n=0; n<OUT_NUM_CHANNELS; n++) {
    // palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_OPENDRAIN | PAL_STM32_OSPEED_HIGHEST);
    palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
  }

  /*
   * Initializes the ADC driver.
   */
#ifdef STM32F4XX
  adcMultiStart();
#elif defined(STM32H7XX)
  adcStart(&ADCD1, &adccfg1);
#endif

  // Initialize buttons: c_force CALIB_FORCE, 0 for the positions without a key (52 54 57 59 61
  // 64 66), the filters and zero levels 0 (KeyScanner::init)
  ks_init();
#ifdef USE_BAS
  for (int n=0; n<N_BUTTONS_BAS; n++) {
    buttons_bas[n].but_id = n;
    buttons_bas[n].src_id = ID_BAS;
    buttons_bas[n].c_force = (ADCFACT>>6) / 6;//calib_bas[n];//(ADCFACT>>6) / 2;
    buttons_bas[n].c_offset = ADC_OFFSET;
  }
#endif

  uint32_t* const UID = (uint32_t*)UID_BASE;

  const bool calibrated = calib_dis_force->UID[0] == UID[0] &&
                          calib_dis_force->UID[1] == UID[1] &&
                          calib_dis_force->UID[2] == UID[2];
  static int32_t zero_offset[N_BUTTONS];  // static: main's stack is 1 KB
  ks_boot_calibration(calibrated, calib_dis_force->type, calib_dis_force->calib, ks_cal, c_force_factory,
                      key_factor, zero_offset);
  for (int n = 0; n < N_BUTTONS; n++) {
    if (zero_offset[n]) ks_set_zero_offset(n, zero_offset[n]);
  }
  if (!calibrated) {
    led_rgb(0xff0000);
    chThdSleepMilliseconds(200);
  }

  if (calib_dis_breakpoint->UID[0] == UID[0] &&
      calib_dis_breakpoint->UID[1] == UID[1] &&
      calib_dis_breakpoint->UID[2] == UID[2] &&
      calib_dis_breakpoint->type == 0xb1) {
    for (int n=0; n<N_BUTTONS; n++) {
      ks_cal[n].c_breakpoint = calib_dis_breakpoint->calib[n] << 16;
    }
  } else {
    for (int n=0; n<N_BUTTONS; n++) {
      ks_cal[n].c_breakpoint = INT32_MAX;
    }
  }
  if (calib_dis_force2->UID[0] == UID[0] &&
      calib_dis_force2->UID[1] == UID[1] &&
      calib_dis_force2->UID[2] == UID[2] &&
      calib_dis_force2->type == 0x01) {
    for (int n=0; n<N_BUTTONS; n++) {
      ks_cal[n].c_force2 = calib_dis_force2->calib[n];
    }
  }

  ks_calibration_changed();  // the scanner thread takes it over at its first scan
  scan_cur = scan_link_begin(&link, 0);
  ks_capture_start_dump_thread();

  /*
   * Start first ADC conversion. Next conversions are triggered from the adc callback.
   */
#ifdef STM32F4XX
  adcMultiStartConversion(&adcgrpcfg1, &adcgrpcfg2, &adcgrpcfg3, adc_samples, ADC_GRP1_BUF_DEPTH);
#elif defined(STM32H7XX)
  adcStartConversion(&ADCD1, &adcgrpcfg1, adc_samples, ADC_GRP1_BUF_DEPTH);
#endif

  /*
   * Creates the thread to process the adc samples
   */
  chThdCreateStatic(waThreadReadButtons, sizeof(waThreadReadButtons), NORMALPRIO, ThreadReadButtons, NULL);
}
