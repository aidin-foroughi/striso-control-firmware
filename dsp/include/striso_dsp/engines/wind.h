// The wind engine (voice_wind.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::wind.
#ifndef STRISO_DSP_ENGINES_WIND_H_
#define STRISO_DSP_ENGINES_WIND_H_

#include "../voice_wind.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct wind {
	STRISO_ENGINE_FUNCTIONS(wind)
	STRISO_ENGINE_PRESETS(wind)
	using FastMemory = NoMemory;
	using LargeMemory = ::wind::LargeMemory;
	static constexpr const char* name = "wind";
	static constexpr const char* summary = "woodwinds: reed and jet driven bores, cylindrical and conical (9 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_shake | in_pedal;
	static constexpr Budget budget = {::wind::MAX_VOICES, 20000, 2.0f};
};
} // namespace traits

namespace engines {
using wind = Adapter<traits::wind>;
} // namespace engines
} // namespace striso::dsp

#endif
