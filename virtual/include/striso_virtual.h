/*
 * striso_virtual: the Striso board's firmware logic on the PC, as a shared library.
 *
 * The same source files as the board (src/synth/synth_control.cpp, src/synth/synth_strisy.cpp
 * with every voice engine, src/app/console.cpp, messaging.cpp, control_thread.cpp, config_store.cpp, src/drivers/led.c)
 * run on a small ChibiOS/HAL stand-in (virtual/hal), in virtual time: a deterministic scheduler
 * runs the firmware's threads one at a time by priority, the system tick (2 kHz), the codec's
 * block interrupt (48 kHz / 32) and the USB frame (1 kHz) come from the frames rendered. The
 * hardware-only parts (ADC key scanning, codec, USB descriptors and audio streaming, motion sensor
 * I2C) are replaced: key and control messages are injected where the key scanner sends them
 * (event_send), motion where the sensor thread reads it, and the USB bulk console is a pair of byte
 * queues with the same protocol as the board (Stc commands, Q writes, the S/s binary stream).
 *
 * Every board is its own copy of the library's code and data (a fresh copy is loaded at create
 * and after every reset), so boards are independent, and a reset (StcB, settings + key 67) starts
 * the firmware from its power-on state while the config flash and the RTC backup register survive.
 *
 * Thread safety: every call on a board is serialised by the board's lock; calls may come from any
 * thread. With sv_realtime_start() a library thread runs the board in real time and the audio goes
 * to a FIFO (sv_read_audio); without it the board only runs inside sv_render/sv_advance.
 *
 * All functions return >= 0 on success and SV_E* (< 0) on failure unless documented otherwise.
 */
#ifndef STRISO_VIRTUAL_H
#define STRISO_VIRTUAL_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(STRISO_VIRTUAL_BUILD)
#define SV_API __declspec(dllexport)
#else
#define SV_API __declspec(dllimport)
#endif
#else
#define SV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on an incompatible change of a function or struct below. */
#define SV_API_VERSION 2

#define SV_SAMPLE_RATE 48000
#define SV_BLOCK 32
/* the user config area in flash, 0x08020000..0x08040000 (16-byte records, see config_store.h) */
#define SV_FLASH_ADDR 0x08020000u
#define SV_FLASH_SIZE 0x20000

/* errors */
#define SV_EINVAL (-1)    /* bad argument */
#define SV_ENODEV (-2)    /* the USB console is not there: booting, resetting, in the bootloader */
#define SV_EBUSY (-3)     /* sv_render/sv_advance while the real-time thread runs the board */
#define SV_ELOAD (-4)     /* the board's code could not be loaded (sv_last_error says why) */
#define SV_ESTATE (-5)    /* not possible in this state */

typedef struct sv_board sv_board;

typedef struct sv_options {
	uint32_t size;          /* sizeof(sv_options) */
	const uint8_t* flash;   /* the config area to start with (e.g. from sv_get_flash), NULL: erased */
	uint32_t flash_len;     /* bytes of flash (at most SV_FLASH_SIZE; the rest stays erased) */
	const char* drive_dir;  /* where the bootloader's update drive appears; NULL: a temp directory */
	int32_t port_key_held;  /* boot with the portamento key held: synth_control_init skips the config */
	uint32_t serial;        /* the last word of the board's UID (to tell boards apart), 0: 1 */
	int32_t boot_ms;        /* virtual time sv_create runs the boot for: 0 the default 1000, < 0 none */
} sv_options;

SV_API int32_t sv_api_version(void);
/* the firmware version the board reports (FWVERSION, as StcV prints it first) */
SV_API const char* sv_firmware_version(void);
/* the last error of sv_create on this thread (or ""). */
SV_API const char* sv_last_error(void);

/* Boots a board: from power on, the boot (USB connects after 500 ms of virtual time, as on the
 * board) runs for options->boot_ms. options may be NULL (all defaults). */
SV_API sv_board* sv_create(const sv_options* options);
SV_API void sv_destroy(sv_board* b);

/* --- time and audio --------------------------------------------------------------------------- */

/* Runs the board for `frames` of virtual time (48 kHz) and writes what the synth gave the USB
 * audio stream in that time: interleaved stereo float, the board's int32 samples (24 bit << 8)
 * divided by 2^31 (the volume applied, as the codec gets it). stereo may be NULL. */
