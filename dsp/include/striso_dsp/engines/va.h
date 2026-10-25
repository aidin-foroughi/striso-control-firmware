// The va engine (voice_va.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::va.
#ifndef STRISO_DSP_ENGINES_VA_H_
#define STRISO_DSP_ENGINES_VA_H_

#include "../voice_va.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct va {
	STRISO_ENGINE_FUNCTIONS(va)
	STRISO_ENGINE_PRESETS(va)
	using FastMemory = NoMemory;
	using LargeMemory = NoMemory;
	static constexpr const char* name = "va";
	static constexpr const char* summary = "virtual analog: saw/pulse/sub oscillators, sync, ladder-style filter, envelopes (8 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_shake | in_rotation | in_pedal;
	static constexpr Budget budget = {::va::MAX_VOICES, 24000, 10.0f};
	// the hero parameters (seeded from Striso Live's hero.dart; the contract suite checks the names)
	static constexpr Heroes heroes = {{"cut_base", "res", "cut_env", "drive", "saw2_detune", "sub", "release", "vibrato"}};
	static constexpr PresetHeroes preset_heroes[] = {
	    {"pad", {{"cut_base", "cut_env", "release", "saw2_detune", "drift", "res", "sub", "vibrato"}}},
	    {"bass", {{"cut_base", "res", "cut_env", "drive", "sub", "acc_level", "acc_cutoff", "release"}}},
	    {"acid", {{"cut_base", "res", "cut_env", "drive", "acc_cutoff", "acc_level", "acc_decay", "release"}}},
	};
};
} // namespace traits

namespace engines {
using va = Adapter<traits::va>;
} // namespace engines
} // namespace striso::dsp

#endif
