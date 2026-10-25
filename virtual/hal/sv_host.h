/*
 * Inside the virtual board: what the hal stand-in (virtual/hal) and the board glue
 * (virtual/board) share with the instance (virtual/src/instance.cpp). Not part of the API.
 */
#ifndef SV_HOST_H
#define SV_HOST_H

#include <stdint.h>

#include "ch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* virtual time: frames at 48 kHz since boot; the 2 kHz system tick is every 24 frames, the codec
 * block interrupt every 32, the USB frame (SOF) every 48 */
#define SVH_TICK_FRAMES 24
#define SVH_SOF_FRAMES 48

/* --- scheduler (sched.cpp) ---------------------------------------------------------------------- */
void svh_sched_init(void);
/* runs the firmware's threads, highest priority first, until every one waits */
void svh_run(void);
/* ends every thread (shutdown) */
void svh_sched_shutdown(void);
uint64_t svh_now(void);
void svh_set_now(uint64_t frames);
/* a system tick: the tick count goes up, sleeping threads due wake */
void svh_tick(void);
/* the firmware reset itself: no thread runs any more */
int svh_halted(void);
/* a thread of the board that the virtual board has no code for (idle): only in the registry */
thread_t* svh_dummy_thread(const char* name, tprio_t prio, tstate_t state);
/* the main thread: runs fn (the firmware's main) when svh_run first runs */
thread_t* svh_start_main(tfunc_t fn, size_t stack);

/* --- board state (hal.c, usb_host.c) ------------------------------------------------------------- */
typedef struct svh_board_io {
	/* pal lines: 11 ports x 16 pads */
	uint8_t line[11 * 16];
	uint8_t line_mode_set[11 * 16];
} svh_board_io;
extern svh_board_io svh_io;

/* console byte queues (usb_host.c) */
void svh_usb_init(void);
void svh_usb_free(void);
int svh_usb_attached(void);
int32_t svh_console_write(const uint8_t* bytes, int32_t n);   /* host -> board */
int32_t svh_console_read(uint8_t* out, int32_t max);          /* board -> host */
int32_t svh_midi_write(const uint8_t* bytes, int32_t n);
int32_t svh_midi_read(uint8_t* out, int32_t max);
int32_t svh_usb_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t* data, uint16_t len);
/* a USB frame: the board's IN buffers go to the host */
void svh_usb_sof(void);

/* codec (board/stubs_host.c): started by codec_init */
int svh_codec_running(void);
/* the block interrupt: computebufI and the USB audio tick, as the SAI DMA interrupt does */
void svh_codec_interrupt(void);

/* audio of the last block the synth committed (board/usb_audio_host.c): set when a block is
 * committed; the instance takes it after the codec interrupt */
typedef struct svh_block {
	int valid;
	int32_t usb[2 * 32];
	float engine[32];
	float fx[2 * 32];
	float controls[10][5];
	float globals[8];
} svh_block;
extern svh_block svh_last_block;
/* taps into synth_strisy.cpp (board/fw_synth_strisy.cpp) */
void svh_synth_taps(float* engine, float* fx_l, float* fx_r, float (*controls)[5], float* globals);
void svh_synth_fx_init(void);
int svh_synth_blocks(void);

/* injection (board/buttons_host.c, board/motion_host.c) */
int32_t svh_inject_key(int32_t src, int32_t id, int32_t pres, int32_t velo, int32_t x, int32_t y);
int32_t svh_inject_control(int32_t id, int32_t value);
int32_t svh_ump_write(const uint32_t* words, int32_t n); /* MIDI 2.0 in: whole UMPs; -1 full */
void svh_inject_motion(const int16_t raw[6]);
/* sv_set_engine_controls took the globals over: the motion thread writes to a dummy (fw_synth_strisy.cpp) */
void svh_motion_off(void);

/* the shared memory of the front (fw_api.h sv_shared) */
struct sv_shared;
extern struct sv_shared* svh_shared;

/* the firmware's main (board/main_host.c) */
void svh_firmware_main(void* arg);

#ifdef __cplusplus
}
#endif

#endif
