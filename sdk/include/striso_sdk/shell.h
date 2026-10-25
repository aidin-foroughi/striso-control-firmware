// The host shell: what a plugin (VST3, CLAP, AU) or a standalone app wraps. One engine and the
// master effects, played by MIDI/MPE (midi_voices.h: the board's own mapping) or directly by key
// signals, rendering any number of frames per call (the engines run 32-sample blocks, rendered
// when the previous one is used up). MIDI is taken every 2 ms, as on the board: a message takes
// effect at the next 2 ms step. Not real-time safe in its constructor and set_engine();
// process() and midi() don't allocate.
//
//   Shell s(*striso::sdk::find_engine("va"), 48000);
//   s.set_preset(5);
//   s.midi(0x90, 60, 100);           // MIDI in (applied at the next block)
//   s.process(left, right, frames);  // stereo out, about +-4 full scale (x0.25 for unity)
#ifndef STRISO_SDK_SHELL_H_
#define STRISO_SDK_SHELL_H_

#include <cstdint>
#include <vector>

#include "striso_sdk/engines.h"
#include "striso_sdk/midi_voices.h"

namespace striso::sdk {

class Shell {
public:
	// voices: the engine's voices (at most its budget's max_voices); fx: the master effects on
	Shell(const EngineEntry& engine, int sample_rate, int voices = 10, bool fx = true);

	void set_engine(const EngineEntry& engine); // a new engine, from silence (MIDI state kept)
	[[nodiscard]] Engine& engine() { return engine_; }
	[[nodiscard]] Effect& effects() { return fx_; }
	void set_fx(bool on) { fx_on_ = on; }
	void set_preset(int i) { engine_.set_preset(i); }

	// MIDI in (midi_voices.h); takes effect at the next 2 ms step
	void midi(const uint8_t* msg, int len) { midi_.message(msg, len); }
	void midi(uint8_t status, uint8_t d1, uint8_t d2) { midi_.message(status, d1, d2); }

	// or the key signals directly (a Striso over USB, OSC, a test): from now on these, not MIDI;
	// gate, strike and release follow from them (dsp::track_key)
	void set_controls(const VoiceControls* vc, const GlobalControls& g);

	// frames of stereo output
	void process(float* left, float* right, int frames);

	// the controls the engine got in the last block
	[[nodiscard]] const VoiceControls* controls() const { return vc_.data(); }
	[[nodiscard]] const GlobalControls& globals() const { return g_; }

	[[nodiscard]] int sample_rate() const { return sr_; }
	[[nodiscard]] int voices() const { return nv_; }

private:
	void render_block();

	Engine engine_;
	Effect fx_;
	MidiVoices midi_;
	int sr_, nv_;
	bool fx_on_;
	bool direct_ = false; // set_controls() was used
	std::vector<VoiceControls> vc_;
	GlobalControls g_;
	uint64_t frames_ = 0, steps_ = 0; // frames rendered, MIDI steps run
	float mono_[dsp::BLOCK], l_[dsp::BLOCK], r_[dsp::BLOCK];
	int pos_ = dsp::BLOCK; // next frame of l_, r_ to hand out
};

} // namespace striso::sdk

#endif // STRISO_SDK_SHELL_H_
