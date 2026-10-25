// The trumpet engine (voice_trumpet.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::trumpet.
// Its bore delay lines (80 KB): 64 KB FastMemory, 16 KB LargeMemory.
#ifndef STRISO_DSP_ENGINES_TRUMPET_H_
#define STRISO_DSP_ENGINES_TRUMPET_H_

#include "../voice_trumpet.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct trumpet {
	STRISO_ENGINE_FUNCTIONS(trumpet)
	STRISO_ENGINE_PRESETS(trumpet)
	using FastMemory = ::trumpet::FastMemory;
	using LargeMemory = ::trumpet::LargeMemory;
	// init() writes every byte of both regions (reset of every voice): no zeroing on a switch (engine.h)
	static constexpr bool init_writes_memory = true;
	static constexpr const char* name = "trumpet";
	static constexpr const char* summary = "physical trumpet: lips, mouthpiece and bore, played like the brass trumpet (1 preset)";
	// pres the breath, vpres the attack, but_y the lips (lip_tilt); acc_abs and the pedal
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_shake | in_pedal;
	static constexpr Budget budget = {::trumpet::MAX_VOICES, 44000, 2.0f}; // profile.sh 42288; silent 0.25 s after the release
	// the hero parameters (seeded from Striso Live's hero.dart; the contract suite checks the names)
	// the new model's: brassiness, the bell and the radiation's brightness, the tongue, the loudest
	// breath, the breath noise, the lips under tilt, the release (no presets to override them)
	static constexpr Heroes heroes = {{"brassiness", "bell_hz", "tone_hz", "tongue", "p_loud", "breath", "lip_tilt", "release"}};
};
} // namespace traits

namespace engines {
using trumpet = Adapter<traits::trumpet>;
} // namespace engines
} // namespace striso::dsp

#endif
