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
// The voice engine is chosen with `make VOICE=<name>`: strisy (default, strisy.h, the port of
// synth.dsp), voice_<name>.h, or `all` for every engine in one firmware, switched at runtime
// with synth_select_voice() (USB console commands '0'..'9', 'v' lists them).
// An engine header provides, in namespace <name>:
//   typedef float real; BLOCK (= 32), MAX_VOICES (>= VOICECOUNT)
//   struct Engine with members  strisy::GlobalControls g;  strisy::VoiceControls vc[MAX_VOICES];
//                               int nvoices;
//   void init(Engine&, int sample_rate, int nvoices);
//   void process(Engine&, real* output);          // BLOCK mono samples, about +-4 full scale
//   int active_voices(const Engine&);              // for the profile print
// The Engine is placed in DTCM (zero-initialized, no constructors run): it must be plain data.

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

#include "striso_dsp/strisy.h"
// VOICE_LIST(X, XP): X(ns) for an engine, XP(ns) for an engine with presets (NUM_PRESETS,
// preset_name(i), set_preset(Engine&, i)), which become one selectable entry each.
#if defined(SYNTH_VOICE_ALL)
#include "striso_dsp/voice_va.h"
#include "striso_dsp/voice_modal.h"
#include "striso_dsp/voice_bowed.h"
#include "striso_dsp/voice_wind.h"
#include "striso_dsp/voice_ep.h"
#include "striso_dsp/voice_brass.h"
#include "striso_dsp/voice_trumpet.h"
#define VOICE_LIST(X, XP) X(strisy) XP(va) XP(modal) XP(bowed) XP(wind) XP(ep) XP(brass) XP(trumpet)
#elif defined(SYNTH_VOICE_HEADER)
#include SYNTH_VOICE_HEADER
#define VOICE_LIST(X, XP) X(SYNTH_VOICE_NS)
#else
#define VOICE_LIST(X, XP) X(strisy)
#endif
#include "striso_dsp/fx.h"
#include "chprintf.h"

#include <array>
#include <bit>
#include <cstdint>

// full scale of the codec's 24 bit samples, and the volume smoothing (one pole per block)
static constexpr int32_t MAX_VAL = 1 << 23;
static constexpr float VOLUME_FILTER = 0.99f;

// Every engine's state lives in DTCM: zero wait state, not shared with DMA. The two level macros
// make sure a macro argument (SYNTH_VOICE_NS) is expanded before it is pasted.
#define DECLARE_ENGINE(ns) DECLARE_ENGINE_(ns)
#define DECLARE_ENGINE_(ns) \
	static ns::Engine engine_##ns __attribute__((section(".ram5_clear"))); \
	static_assert(CHANNEL_BUFFER_SIZE == ns::BLOCK, "engine block size must match the codec buffer"); \
	static_assert(VOICECOUNT <= ns::MAX_VOICES, "too many voices for the engine");
VOICE_LIST(DECLARE_ENGINE, DECLARE_ENGINE)

#define VOICE_ENUM(ns) VOICE_ENUM_(ns)
#define VOICE_ENUM_(ns) VOICE_##ns,
enum { VOICE_LIST(VOICE_ENUM, VOICE_ENUM) NUM_ENGINES };

// selectable entries: an engine, or one preset of an engine
struct VoiceEntry {
	uint8_t engine;
	int8_t preset; // -1: engine without presets
	const char* engine_name;
	const char* preset_name;
};
// every entry of VOICE_LIST: one per engine without presets, one per preset of the others
#define COUNT_ENTRY(ns) +1
#define COUNT_PRESETS(ns) COUNT_PRESETS_(ns)
#define COUNT_PRESETS_(ns) +ns::NUM_PRESETS
static constexpr int MAX_ENTRIES = 0 VOICE_LIST(COUNT_ENTRY, COUNT_PRESETS);
static_assert(MAX_ENTRIES <= 128, "the console selects an entry with one byte, 0x80 + n (pconnection.c)");
static std::array<VoiceEntry, MAX_ENTRIES> entries;
static int num_entries;