SV_API int32_t sv_render(sv_board* b, float* stereo, int32_t frames);
/* The same with every tap, each may be NULL: usb (stereo, as sv_render), engine (mono: the voice
 * engine's output, before the effects and the volume: what tests/golden/renders.txt hashes),
 * fx (stereo float: after the master effects, before the volume). */
SV_API int32_t sv_render_taps(sv_board* b, int32_t frames, float* usb, float* engine, float* fx);
SV_API int32_t sv_advance_ms(sv_board* b, int32_t ms);
/* virtual time since sv_create, in frames */
SV_API uint64_t sv_time_frames(sv_board* b);

/* Real time: a library thread runs the board with the wall clock and keeps the latest
 * fifo_frames of USB audio (0: 1 s) for sv_read_audio. sv_render/sv_advance return SV_EBUSY. */
SV_API int32_t sv_realtime_start(sv_board* b, int32_t fifo_frames);
SV_API int32_t sv_realtime_stop(sv_board* b);
/* Takes up to max_frames of the real-time audio (interleaved stereo). Returns the frames taken. */
SV_API int32_t sv_read_audio(sv_board* b, float* stereo, int32_t max_frames);

/* An audio callback that renders the board, for an output device's own thread (Striso Live's
 * audition: striso_audio's sa_set_source(sv_audio_source, board)): runs the board for the
 * buffer's time and writes its USB audio, resampled (linear) when the device is not at 48 kHz.
 * Returns frames written (0 while real time runs: one of the two drives the board). */
SV_API int32_t sv_audio_source(void* board, float* stereo, int32_t frames, int32_t rate);

/* --- USB ---------------------------------------------------------------------------------------- */

#define SV_USB_DETACHED 0   /* off the bus: booting, resetting */
#define SV_USB_ATTACHED 1   /* the console (interface 2, bulk 0x02/0x82) and USB MIDI are there */
#define SV_USB_BOOTLOADER 2 /* the UF2 bootloader's drive is there instead (sv_drive_path) */
SV_API int32_t sv_usb_state(sv_board* b);
/* counts attaches: a client that saw generation g and now sees another lost its connection */
SV_API uint32_t sv_usb_generation(sv_board* b);

/* The bulk console: bytes from the host (EP 0x02), as the board's console reads them; bytes to
 * the host (EP 0x82), what the board printed or streamed. SV_ENODEV when not attached. */
SV_API int32_t sv_console_write(sv_board* b, const uint8_t* bytes, int32_t n);
SV_API int32_t sv_console_read(sv_board* b, uint8_t* out, int32_t max);
/* USB MIDI, 4-byte USB-MIDI event packets both ways (the board's MIDI out: MPE, CCs) */
SV_API int32_t sv_midi_write(sv_board* b, const uint8_t* packets, int32_t n);
SV_API int32_t sv_midi_read(sv_board* b, uint8_t* out, int32_t max);
/* MIDI 2.0 in: Universal MIDI Packets, n 32-bit words of whole packets (a packet cut off at the
 * end waits for its rest). MIDI 2.0 channel voice messages (type 4) play the internal synth in
 * their resolution (doc/engines.md, "MIDI 2.0": the standard semantics, full-scale pressure = the
 * Striso's full press); MIDI 1.0 ones (type 2) and system messages (type 1) are as sv_midi_write's;
 * the other types are skipped; every group is played. Returns n, SV_EBUSY when the queue (1024
 * words) is full, SV_ENODEV when not attached. */
SV_API int32_t sv_ump_write(sv_board* b, const uint32_t* words, int32_t n);

/* --- injection ------------------------------------------------------------------------------------ */

/* A key message as the key scanner sends it (key_scanner.cpp: messaging::send of 4 values):
 * src ID_DIS (0), id the button 0..67, pres/velo/x/y as the scanner's 14 bit values (8191 = 1.0;
 * pres 0 releases). It goes through the firmware's message queue, the control thread
 * (synth_message, the binary stream) and synth_control's key handling. */
