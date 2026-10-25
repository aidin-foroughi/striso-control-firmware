// The sound engine contract (doc/engines.md): what every voice engine and every effect offers its
// host, the board's synth thread (src/synth/synth_strisy.cpp) or a program on the PC (sdk/: the
// renderer, the tests, a plugin). Namespace striso::dsp. No HAL, no ChibiOS, no heap, no globals:
// an engine is a plain-data object plus the memory regions its host gives it.
//
//   VoiceEngine   a polyphonic sound engine: per-voice controls in, a block of audio out
//   Effect        an audio effect: a block of audio in, a block out (fx.h, the master effects)
//
// Engines are written in the shape every voice_<name>.h has (namespace <name>: struct Engine,
// init, process, active_voices, the optional presets and live parameters); adapter.h turns that
// shape into a VoiceEngine, with a traits struct per engine in engines/<name>.h that declares
// what the engine reads, what it costs and what memory it needs.
#ifndef STRISO_DSP_ENGINE_H_
#define STRISO_DSP_ENGINE_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "controls.h"
#include "params.h"

namespace striso::dsp {

inline constexpr int BLOCK = strisy::BLOCK; // samples per process() call

using strisy::GlobalControls;
using strisy::ParamDesc;
using strisy::ParamKind;

// The controls of one voice, as a key or a note gives them. The first five are the board's key
// signals (strisy::VoiceControls: controls.h has their ranges); gate, strike and release are the
// same key seen as a note (a MIDI host has them directly; on the board track_key() derives them).
struct VoiceControls {
	float note;    // pitch in semitones, MIDI numbering (69 = A4), fractional
	float pres;    // pressure, 0 (released) .. 1 (firm), up to ~1.5
	float vpres;   // pressure velocity, about -1 .. 1: > 0 striking, < 0 releasing
	float but_x;   // tilt along the row, -1 .. 1
	float but_y;   // tilt across the row, -1 .. 1
	float gate;    // 1 while the key is held (pres > 0), else 0
	float strike;  // velocity of the strike that opened the gate, 0 .. 1 (kept until the next)
	float release; // velocity of the release that closed it, 0 .. 1 (kept until the next)
};

// the key signals of a voice (what the engines written for the board read)
[[nodiscard]] constexpr strisy::VoiceControls key_controls(const VoiceControls& c) {
	return {c.note, c.pres, c.vpres, c.but_x, c.but_y};
}

// a voice at rest: note 69, no pressure, no tilt (what the board starts with)
inline constexpr VoiceControls voice_at_rest = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
// the instrument at rest: accelerometer 1 g down, no rotation, pedal 3/16 (the board's default)
inline constexpr GlobalControls globals_at_rest = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};

// Gate, strike and release from the key signals, once per block after they were written: the
// gate opens when pres rises above 0 and closes when it returns to 0; strike is the highest vpres
// seen while the gate is open (the key's speed going down), release the highest -vpres since it
// closed. The board's keys only give pres and vpres; a MIDI host sets all three itself.
constexpr void track_key(VoiceControls& c) {
	const auto clip = [](float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); };
	if (c.pres > 0.0f) {
		if (c.gate == 0.0f) {
			c.gate = 1.0f;
			c.strike = clip(c.vpres);
		} else if (c.vpres > c.strike) {
			c.strike = clip(c.vpres);
		}
	} else if (c.gate != 0.0f) {
		c.gate = 0.0f;
		c.release = clip(-c.vpres);
	} else if (-c.vpres > c.release) {
		c.release = clip(-c.vpres);
	}
}

// ---------------------------------------------------------------------------------------------
// What an engine declares about itself

// The inputs an engine reads (EngineInfo::inputs). An input it doesn't declare makes no
// difference to its output (the contract tests check this). The pitch is always read.
enum Input : uint32_t {
	in_pressure = 1u << 0,    // pres
	in_velocity = 1u << 1,    // vpres
	in_tilt_x = 1u << 2,      // but_x (the pitch bend is already in note)
	in_tilt_y = 1u << 3,      // but_y
	in_note_events = 1u << 4, // gate, strike, release
	in_shake = 1u << 5,       // acc_abs
	in_accel = 1u << 6,       // acc_x, acc_y, acc_z
	in_rotation = 1u << 7,    // rot_x, rot_y, rot_z
	in_pedal = 1u << 8,       // pedal
};

// Where a host puts an engine's memory. The engine object itself is always Placement::tcm.
enum class Placement : uint8_t {
	tcm,   // tightly coupled, zero wait states: the hot state (the board: DTCM, 128 KB shared by all)
	fast,  // FastMemory: large, touched every sample (the board: AHB SRAM1/2, 256 KB)
	large, // LargeMemory: large, any RAM (the board: AXI SRAM, 512 KB)
};

// What running the engine costs (hints for hosts; budgets.max_voices is a hard limit)
struct Budget {
	int max_voices;           // voices it can run
	uint32_t instr_per_block; // worst preset, QEMU instructions per block (10 voices, the default
	                          // performance: tools/target_render/profile.sh); the board runs a block
	                          // in 480 MHz / 48828 Hz * 32 = 314k cycles
	float tail_seconds;       // after the last release it is silent and idle within this time
};

// a region an engine doesn't need
struct NoMemory {};

