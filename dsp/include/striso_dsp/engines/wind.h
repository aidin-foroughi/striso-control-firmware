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
	// the hero parameters (seeded from Striso Live's hero.dart; the contract suite checks the names)
	static constexpr Heroes heroes = {{"reed_open", "attack", "bright", "breath", "growl", "vib", "body", "tongue"}};
	static constexpr PresetHeroes preset_heroes[] = {
	    {"flute", {{"jet", "jet_off", "breath", "turb", "bright", "attack", "vib", "body"}}},
	};
};
} // namespace traits

namespace engines {
using wind = Adapter<traits::wind>;
} // namespace engines
} // namespace striso::dsp

#endif
