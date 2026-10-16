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

// Synth thread running the hand-optimized C++ engine (strisy.h) instead of the Faust generated
// synth.cpp. Select with `make SYNTH_ENGINE=strisy` (default) or `make SYNTH_ENGINE=faust`.

extern "C" {
    #include "ch.h"
    #include "hal.h"
    #include "synth.h"
}

// per-section cycle counts for the 'P' console command (see synth_profile_print)
static volatile uint32_t prof[8];
static uint32_t prof_t;
static volatile uint32_t prof_blocks;
#define STRISY_PROF_START() (prof_t = DWT->CYCCNT)
#define STRISY_PROF_MARK(k) do { uint32_t t_ = DWT->CYCCNT; prof[k] += t_ - prof_t; prof_t = t_; } while (0)

#include "strisy.h"
#include "chprintf.h"

#define MAX_VAL (1<<23)
#define VOLUME_FILTER 0.99f

// Engine state and noise table live in DTCM: zero wait state, not shared with DMA.
static strisy::Engine engine __attribute__((section(".ram5_clear")));

synth_interface_t synth_interface;
static float dump;
float volume = 1.0f;
volatile synth_load_t synth_load;

static int32_t *outbuf;
static int32_t *inbuf;
static float output0[CHANNEL_BUFFER_SIZE];

static_assert(CHANNEL_BUFFER_SIZE == strisy::BLOCK, "engine block size must match the codec buffer");
static_assert(VOICECOUNT <= strisy::MAX_VOICES, "too many voices for the engine");

// 4 KB stack in DTCM (upstream's Faust synth used 1024 bytes, almost all of it)
static THD_WORKING_AREA(waSynthThread, 4096) __attribute__((section(".ram5_clear")));
static thread_t* pThreadDSP = 0;
static void synthThread(void *arg) {  // THE SYNTH THREAD
	(void)arg;
	chRegSetThreadName("SYNTH");

	int32_t tmp;
	float volume_filtered = 0.0f;

	chEvtAddEvents(1);

	strisy::init(engine, SAMPLINGFREQ, VOICECOUNT);

	// controls the engine doesn't use, and voices beyond VOICECOUNT, write to a dummy
	synth_interface.acc_abs = &engine.g.acc_abs;
	synth_interface.acc_x = &engine.g.acc_x;
	synth_interface.acc_y = &engine.g.acc_y;
	synth_interface.acc_z = &engine.g.acc_z;
	synth_interface.rot_x = &engine.g.rot_x;
	synth_interface.rot_y = &engine.g.rot_y;
	synth_interface.rot_z = &engine.g.rot_z;
	synth_interface.pedal = &engine.g.pedal;
	for (int v = 0; v < MAX_VOICECOUNT; v++) {
		bool used = v < VOICECOUNT;
		synth_interface.note[v] = used ? &engine.vc[v].note : &dump;
		synth_interface.pres[v] = used ? &engine.vc[v].pres : &dump;
		synth_interface.vpres[v] = used ? &engine.vc[v].vpres : &dump;
		synth_interface.but_x[v] = used ? &engine.vc[v].but_x : &dump;
		synth_interface.but_y[v] = used ? &engine.vc[v].but_y : &dump;
	}

	while (true) {
		chEvtWaitOne(1);
		rtcnt_t t_start = chSysGetRealtimeCounterX();

		strisy::process(engine, output0);
		prof_blocks++;

		volume_filtered = VOLUME_FILTER * volume_filtered + (1 - VOLUME_FILTER) * volume;

		// convert float to int with scale, clamp and round
		for (int n = 0; n < CHANNEL_BUFFER_SIZE; n++) {
			tmp = (int32_t)(output0[n] * volume_filtered * MAX_VAL);
			if (tmp <= -MAX_VAL) {
				tmp = -(MAX_VAL-1);
			} else if (tmp >= MAX_VAL) {
				tmp = MAX_VAL-1;
			}
			// make both audio channels the same
			outbuf[2*n] = outbuf[2*n+1] = tmp * (1<<8);
		}

		rtcnt_t t_block = chSysGetRealtimeCounterX() - t_start;
		chSysLock();
		synth_load.sum += t_block;
		if (t_block > synth_load.peak) synth_load.peak = t_block;
		synth_load.blocks++;
		chSysUnlock();

		// Overload protection: the synth thread outranks buttons, USB console and LEDs,
		// so a too heavy synth would starve them. Yield a tick to keep the board responsive.
		if (t_block > SYNTH_BLOCK_CYCLES / 10 * 9) {
			chThdSleep(1);
		}

		if (chThdShouldTerminateX()) break;
	}

	pThreadDSP = NULL;
	palToggleLine(LINE_LED1);
}

void start_synth_thread(void) {
	pThreadDSP = chThdCreateStatic(waSynthThread, sizeof(waSynthThread), NORMALPRIO+2, synthThread, NULL);
}

extern "C" void computebufI(int32_t *inp, int32_t *outp);
void computebufI(int32_t *inp, int32_t *outp) {
	outbuf = outp;
	inbuf = inp;
	if (pThreadDSP) {
		chSysLockFromISR();
		chEvtSignalI(pThreadDSP, (eventmask_t)1);
		chSysUnlockFromISR();
	}
	else
		for (int i = 0; i < PLAYBACK_BUFFER_SIZE; i++) {
			outp[i] = (i - PLAYBACK_BUFFER_SIZE / 2) * 1<<22;
		}
}

void synth_profile_print(void *chp) {
	static const char* names[8] = {"global", "voice setup", "1a control", "2b lp+bp", "2a osc", "2c body", "master", "1b coefs"};
	uint32_t p[8], blocks, total = 0;
	chSysLock();
	for (int k = 0; k < 8; k++) { p[k] = prof[k]; prof[k] = 0; total += p[k]; }
	blocks = prof_blocks; prof_blocks = 0;
	chSysUnlock();
	if (!blocks) return;
	int active = 0;
	for (int v = 0; v < engine.nvoices; v++) active += engine.vs[v].active;
	chprintf((BaseSequentialStream *)chp, "profile over %lu blocks, %d voices active now, cycles per block:\r\n", blocks, active);
	for (int k = 0; k < 8; k++)
		chprintf((BaseSequentialStream *)chp, "  %-12s %6lu\r\n", names[k], p[k] / blocks);
	chprintf((BaseSequentialStream *)chp, "  %-12s %6lu of %lu\r\n", "total", total / blocks, (uint32_t)SYNTH_BLOCK_CYCLES);
}
