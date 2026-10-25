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

// Synth thread running C++ voice engines instead of the Faust generated synth.cpp.
// Select with `make SYNTH_ENGINE=strisy` (default) or `make SYNTH_ENGINE=faust`.
//
// The voice engine is chosen with `make VOICE=<name>`: strisy (default), <name> for
// dsp/include/striso_dsp/engines/<name>.h, or `all` for every engine in one firmware, switched at
// runtime with synth_select_voice() (USB console commands '0'..'9', 'v' lists them).
// Every engine is a striso::dsp::VoiceEngine (dsp/include/striso_dsp/engine.h, doc/engines.md)
// and this file is its host on the board: it places the engines' state and memory, maps the keys'
// signals (synth_interface) to their controls in one place (controls_vc, controls_g), and
// dispatches through an EngineSet built from the engine list (SYNTH_ENGINES).

extern "C" {
#include "ch.h"
#include "hal.h"
#include "synth.h"
#include "usb_audio.h"
}

// per-section cycle counts for the 'P' console command (see synth_profile_print)
static volatile uint32_t prof[8];
static uint32_t prof_t;
static volatile uint32_t prof_blocks;
#define STRISY_PROF_START() (prof_t = DWT->CYCCNT)
#define STRISY_PROF_MARK(k) \
	do { \
		uint32_t t_ = DWT->CYCCNT; \
		prof[k] += t_ - prof_t; \
		prof_t = t_; \
	} while (0)

#include "striso_dsp/engine_set.h"
#include "striso_dsp/effects/fx.h"
#if defined(SYNTH_VOICE_ALL)
#include "striso_dsp/engines/strisy.h"
#include "striso_dsp/engines/va.h"
#include "striso_dsp/engines/modal.h"
#include "striso_dsp/engines/bowed.h"
#include "striso_dsp/engines/wind.h"
#include "striso_dsp/engines/ep.h"
#include "striso_dsp/engines/brass.h"
#include "striso_dsp/engines/trumpet.h"
// the engines of the VOICE=all firmware, in the order of the voice list ('v', the settings keys)
#define SYNTH_ENGINES_ALL E::strisy, E::va, E::modal, E::bowed, E::wind, E::ep, E::brass, E::trumpet
#define SYNTH_ENGINES SYNTH_ENGINES_ALL
#define SYNTH_DEFAULT_ENGINE "va" // at power on: its first entry
#elif defined(SYNTH_ENGINE_HEADER)
#include SYNTH_ENGINE_HEADER
#define SYNTH_ENGINES E::SYNTH_VOICE_NS
#else
#include "striso_dsp/engines/strisy.h"
#define SYNTH_ENGINES E::strisy
#endif
#include "chprintf.h"

#include <array>
#include <bit>
#include <cstdint>

namespace E = striso::dsp::engines;
using Engines = striso::dsp::EngineSet<SYNTH_ENGINES>;

// full scale of the codec's 24 bit samples, and the volume smoothing (one pole per block)
static constexpr int32_t MAX_VAL = 1 << 23;
static constexpr float VOLUME_FILTER = 0.99f;

template <class... V> static constexpr bool fit_the_board() {
	return ((VOICECOUNT <= V::info.budget.max_voices) && ...);
}
static_assert(fit_the_board<SYNTH_ENGINES>(), "too many voices for an engine");
static_assert(CHANNEL_BUFFER_SIZE == striso::dsp::BLOCK, "engine block size must match the codec buffer");

// Where the engines live: their state in DTCM (zero wait state, not shared with DMA), their
// FastMemory in AHB SRAM1/2 (ram1), their LargeMemory in AXI SRAM (.bss). All zero-filled at
// boot; init() starts an engine from there (no constructors run).
static Engines::States engines __attribute__((section(".ram5_clear")));
static Engines::Fast engines_fast __attribute__((section(".ram1_clear")));
static Engines::Large engines_large;

