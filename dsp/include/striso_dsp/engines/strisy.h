// The strisy engine (strisy.h) as a awva::VoiceEngine: awva::engines::strisy.
#ifndef STRISO_DSP_ENGINES_STRISY_H_
#define STRISO_DSP_ENGINES_STRISY_H_

#include "../strisy.h"
#include "awva/adapter.h"

namespace awva {
namespace traits {
struct strisy {
	AWVA_ENGINE_FUNCTIONS(strisy)
	using FastMemory = NoMemory;
	using LargeMemory = NoMemory;
	static constexpr const char* name = "strisy";
	static constexpr const char* summary = "the original Striso synth (synth.dsp), hand-ported: wavetable noise oscillator, filters, motion body";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_x | in_tilt_y | in_shake | in_accel | in_rotation | in_pedal;
	static constexpr Budget budget = {::strisy::MAX_VOICES, 12000, 2.0f};
};
} // namespace traits

namespace engines {
using strisy = Adapter<traits::strisy>;
} // namespace engines
} // namespace awva

#endif