static void build_entries(void) {
	num_entries = 0;
#define ADD_ENTRY(ns) ADD_ENTRY_(ns)
#define ADD_ENTRY_(ns) entries[num_entries++] = {VOICE_##ns, -1, #ns, nullptr};
#define ADD_PRESETS(ns) ADD_PRESETS_(ns)
#define ADD_PRESETS_(ns) \
	for (int i = 0; i < ns::NUM_PRESETS; i++) entries[num_entries++] = {VOICE_##ns, (int8_t)i, #ns, ns::preset_name(i)};
	VOICE_LIST(ADD_ENTRY, ADD_PRESETS)
}

// voice at power on: the first entry of this engine
#if defined(SYNTH_VOICE_ALL)
static constexpr int DEFAULT_ENGINE = VOICE_va;
#else
static constexpr int DEFAULT_ENGINE = 0;
#endif

// The keys write into one shared set of controls; the active engine gets a copy every block.
static strisy::VoiceControls controls_vc[VOICECOUNT];
static strisy::GlobalControls controls_g;
static volatile int voice_request = 0; // entry to switch to, set by synth_select_voice()
static int voice_current = -1;         // entry running in the synth thread

template <class E> static inline void copy_controls(E& e) {
	e.g = controls_g;
	for (int v = 0; v < VOICECOUNT; v++) e.vc[v] = controls_vc[v];
}

static void engine_init(int k) {
	switch (k) {
#define CASE_INIT(ns) CASE_INIT_(ns)
#define CASE_INIT_(ns) \
	case VOICE_##ns: ns::init(engine_##ns, SAMPLINGFREQ, VOICECOUNT); break;
		VOICE_LIST(CASE_INIT, CASE_INIT)
	}
}

static void engine_set_preset(int k, int p) {
	(void)p;
	switch (k) {
#define CASE_NO_PRESET(ns)
#define CASE_PRESET(ns) CASE_PRESET_(ns)
#define CASE_PRESET_(ns) \
	case VOICE_##ns: ns::set_preset(engine_##ns, p); break;
		VOICE_LIST(CASE_NO_PRESET, CASE_PRESET)
	default: break;
	}
}

static void engine_process(int k, float* out) {
	switch (k) {
#define CASE_PROCESS(ns) CASE_PROCESS_(ns)
#define CASE_PROCESS_(ns) \
	case VOICE_##ns: \
		copy_controls(engine_##ns); \
		ns::process(engine_##ns, out); \
		break;
		VOICE_LIST(CASE_PROCESS, CASE_PROCESS)
	}
}

[[nodiscard]] static int engine_active_voices(int k) {
	switch (k) {
#define CASE_ACTIVE(ns) CASE_ACTIVE_(ns)
#define CASE_ACTIVE_(ns) \
	case VOICE_##ns: return ns::active_voices(engine_##ns);
		VOICE_LIST(CASE_ACTIVE, CASE_ACTIVE)
	}
	return 0;
}

// master effects (mono in, stereo out), after the voice engine
static fx::Engine fx_engine __attribute__((section(".ram5_clear")));

// ---------------------------------------------------------------------------------------------
// Live parameters: writes from the USB console are queued and applied at the start of an audio
// block in the synth thread, so they never change a value in the middle of the DSP.
// Target 0 is the current voice's engine, target 1 the master effects.

template <class E> [[nodiscard]] static constexpr int param_count(E&) { return strisy::EngineParams<E>::count; }

[[nodiscard]] static int engine_param_count(int k) {
	switch (k) {
#define CASE_PCOUNT(ns) CASE_PCOUNT_(ns)
#define CASE_PCOUNT_(ns) \
	case VOICE_##ns: return param_count(engine_##ns);
		VOICE_LIST(CASE_PCOUNT, CASE_PCOUNT)
	}
	return 0;
}

[[nodiscard]] static const strisy::ParamDesc* engine_param_desc(int k, int i) {
	switch (k) {
#define CASE_PDESC(ns) CASE_PDESC_(ns)
#define CASE_PDESC_(ns) \
	case VOICE_##ns: return strisy::EngineParams<ns::Engine>::desc(i);
		VOICE_LIST(CASE_PDESC, CASE_PDESC)
	}
	return nullptr;
}

[[nodiscard]] static float* engine_param_ptr(int k, int i) {
	switch (k) {
#define CASE_PPTR(ns) CASE_PPTR_(ns)
#define CASE_PPTR_(ns) \
	case VOICE_##ns: return strisy::EngineParams<ns::Engine>::ptr(engine_##ns, i);
		VOICE_LIST(CASE_PPTR, CASE_PPTR)
	}
	return nullptr;
}

static void engine_param_changed(int k, int i) {
	switch (k) {
#define CASE_PCHG(ns) CASE_PCHG_(ns)
#define CASE_PCHG_(ns) \
	case VOICE_##ns: strisy::EngineParams<ns::Engine>::changed(engine_##ns, i); break;
		VOICE_LIST(CASE_PCHG, CASE_PCHG)
	}
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
		const strisy::ParamDesc* d;
		float* p;
		if (w.target == 0) {
			d = engine_param_desc(engine, w.index);
			p = engine_param_ptr(engine, w.index);
		} else {
			d = strisy::EngineParams<fx::Engine>::desc(w.index);
			p = strisy::EngineParams<fx::Engine>::ptr(fx_engine, w.index);
		}
		if (d && p && w.value == w.value) {
			float v = w.value < d->min ? d->min : (w.value > d->max ? d->max : w.value);
			if (d->kind != strisy::ParamKind::continuous) v = (float)(int)(v + (v >= 0 ? 0.5f : -0.5f));
			*p = v;
			if (w.target == 0) engine_param_changed(engine, w.index);
			else strisy::EngineParams<fx::Engine>::changed(fx_engine, w.index);
		}
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

	fx::init(fx_engine, SAMPLINGFREQ);

	// defaults as the Faust sliders: accelerometer at rest, pedal 3/16, note 69
	controls_g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int v = 0; v < VOICECOUNT; v++) controls_vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};

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
		fx::process(fx_engine, output0, out_l, out_r);
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
			const strisy::ParamDesc* d = engine_param_desc(k, i);
			const float* p = engine_param_ptr(k, i);
			chprintf(out, "%d %s %s %08lx %08lx %08lx %d\r\n", i, d->name, d->group,
			    float_bits(d->min), float_bits(d->max), float_bits(p ? *p : 0.0f), static_cast<int>(d->kind));
		}
	} else {
		const int count = strisy::EngineParams<fx::Engine>::count;
		chprintf(out, "params 1 fx %d\r\n", count);
		for (int i = 0; i < count; i++) {
			const strisy::ParamDesc* d = strisy::EngineParams<fx::Engine>::desc(i);
			const float* p = strisy::EngineParams<fx::Engine>::ptr(fx_engine, i);
			chprintf(out, "%d %s %s %08lx %08lx %08lx %d\r\n", i, d->name, d->group,
			    float_bits(d->min), float_bits(d->max), float_bits(p ? *p : 0.0f), static_cast<int>(d->kind));
		}
	}
}

int synth_param_set(int target, int index, float value) {
	if (target < 0 || target > 1 || index < 0) return 0;
	const int count = target == 0 ? engine_param_count(entries[voice_request].engine) : strisy::EngineParams<fx::Engine>::count;
	if (index >= count) return 0;
	if (param_head - param_tail >= (uint32_t)PARAM_QUEUE) return 0;
	param_queue[param_head % PARAM_QUEUE] = {(uint8_t)target, (uint8_t)index, value};
	param_head = param_head + 1;
	return 1;
}
