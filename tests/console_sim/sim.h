#ifndef SIM_H
#define SIM_H
#include <setjmp.h>
#include "ch.h"
#include "hal.h"
#include "chprintf.h"
#include "usbcfg.h"
#include "config.h"
#include "midi.h"
#include "synth.h"
#include "button_read.h"
#include "usb_audio.h"
#include "pconnection.h"

extern systime_t sim_time;
extern char sim_log[1 << 16];
extern size_t sim_log_len;
void sim_logf(const char *fmt, ...);
void sim_log_clear(void);
extern int sim_halted, sim_deadlocks, sim_sleeps, sim_resets, midi_in_calls;
extern const char *sim_halt_reason;
extern jmp_buf *sim_halt_jmp, *sim_reset_jmp;
extern int (*sim_block_hook)(threads_queue_t *tqp);
int sim_hook_reading_host(threads_queue_t *tqp);
extern uint8_t host_in[3][1 << 16];
extern size_t host_in_len[3];
extern int host_in_transfers[3];
long host_read(usbep_t ep);
size_t host_write(usbep_t ep, const uint8_t *data, size_t len);
void sim_sof(void);
int sim_host_drain(void);
void sim_usb_configure(void);
void sim_map_devspec(int erased);
void sim_init(void);
#endif