// selectable entries: an engine, or one preset of an engine
struct VoiceEntry {
	uint8_t engine;
	int8_t preset; // -1: engine without presets
	const char* engine_name;
	const char* preset_name;
};
template <class... V> static constexpr int count_entries() {
	return (0 + ... + (V::info.num_presets > 0 ? V::info.num_presets : 1));
}
static constexpr int MAX_ENTRIES = count_entries<SYNTH_ENGINES>();
static_assert(MAX_ENTRIES <= 128, "the console selects an entry with one byte, 0x80 + n (pconnection.c)");
static std::array<VoiceEntry, MAX_ENTRIES> entries;
static int num_entries;

static void build_entries(void) {
	num_entries = 0;
	for (int k = 0; k < Engines::count; k++) {
		Engines::visit(k, engines, [k](auto& e, auto) {
			const striso::dsp::EngineInfo& in = e.info;
			if (in.num_presets == 0) entries[num_entries++] = {(uint8_t)k, -1, in.name, nullptr};
			for (int i = 0; i < in.num_presets; i++)
				entries[num_entries++] = {(uint8_t)k, (int8_t)i, in.name, e.preset_name(i)};
		});
	}
}

// voice at power on: the first entry of this engine
[[nodiscard]] static constexpr bool same_name(const char* a, const char* b) {
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}
template <class... V> static constexpr int engine_index(const char* name) {
	int i = 0, found = 0;
	((same_name(V::info.name, name) ? found = i : 0, i++), ...);
	return found;
}
#ifdef SYNTH_DEFAULT_ENGINE
static constexpr int DEFAULT_ENGINE = engine_index<SYNTH_ENGINES>(SYNTH_DEFAULT_ENGINE);
#else
static constexpr int DEFAULT_ENGINE = 0;
#endif

// The keys write into one shared set of controls: synth_interface points into controls_vc and
// controls_g (the one mapping of the board's key signals to the engines' controls). Every block
// the gates, strikes and releases follow from them (track_key) and the active engine gets a copy.
static striso::dsp::VoiceControls controls_vc[VOICECOUNT];
static striso::dsp::GlobalControls controls_g;
static volatile int voice_request = 0; // entry to switch to, set by synth_select_voice()
static int voice_current = -1;         // entry running in the synth thread

static void engine_init(int k) {
	Engines::visit(k, engines, engines_fast, engines_large,
	    [](auto& e, auto& fast, auto& large, auto) { e.init(fast, large, SAMPLINGFREQ, VOICECOUNT); });
}

static void engine_set_preset(int k, int p) {
	Engines::visit(k, engines, [p](auto& e, auto) { e.set_preset(p); });
}

static void engine_process(int k, float* out) {
	for (int v = 0; v < VOICECOUNT; v++) striso::dsp::track_key(controls_vc[v]);
	float* const outs[1] = {out};
	Engines::visit(k, engines, [&outs](auto& e, auto) { e.process(controls_vc, controls_g, outs); });
}

[[nodiscard]] static int engine_active_voices(int k) {
	int n = 0;
	Engines::visit(k, engines, [&n](auto& e, auto) { n = e.active_voices(); });
	return n;
}

// master effects (mono in, stereo out), after the voice engine: state in DTCM, the delay line and
// the pre-delay in AHB SRAM1/2, the reverb ring in AXI SRAM
static striso::dsp::effects::fx fx_engine __attribute__((section(".ram5_clear")));
static striso::dsp::effects::fx::FastMemory fx_fast __attribute__((section(".ram1_clear")));
static striso::dsp::effects::fx::LargeMemory fx_large;

// ---------------------------------------------------------------------------------------------
// Live parameters: writes from the USB console are queued and applied at the start of an audio
// block in the synth thread, so they never change a value in the middle of the DSP.
// Target 0 is the current voice's engine, target 1 the master effects.

