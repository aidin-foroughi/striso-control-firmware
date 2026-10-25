// The ep engine (voice_ep.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::ep.
#ifndef STRISO_DSP_ENGINES_EP_H_
#define STRISO_DSP_ENGINES_EP_H_

#include "../voice_ep.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct ep {
	STRISO_ENGINE_FUNCTIONS(ep)
	STRISO_ENGINE_PRESETS(ep)
	using FastMemory = NoMemory;
	using LargeMemory = NoMemory;
	static constexpr const char* name = "ep";
	static constexpr const char* summary = "electric pianos: modal tines and reeds, pickups, damper (14 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_pedal;
	static constexpr Budget budget = {::ep::MAX_VOICES, 12500, 3.0f};
};
} // namespace traits

namespace engines {
using ep = Adapter<traits::ep>;
} // namespace engines
} // namespace striso::dsp

#endif
