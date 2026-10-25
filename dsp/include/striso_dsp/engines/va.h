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
};
} // namespace traits

namespace engines {
using va = Adapter<traits::va>;
} // namespace engines
} // namespace striso::dsp

#endif
