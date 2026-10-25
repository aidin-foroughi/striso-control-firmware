// The master effects (fx.h) as a striso::dsp::Effect: striso::dsp::effects::fx (mono in, stereo out).
#ifndef STRISO_DSP_EFFECTS_FX_H_
#define STRISO_DSP_EFFECTS_FX_H_

#include "../fx.h"
#include "../adapter.h"

namespace striso::dsp {
namespace traits {
struct fx {
	using Engine = ::fx::Engine;
	using FastMemory = ::fx::FastMemory;
	using LargeMemory = ::fx::LargeMemory;
	static constexpr const char* name = "fx";
	static constexpr const char* summary = "master effects: drive, phaser, chorus, tremolo, delay, plate reverb, limiter";
	static constexpr int inputs = 1, outputs = 2;
	static constexpr uint32_t instr_per_block = 14200; // defaults (all stages on: about 28k)
	static void init(Engine& e, int sample_rate) { ::fx::init(e, sample_rate); }
	static void process(Engine& e, const float* const* in, float* const* out) { ::fx::process(e, in[0], out[0], out[1]); }
};
} // namespace traits

namespace effects {
using fx = EffectAdapter<traits::fx>;
} // namespace effects
} // namespace striso::dsp

#endif
