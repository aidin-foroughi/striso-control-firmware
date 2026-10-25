// The modal engine (voice_modal.h) as a striso::dsp::VoiceEngine: striso::dsp::engines::modal.
#ifndef STRISO_DSP_ENGINES_MODAL_H_
#define STRISO_DSP_ENGINES_MODAL_H_

#include "../voice_modal.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct modal {
	STRISO_ENGINE_FUNCTIONS(modal)
	STRISO_ENGINE_PRESETS(modal)
	using FastMemory = NoMemory;
	using LargeMemory = NoMemory;
	static constexpr const char* name = "modal";
	static constexpr const char* summary = "tuned percussion: banks of damped resonators, struck or rubbed (6 presets)";
	static constexpr uint32_t inputs = in_pressure | in_velocity | in_tilt_y | in_rotation | in_pedal;
	static constexpr Budget budget = {::modal::MAX_VOICES, 18000, 5.0f};
};
} // namespace traits

namespace engines {
using modal = Adapter<traits::modal>;
} // namespace engines
} // namespace striso::dsp

#endif
