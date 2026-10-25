/*
 * The key scan on the board (scan_thread.cpp): the ADC interrupt's sequence and the scanner
 * thread's work, the ScanSequencer and ScanConsumer (scan_sequencer.hpp) over the HAL.
 * button_read.c has the pins, the ADC set-up and the thread.
 */
#ifndef SCAN_THREAD_H_
#define SCAN_THREAD_H_

#include <stdint.h>

#include "hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the key matrix's output channels, 3 per column (button_read.c) */
extern const ioportid_t out_channels_port[51];
extern const iopadid_t out_channels_pad[51];
extern const ioportmask_t out_channels_portmask[51];

/* before the first conversion: the first scan's frame */
void scan_isr_init(void);
/* the ADC callback's work, with the four row samples (then the next conversion starts) */
void scan_isr_callback(const adcsample_t adc[4]);
/* the scanner thread's loop: the columns as they are complete, to the KeyScanner */
void scan_thread_loop(void);

#ifdef __cplusplus
}
#endif

#endif
