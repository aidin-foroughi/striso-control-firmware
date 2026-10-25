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
	static constexpr const char* name = "brass";
	static constexpr const char* summary = "brass: a lip source and a physical bore, bell, mutes (9 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_shake | in_pedal;
	static constexpr Budget budget = {::brass::MAX_VOICES, 30000, 2.0f};
};
} // namespace traits

namespace engines {
using brass = Adapter<traits::brass>;
} // namespace engines
} // namespace striso::dsp

#endif
