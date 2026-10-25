// The bowed engine (voice_bowed.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::bowed.
#ifndef STRISO_DSP_ENGINES_BOWED_H_
#define STRISO_DSP_ENGINES_BOWED_H_

#include "../voice_bowed.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct bowed {
	STRISO_ENGINE_FUNCTIONS(bowed)
	STRISO_ENGINE_PRESETS(bowed)
	using FastMemory = ::bowed::FastMemory;
	using LargeMemory = ::bowed::LargeMemory;
	// init() writes every byte of both regions (reset of every voice): no zeroing on a switch (engine.h)
	static constexpr bool init_writes_memory = true;
	static constexpr const char* name = "bowed";
	static constexpr const char* summary = "bowed strings: friction-driven waveguide strings, modal body, section (9 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_shake | in_rotation | in_pedal;
	static constexpr Budget budget = {::bowed::MAX_VOICES, 32000, 2.0f};
	// the hero parameters (seeded from Striso Live's hero.dart; the contract suite checks the names)
	static constexpr Heroes heroes = {{"force_hi", "pos", "rosin", "bright", "body_amt", "damp", "vibrato", "ensemble"}};
	static constexpr PresetHeroes preset_heroes[] = {
	    {"pizzicato", {{"pizz_decay", "pizz_bright", "pos", "bright", "body_amt", "damp", "rel", "vibrato"}}},
	    {"section", {{"ensemble", "ens_detune", "ens_spread", "force_hi", "pos", "bright", "body_amt", "vibrato"}}},
	};
};
} // namespace traits

namespace engines {
using bowed = Adapter<traits::bowed>;
} // namespace engines
} // namespace striso::dsp

#endif