[[nodiscard]] static int engine_param_count(int k) {
	int n = 0;
	Engines::visit(k, engines, [&n](auto& e, auto) { n = e.num_params; });
	return n;
}

[[nodiscard]] static const striso::dsp::ParamDesc* engine_param_desc(int k, int i) {
	const striso::dsp::ParamDesc* d = nullptr;
	Engines::visit(k, engines, [&d, i](auto& e, auto) { d = e.param_desc(i); });
	return d;
}

[[nodiscard]] static float* engine_param_ptr(int k, int i) {
	float* p = nullptr;
	Engines::visit(k, engines, [&p, i](auto& e, auto) { p = e.param(i); });
	return p;
}

static void engine_set_param(int k, int i, float v) {
	Engines::visit(k, engines, [i, v](auto& e, auto) { (void)e.set_param(i, v); });
}

struct ParamWrite {
	uint8_t target, index;
	float value;
};
static constexpr int PARAM_QUEUE = 64;
static std::array<ParamWrite, PARAM_QUEUE> param_queue;
static volatile uint32_t param_head, param_tail; // single producer (USB), single consumer (synth)

// apply queued writes; called from the synth thread at the start of a block
static void apply_params(int engine) {
	while (param_tail != param_head) {
		const ParamWrite w = param_queue[param_tail % PARAM_QUEUE];
		// NaN ignored, clamped to [min, max], rounded unless continuous (strisy::write_param)
		if (w.target == 0) engine_set_param(engine, w.index, w.value);
		else (void)fx_engine.set_param(w.index, w.value);
		param_tail = param_tail + 1;
	}
}

synth_interface_t synth_interface;
static float dump;
float volume = 1.0f;
volatile synth_load_t synth_load;

static int32_t* outbuf;
static int32_t* inbuf;
static float output0[CHANNEL_BUFFER_SIZE];
static float out_l[CHANNEL_BUFFER_SIZE], out_r[CHANNEL_BUFFER_SIZE];

