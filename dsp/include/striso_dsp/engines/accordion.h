// The accordion engine (voice_accordion.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::accordion.
#ifndef STRISO_DSP_ENGINES_ACCORDION_H_
#define STRISO_DSP_ENGINES_ACCORDION_H_

#include "../voice_accordion.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct accordion {
	STRISO_ENGINE_FUNCTIONS(accordion)
	STRISO_ENGINE_PRESETS(accordion)
	using FastMemory = NoMemory;
	using LargeMemory = NoMemory;
	static constexpr const char* name = "accordion";
	static constexpr const char* summary =
	    "free-reed accordion: registers, musette tuning, reed chambers, cassotto, a shared bellows (9 presets)";
	// pres drives the bellows, but_y the tilt expression; acc_abs the bellows shake, the pedal its reversal
	static constexpr uint32_t inputs = in_pressure | in_tilt_y | in_shake | in_pedal;
	static constexpr Budget budget = {::accordion::MAX_VOICES, 32000, 1.0f}; // master (preset 6) 31556; silent 0.67 s after the release
	// the hero parameters: how it plays (the musette's beating, the bellows, the pressure response, the
	// reeds' speech, the cassotto, the chamber's brightness, the pallet bend, the tilt expression)
	static constexpr Heroes heroes = {{"beat", "bellows", "curve", "speak_ms", "cassotto", "ch_hz", "bend", "tilt"}};
};
} // namespace traits

namespace engines {
using accordion = Adapter<traits::accordion>;
} // namespace engines
} // namespace striso::dsp

#endif