// The hero parameters: the live parameters that shape the sound most, by name, in display order
// (an app's main knobs; unused slots nullptr). An engine has its own list and may override it for
// presets whose character lives elsewhere.
inline constexpr int MAX_HEROES = 8;
struct Heroes {
	const char* name[MAX_HEROES];
};
struct PresetHeroes {
	const char* preset; // the preset's name (preset_name)
	Heroes heroes;
};

struct EngineInfo {
	const char* name;    // the firmware's name for it ('v' console command), e.g. "va"
	const char* summary; // one line for people
	int channels;        // 1: mono, 2: stereo out
	uint32_t inputs;     // Input bits
	Budget budget;
	size_t state_bytes;                // the engine object (Placement::tcm)
	size_t fast_bytes;                 // its FastMemory (0: none)
	size_t large_bytes;                // its LargeMemory (0: none)
	int num_presets;                   // 0: no presets
	int num_params;                    // live parameters
	Heroes heroes;                     // the engine's hero parameters
	const PresetHeroes* preset_heroes; // presets with their own (num_preset_heroes of them)
	int num_preset_heroes;
};

// the hero parameters of a preset, by its name (the engine's own if it has none, or for nullptr)
[[nodiscard]] constexpr const Heroes& heroes_of(const EngineInfo& in, const char* preset) {
	for (int i = 0; preset && i < in.num_preset_heroes; i++) {
		const char *a = in.preset_heroes[i].preset, *b = preset;
		while (*a && *a == *b) a++, b++;
		if (*a == *b) return in.preset_heroes[i].heroes;
	}
	return in.heroes;
}

struct EffectInfo {
	const char* name;
	const char* summary;
	int inputs, outputs; // channels
	uint32_t instr_per_block;
	size_t state_bytes, fast_bytes, large_bytes;
	int num_params;
	Heroes heroes;
};

// The memory of an engine as one object, for hosts that don't place the regions separately (the
// PC); the board puts FastMemory and LargeMemory in different RAMs (engine_set.h).
template <class E> struct Memory {
	[[no_unique_address]] typename E::FastMemory fast;
	[[no_unique_address]] typename E::LargeMemory large;
};

// plain data: zero-filled storage is a valid engine before init(); nothing runs at startup
template <class T>
concept PlainData = std::is_trivially_default_constructible_v<T> && std::is_trivially_destructible_v<T>;

// ---------------------------------------------------------------------------------------------
// The contracts

// A polyphonic sound engine.
//   init(fast, large, sample_rate, voices)   start from silence; the regions stay the engine's
//                                             until the next init (voices <= budget.max_voices)
//   reset()             back to silence, keeping the preset and the live parameter values
//   process(vc, g, out) one block: vc[voices()] controls, g the globals, out[0..channels) BLOCK
//                       samples each (full scale about +-4: the board's volume is x0.25)
//   active_voices()     voices sounding now; voices() the count init() set
//   presets: num_presets() (static), preset_name(i), set_preset(i) (any time, between blocks),
//            preset() (-1 without presets)
//   live parameters: param_desc(i) (nullptr past the last), param(i) the live value,
//            set_param(i, v) a write by write_param()'s rule (false: not written)
template <class E>
concept VoiceEngine = PlainData<E> && PlainData<typename E::FastMemory> && PlainData<typename E::LargeMemory> &&
    requires(E& e, const E& ce, typename E::FastMemory& f, typename E::LargeMemory& l,
        const VoiceControls* vc, const GlobalControls& g, float* const* out, int i, float v) {
	    { E::info } -> std::convertible_to<const EngineInfo&>;
	    { e.init(f, l, i, i) } -> std::same_as<void>;
	    { e.reset() } -> std::same_as<void>;
	    { e.process(vc, g, out) } -> std::same_as<void>;
	    { ce.active_voices() } -> std::same_as<int>;
	    { ce.voices() } -> std::same_as<int>;
	    { E::preset_name(i) } -> std::same_as<const char*>;
	    { e.set_preset(i) } -> std::same_as<void>;
	    { ce.preset() } -> std::same_as<int>;
	    { E::param_desc(i) } -> std::same_as<const ParamDesc*>;
	    { e.param(i) } -> std::same_as<float*>;
	    { e.set_param(i, v) } -> std::same_as<bool>;
    };

// An audio effect: init(fast, large, sample_rate), reset(), process(in, out) with
// in[0..inputs) and out[0..outputs) BLOCK samples each, live parameters as a VoiceEngine.
template <class F>
concept Effect = PlainData<F> && PlainData<typename F::FastMemory> && PlainData<typename F::LargeMemory> &&
    requires(F& f, typename F::FastMemory& fm, typename F::LargeMemory& lm, const float* const* in,
        float* const* out, int i, float v) {
	    { F::info } -> std::convertible_to<const EffectInfo&>;
	    { f.init(fm, lm, i) } -> std::same_as<void>;
	    { f.reset() } -> std::same_as<void>;
	    { f.process(in, out) } -> std::same_as<void>;
	    { F::param_desc(i) } -> std::same_as<const ParamDesc*>;
	    { f.param(i) } -> std::same_as<float*>;
	    { f.set_param(i, v) } -> std::same_as<bool>;
    };

} // namespace striso::dsp

#endif // STRISO_DSP_ENGINE_H_