// 4 KB stack in DTCM (upstream's Faust synth used 1024 bytes, almost all of it)
static THD_WORKING_AREA(waSynthThread, 4096) __attribute__((section(".ram5_clear")));
static thread_t* pThreadDSP = 0;
static void synthThread(void* arg) { // THE SYNTH THREAD
	(void)arg;
	chRegSetThreadName("SYNTH");

	int32_t tmp;
	float volume_filtered = 0.0f;

	chEvtAddEvents(1);

	fx_engine.init(fx_fast, fx_large, SAMPLINGFREQ);

	// defaults as the Faust sliders: accelerometer at rest, pedal 3/16, note 69
	controls_g = striso::dsp::globals_at_rest;
	for (int v = 0; v < VOICECOUNT; v++) controls_vc[v] = striso::dsp::voice_at_rest;

	// voices beyond VOICECOUNT write to a dummy
	synth_interface.acc_abs = &controls_g.acc_abs;
	synth_interface.acc_x = &controls_g.acc_x;
	synth_interface.acc_y = &controls_g.acc_y;
	synth_interface.acc_z = &controls_g.acc_z;
	synth_interface.rot_x = &controls_g.rot_x;
	synth_interface.rot_y = &controls_g.rot_y;
	synth_interface.rot_z = &controls_g.rot_z;
	synth_interface.pedal = &controls_g.pedal;
	for (int v = 0; v < MAX_VOICECOUNT; v++) {
		bool used = v < VOICECOUNT;
		synth_interface.note[v] = used ? &controls_vc[v].note : &dump;
		synth_interface.pres[v] = used ? &controls_vc[v].pres : &dump;
		synth_interface.vpres[v] = used ? &controls_vc[v].vpres : &dump;
		synth_interface.but_x[v] = used ? &controls_vc[v].but_x : &dump;
		synth_interface.but_y[v] = used ? &controls_vc[v].but_y : &dump;
	}

	while (true) {
		chEvtWaitOne(1);
		rtcnt_t t_start = chSysGetRealtimeCounterX();

		// switch voices between blocks; the new one starts from a clean state
		int req = voice_request;
		if (req != voice_current) {
			engine_init(entries[req].engine);
			if (entries[req].preset >= 0) engine_set_preset(entries[req].engine, entries[req].preset);
			voice_current = req;
		}
		apply_params(entries[voice_current].engine);
		engine_process(entries[voice_current].engine, output0);
		{
			const float* const fx_in[1] = {output0};
			float* const fx_out[2] = {out_l, out_r};
			fx_engine.process(fx_in, fx_out);
		}
		prof_blocks = prof_blocks + 1; // (C++20: no ++ on volatile)

		volume_filtered = VOLUME_FILTER * volume_filtered + (1 - VOLUME_FILTER) * volume;

		// USB audio: the same 24 bit samples (or pre-volume, see config.h), into its ring (always)
		int32_t* usb = usb_audio_write_ptr();

		// convert float to int with scale, clamp and round; left and right interleaved
		for (int n = 0; n < CHANNEL_BUFFER_SIZE; n++) {
			for (int ch = 0; ch < 2; ch++) {
				const float x = ch ? out_r[n] : out_l[n];
				tmp = (int32_t)(x * volume_filtered * MAX_VAL);
				if (tmp <= -MAX_VAL) {
					tmp = -(MAX_VAL - 1);
				} else if (tmp >= MAX_VAL) {
					tmp = MAX_VAL - 1;
				}
				outbuf[2 * n + ch] = tmp * (1 << 8);
#if USB_AUDIO_PRE_VOLUME
				if (usb) {
					int32_t u = (int32_t)(x * (USB_AUDIO_GAIN * MAX_VAL));
					u = u <= -MAX_VAL ? -(MAX_VAL - 1) : (u >= MAX_VAL ? MAX_VAL - 1 : u);
					usb[2 * n + ch] = u * (1 << 8);
				}
#else
				if (usb) usb[2 * n + ch] = tmp * (1 << 8);
#endif
			}
		}
		if (usb) usb_audio_write_commit();

		rtcnt_t t_block = chSysGetRealtimeCounterX() - t_start;
		chSysLock();
		synth_load.sum += t_block;
		if (t_block > synth_load.peak) synth_load.peak = t_block;
		synth_load.blocks = synth_load.blocks + 1;
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
	build_entries();
	for (int n = 0; n < num_entries; n++) {
		if (entries[n].engine == DEFAULT_ENGINE) {
			voice_request = n;
			break;
		}
	}
	pThreadDSP = chThdCreateStatic(waSynthThread, sizeof(waSynthThread), NORMALPRIO + 2, synthThread, NULL);
}

extern "C" void computebufI(int32_t* inp, int32_t* outp);
void computebufI(int32_t* inp, int32_t* outp) {
	outbuf = outp;
	inbuf = inp;
	if (pThreadDSP) {
		chSysLockFromISR();
		chEvtSignalI(pThreadDSP, (eventmask_t)1);
		chSysUnlockFromISR();
	} else
		// no synth yet (the DMA starts before it, with the codec unmuted): silence (D12)
		for (int i = 0; i < PLAYBACK_BUFFER_SIZE; i++) {
			outp[i] = 0;
		}
}

int synth_num_voices(void) {
	return num_entries;
}

const char* synth_voice_name(int n, const char** preset) {
	if (n < 0 || n >= num_entries) return nullptr;
	if (preset) *preset = entries[n].preset_name;
	return entries[n].engine_name;
}

int synth_current_voice(void) {
	return voice_request;
}

int synth_select_voice(int n) {
	if (n < 0 || n >= num_entries) return 0;
	voice_request = n;
	return 1;
}

void synth_profile_print(void* chp) {
	static const char* names[8] = {"global", "voice setup", "1a control", "2b lp+bp", "2a osc", "2c body", "master", "1b coefs"};
	uint32_t p[8], blocks, total = 0;
	chSysLock();
	for (int k = 0; k < 8; k++) {
		p[k] = prof[k];
		prof[k] = 0;
		total += p[k];
	}
	blocks = prof_blocks;
	prof_blocks = 0;
	chSysUnlock();
	if (!blocks) return;
	const VoiceEntry& en = entries[voice_current];
	int active = engine_active_voices(en.engine);
	chprintf((BaseSequentialStream*)chp, "profile over %lu blocks, voice %s%s%s, %d voices active now, cycles per block:\r\n",
	    blocks, en.engine_name, en.preset_name ? ":" : "", en.preset_name ? en.preset_name : "", active);
	for (int k = 0; k < 8; k++)
		chprintf((BaseSequentialStream*)chp, "  %-12s %6lu\r\n", names[k], p[k] / blocks);
	chprintf((BaseSequentialStream*)chp, "  %-12s %6lu of %lu\r\n", "total", total / blocks, (uint32_t)SYNTH_BLOCK_CYCLES);
}

[[nodiscard]] static constexpr uint32_t float_bits(float f) { return std::bit_cast<uint32_t>(f); }

void synth_params_print(void* chp, int target) {
	BaseSequentialStream* out = (BaseSequentialStream*)chp;
	if (target == 0) {
		const VoiceEntry& en = entries[voice_request];
		const int k = en.engine;
		const int count = engine_param_count(k);
		chprintf(out, "params 0 %s%s%s %d\r\n", en.engine_name, en.preset_name ? ":" : "", en.preset_name ? en.preset_name : "", count);
		for (int i = 0; i < count; i++) {
			const striso::dsp::ParamDesc* d = engine_param_desc(k, i);
			const float* p = engine_param_ptr(k, i);
			chprintf(out, "%d %s %s %08lx %08lx %08lx %d\r\n", i, d->name, d->group,
			    float_bits(d->min), float_bits(d->max), float_bits(p ? *p : 0.0f), static_cast<int>(d->kind));
		}
	} else {
		const int count = fx_engine.num_params;
		chprintf(out, "params 1 fx %d\r\n", count);
		for (int i = 0; i < count; i++) {
			const striso::dsp::ParamDesc* d = fx_engine.param_desc(i);
			const float* p = fx_engine.param(i);
			chprintf(out, "%d %s %s %08lx %08lx %08lx %d\r\n", i, d->name, d->group,
			    float_bits(d->min), float_bits(d->max), float_bits(p ? *p : 0.0f), static_cast<int>(d->kind));
		}
	}
}

uint32_t synth_params_signature(void) {
	uint32_t h = 2166136261u; // FNV-1a over 32-bit words
	const auto mix = [&h](uint32_t w) {
		for (int b = 0; b < 4; b++) h = (h ^ ((w >> (8 * b)) & 0xff)) * 16777619u;
	};
	mix(param_head);
	mix(static_cast<uint32_t>(voice_request));
	const int k = entries[voice_request].engine;
	for (int i = 0; i < engine_param_count(k); i++) {
		const float* p = engine_param_ptr(k, i);
		mix(p ? float_bits(*p) : 0u);
	}
	for (int i = 0; i < fx_engine.num_params; i++) {
		const float* p = fx_engine.param(i);
		mix(p ? float_bits(*p) : 0u);
	}
	return h;
}

int synth_param_set(int target, int index, float value) {
	if (target < 0 || target > 1 || index < 0) return 0;
	const int count = target == 0 ? engine_param_count(entries[voice_request].engine) : fx_engine.num_params;
	if (index >= count) return 0;
	if (param_head - param_tail >= (uint32_t)PARAM_QUEUE) return 0;
	param_queue[param_head % PARAM_QUEUE] = {(uint8_t)target, (uint8_t)index, value};
	param_head = param_head + 1;
	return 1;
}
