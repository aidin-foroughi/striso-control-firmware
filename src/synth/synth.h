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
// the C++ engines (synth_strisy.cpp) run more voices than the Faust synth; spaced "#  define" so
// faust2striso.py, which rewrites the line above, leaves this one alone
#ifdef SYNTH_ENGINE_STRISY
#  undef VOICECOUNT
#  define VOICECOUNT 10
#endif

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

// Live parameters. target 0: the current voice's engine, target 1: the master effects.
// Print the list as lines "index name group min max value kind", min/max/value as the 8 hex digits
// of the float's bits (exact, no float printf), preceded by "params <target> <name> <count>".
void synth_params_print(void *chp, int target);
// queue a parameter write, applied at the next audio block; returns 0 if out of range or queue full
int synth_param_set(int target, int index, float value);

// Global touch parameters (synth_control.cpp, parameter target 2): pressure, velocity, tilt and
// bend sensitivity and the note-on threshold. Applied immediately, not saved on the board.
int synth_touch_count(void);
const char* synth_touch_info(int i, float* lo, float* hi);
float synth_touch_get(int i);
int synth_touch_set(int i, float v);

// The active tuning and every key's pitch (console command StcN, read only; line format in
// src/synth/tuning.h) to a BaseSequentialStream.
void synth_tuning_print(void *chp);

int synth_message(int size, int* msg);
void synth_tick(void);
void midi_config(void);
void synth_control_init(void);

void clear_dead_notes(void);

extern float volume;

#endif /* SYNTH_H_ */
