// The host shell (shell.h).
#include "striso_sdk/shell.h"

#include <algorithm>

namespace striso::sdk {

Shell::Shell(const EngineEntry& engine, int sample_rate, int voices, bool fx) :
    engine_(engine), fx_(fx_effect()), midi_(std::min(voices, engine.info->budget.max_voices)), sr_(sample_rate),
    nv_(std::min(voices, engine.info->budget.max_voices)), fx_on_(fx), vc_(static_cast<size_t>(nv_), dsp::voice_at_rest),
    g_(dsp::globals_at_rest) {
	engine_.init(sr_, nv_);
	fx_.init(sr_);
	std::fill(std::begin(l_), std::end(l_), 0.0f);
	std::fill(std::begin(r_), std::end(r_), 0.0f);
}

void Shell::set_engine(const EngineEntry& engine) {
	engine_ = Engine(engine);
	nv_ = std::min(nv_, engine.info->budget.max_voices);
	engine_.init(sr_, nv_);
}

void Shell::set_controls(const VoiceControls* vc, const GlobalControls& g) {
	direct_ = true;
	for (int v = 0; v < nv_; v++) vc_[static_cast<size_t>(v)] = vc[v];
	g_ = g;
}

void Shell::render_block() {
	if (!direct_) {
		// the strikes fade every 2 ms
		ticks_ += static_cast<double>(dsp::BLOCK) / sr_ / MidiIn::TICK_SECONDS;
		for (; ticks_ >= 1.0; ticks_ -= 1.0) midi_.tick();
		for (int v = 0; v < nv_; v++) vc_[static_cast<size_t>(v)] = midi_.voices()[v];
		g_ = midi_.globals();
	} else {
		for (VoiceControls& c : vc_) dsp::track_key(c);
	}
	float* const eo[1] = {mono_};
	engine_.process(vc_.data(), g_, eo);
	if (fx_on_) {
		const float* const in[1] = {mono_};
		float* const out[2] = {l_, r_};
		fx_.process(in, out);
	} else {
		std::copy(std::begin(mono_), std::end(mono_), l_);
		std::copy(std::begin(mono_), std::end(mono_), r_);
	}
	pos_ = 0;
}

void Shell::process(float* left, float* right, int frames) {
	for (int n = 0; n < frames; n++) {
		if (pos_ == dsp::BLOCK) render_block();
		left[n] = l_[pos_];
		right[n] = r_[pos_];
		pos_++;
	}
}

} // namespace striso::sdk
