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
#ifndef SYNTH_H_
#define SYNTH_H_

#include "ch.h"
#include "hal.h"

#include "config.h"
#include "midi.h"
#include "codec.h"

#define VOICECOUNT 6

typedef struct struct_synth_interface {
	float* acc_abs;
	float* acc_x;
	float* acc_y;
	float* acc_z;
	float* rot_x;
	float* rot_y;
	float* rot_z;
	float* pedal;
	float* note[MAX_VOICECOUNT];
	float* pres[MAX_VOICECOUNT];
	float* vpres[MAX_VOICECOUNT];
	float* but_x[MAX_VOICECOUNT];
	float* but_y[MAX_VOICECOUNT];
} synth_interface_t;

// CPU cycles available per audio block
#define SYNTH_BLOCK_CYCLES ((uint32_t)((uint64_t)STM32_SYS_CK * CHANNEL_BUFFER_SIZE / SAMPLINGFREQ))

// synth DSP load in CPU cycles, accumulated since last read (read with chSysLock held)
typedef struct struct_synth_load {
	uint64_t sum;
	uint32_t peak;
	uint32_t blocks;
} synth_load_t;

#ifdef USE_INTERNAL_SYNTH
extern synth_interface_t synth_interface;
extern volatile synth_load_t synth_load;

void start_synth_thread(void);
#endif

// print and reset the synth engine profile to a BaseSequentialStream (no-op if unsupported)
void synth_profile_print(void *chp);

// Selectable voices: engines, or presets of an engine (synth_strisy.cpp; the Faust synth has one).
int synth_num_voices(void);
// engine name of voice n (NULL past the end); *preset gets the preset name or NULL
const char* synth_voice_name(int n, const char** preset);
int synth_current_voice(void);
// switch to voice n at the next audio block; returns 0 if n is out of range
int synth_select_voice(int n);

int synth_message(int size, int* msg);
void synth_tick(void);
void midi_config(void);
void synth_control_init(void);

void clear_dead_notes(void);

extern float volume;

#endif /* SYNTH_H_ */