SV_API int32_t sv_inject_key(sv_board* b, int32_t src, int32_t id, int32_t pres, int32_t velo, int32_t x, int32_t y);
/* An aux control message (ID_CONTROL): IDC_PORTAMENTO 0, IDC_OCT_UP 1, IDC_OCT_DOWN 2,
 * IDC_ALT 3 (the settings key), IDC_PEDAL_EXP 11, IDC_PEDAL_1 12, IDC_PEDAL_2 13; value 1/0. */
SV_API int32_t sv_inject_control(sv_board* b, int32_t id, int32_t value);
/* The motion sensor's raw reading as the LSM6DSL gives it (gyro x y z, accel x y z; +-2 g,
 * +-500 dps full scale): the motion thread turns it into the synth's controls and the stream.
 * At power on the board lies flat at rest: {0, 0, 0, 0, 0, 16384}. */
SV_API int32_t sv_inject_motion(sv_board* b, const int16_t raw[6]);

/* Engine-level controls, bypassing the keys: writes voice v's controls (note, pres, vpres, but_x,
 * but_y) where synth_control writes them (synth_interface), or with v = -1 the 8 globals
 * (acc_abs, acc_x, acc_y, acc_z, rot_x, rot_y, rot_z, pedal); the motion thread then stops
 * writing the globals. For renders of scripted performances (the golden checks). */
SV_API int32_t sv_set_engine_controls(sv_board* b, int32_t v, const float* values);
/* synth_select_voice(n), as the console's voice command calls it: the engine starts from a clean
 * state at the next block */
SV_API int32_t sv_select_voice(sv_board* b, int32_t n);

/* --- observable state ------------------------------------------------------------------------- */

#define SV_LED_OFF 0
#define SV_LED_DIM 1
#define SV_LED_ON 2
typedef struct sv_leds {
	int32_t rgb[3];      /* the indicator's colour as last set (led.c last_color), 0..255 */
	int32_t pwm[3];      /* its PWM duty now, gamma corrected (0..2048): shows blinks */
	int32_t updown[4];   /* up2, up, down, down2: SV_LED_* */
	int32_t led1;        /* the blinker line */
} sv_leds;
SV_API int32_t sv_get_leds(sv_board* b, sv_leds* out);

typedef struct sv_state {
	uint32_t size;          /* sizeof(sv_state) */
	int32_t usb;            /* SV_USB_* */
	int32_t voice;          /* synth_current_voice() */
	int32_t voices;         /* synth_num_voices() */
	int32_t streaming;      /* config.send_usb_bulk */
	int32_t midi_mode;      /* config.midi_mode: 0 MPE, 1 normal, 2 mono */
	int32_t resets;         /* resets since sv_create */
	int32_t blocks;         /* audio blocks the synth rendered since the last boot */
	int32_t to_block;       /* frames until the codec's next block interrupt (-1: codec off) */
	uint32_t underruns;     /* events the message queue refused (messaging.cpp) */
	float volume;           /* the synth's volume factor */
	float controls[10][5];  /* the voices' controls the engine got last block */
	float globals[8];
} sv_state;
SV_API int32_t sv_get_state(sv_board* b, sv_state* out);

/* The config area as the board has it now (SV_FLASH_SIZE bytes): keep it to start a board with
 * the same settings later. Returns the bytes copied. */
SV_API int32_t sv_get_flash(sv_board* b, uint8_t* out, int32_t max);
/* The bootloader's update drive directory (exists while sv_usb_state is SV_USB_BOOTLOADER):
 * INFO_UF2.TXT, CONFIG.UF2; a UF2 written there that covers the config area is flashed and the
 * board restarts. Returns the length of the path (0 when not in the bootloader). */
SV_API int32_t sv_drive_path(sv_board* b, char* out, int32_t max);

/* --- tests ---------------------------------------------------------------------------------------- */

/* Test hooks: SV_TEST_FX_INIT re-initialises the master effects (fx::init) so a render starts as
 * the host renderers do; SV_TEST_MOTION_OFF stops the motion thread writing the globals. */
#define SV_TEST_FX_INIT 1
#define SV_TEST_MOTION_OFF 2
SV_API int32_t sv_test(sv_board* b, int32_t what, int32_t arg);

#ifdef __cplusplus
}
#endif

#endif /* STRISO_VIRTUAL_H */
