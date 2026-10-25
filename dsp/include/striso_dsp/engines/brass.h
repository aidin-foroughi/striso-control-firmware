// The brass engine (voice_brass.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::brass.
#ifndef STRISO_DSP_ENGINES_BRASS_H_
#define STRISO_DSP_ENGINES_BRASS_H_

#include "../voice_brass.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct brass {
	STRISO_ENGINE_FUNCTIONS(brass)
	STRISO_ENGINE_PRESETS(brass)
	using FastMemory = NoMemory;
	using LargeMemory = ::brass::LargeMemory;
	// init() writes every byte of both regions (reset of every voice): no zeroing on a switch (engine.h)
	static constexpr bool init_writes_memory = true;
	static constexpr const char* name = "brass";
	static constexpr const char* summary = "brass: a lip source and a physical bore, bell, mutes (9 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_shake | in_pedal;
	static constexpr Budget budget = {::brass::MAX_VOICES, 30000, 2.0f};
	// the hero parameters (seeded from Striso Live's hero.dart; the contract suite checks the names)
	static constexpr Heroes heroes = {{"brassiness", "lip_loud", "tone_hz", "breath", "mute", "release", "vibrato", "pair"}};
	static constexpr PresetHeroes preset_heroes[] = {
	    {"harmon", {{"mute", "mute_bp", "mute_hz", "wah", "brassiness", "tone_hz", "vibrato", "release"}}},
	    {"plunger", {{"mute", "mute_lp", "mute_hz", "wah", "brassiness", "tone_hz", "vibrato", "release"}}},
	    {"section", {{"pair", "detune", "brassiness", "lip_loud", "tone_hz", "breath", "vibrato", "release"}}},
	};
};
} // namespace traits

namespace engines {
using brass = Adapter<traits::brass>;
} // namespace engines
} // namespace striso::dsp

#endif
