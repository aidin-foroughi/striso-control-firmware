// The trumpet engine (voice_trumpet.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::trumpet.
// (voice_trumpet.h still keeps its delay lines in static arrays, cyl_f, cyl_b, room_buf: two
// instances share them. Its rewrite moves them into trumpet::FastMemory, docs/engines.md.)
#ifndef STRISO_DSP_ENGINES_TRUMPET_H_
#define STRISO_DSP_ENGINES_TRUMPET_H_

#include "../voice_trumpet.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct trumpet {
	STRISO_ENGINE_FUNCTIONS(trumpet)
	STRISO_ENGINE_PRESETS(trumpet)
	using FastMemory = NoMemory;
	using LargeMemory = NoMemory;
	static constexpr const char* name = "trumpet";
	static constexpr const char* summary = "trumpet family: lips locked to a physical bore at 96 kHz, valves, mutes (9 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_x | in_tilt_y | in_shake | in_pedal;
	static constexpr Budget budget = {::trumpet::MAX_VOICES, 94000, 2.0f};
};
} // namespace traits

namespace engines {
using trumpet = Adapter<traits::trumpet>;
} // namespace engines
} // namespace striso::dsp

#endif
