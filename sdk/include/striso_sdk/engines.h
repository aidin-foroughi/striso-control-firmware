// The engines and the effects for programs on the PC: each striso::dsp::VoiceEngine behind one
// table of functions (EngineEntry), so a program picks an engine by name at run time and links
// only the engine libraries it wants (striso_engine_<name>, sdk/CMakeLists.txt). An Engine
// instance holds the engine object and its memory regions on the heap.
#ifndef STRISO_SDK_ENGINES_H_
#define STRISO_SDK_ENGINES_H_

#include <span>
#include <string_view>

#include "striso_dsp/engine.h"

namespace striso::sdk {

using dsp::EffectInfo;
using dsp::EngineInfo;
using dsp::GlobalControls;
using dsp::ParamDesc;
using dsp::VoiceControls;

// one engine (sdk/src/engine_entry.h makes it from the engine's type)
struct EngineEntry {
	const EngineInfo* info;
	const char* header; // the engine's own header in dsp/include/striso_dsp, e.g. "voice_va.h"
	void* (*create)();  // a zero-filled engine and its memory
	void (*destroy)(void*);
	void (*init)(void*, int sample_rate, int voices);
	void (*reset)(void*);
	void (*process)(void*, const VoiceControls*, const GlobalControls&, float* const* out);
	int (*active_voices)(const void*);
	int (*voices)(const void*);
	const char* (*preset_name)(int);
	void (*set_preset)(void*, int);
	int (*preset)(const void*);
	const ParamDesc* (*param_desc)(int);
	float* (*param)(void*, int);
	bool (*set_param)(void*, int, float);
};

struct EffectEntry {
	const EffectInfo* info;
	void* (*create)();
	void (*destroy)(void*);
	void (*init)(void*, int sample_rate);
	void (*reset)(void*);
	void (*process)(void*, const float* const* in, float* const* out);
	const ParamDesc* (*param_desc)(int);
	float* (*param)(void*, int);
	bool (*set_param)(void*, int, float);
};

// the engines linked into this program, in the order of dsp's engine list (strisy first)
[[nodiscard]] std::span<const EngineEntry* const> engines();
[[nodiscard]] const EngineEntry* find_engine(std::string_view name);
// the master effects (striso_effect_fx)
[[nodiscard]] const EffectEntry& fx_effect();

// preset i of an entry by number or by name ("5", "pad"): -1 if there is none
[[nodiscard]] int find_preset(const EngineEntry& e, std::string_view number_or_name);

// An engine instance: the engine object and its memory, owned.
class Engine {
public:
	explicit Engine(const EngineEntry& entry) :
	    e_(&entry), p_(entry.create()) {}
	~Engine() {
		if (p_) e_->destroy(p_);
	}
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;
	Engine(Engine&& o) noexcept :
	    e_(o.e_), p_(o.p_) { o.p_ = nullptr; }
	Engine& operator=(Engine&& o) noexcept {
		if (this != &o) {
			if (p_) e_->destroy(p_);
			e_ = o.e_;
			p_ = o.p_;
			o.p_ = nullptr;
		}
		return *this;
	}

	[[nodiscard]] const EngineEntry& entry() const { return *e_; }
	[[nodiscard]] const EngineInfo& info() const { return *e_->info; }
	void init(int sample_rate, int voices) { e_->init(p_, sample_rate, voices); }
	void reset() { e_->reset(p_); }
	void process(const VoiceControls* vc, const GlobalControls& g, float* const* out) { e_->process(p_, vc, g, out); }
	[[nodiscard]] int active_voices() const { return e_->active_voices(p_); }
	[[nodiscard]] int voices() const { return e_->voices(p_); }
	[[nodiscard]] int num_presets() const { return e_->info->num_presets; }
	[[nodiscard]] const char* preset_name(int i) const { return e_->preset_name(i); }
	void set_preset(int i) { e_->set_preset(p_, i); }
	[[nodiscard]] int preset() const { return e_->preset(p_); }
	[[nodiscard]] int num_params() const { return e_->info->num_params; }
	[[nodiscard]] const ParamDesc* param_desc(int i) const { return e_->param_desc(i); }
	[[nodiscard]] float* param(int i) { return e_->param(p_, i); }
	bool set_param(int i, float v) { return e_->set_param(p_, i, v); }
	// the hero parameters of the current preset (its own, or the engine's)
	[[nodiscard]] const dsp::Heroes& heroes() const { return dsp::heroes_of(*e_->info, preset() >= 0 ? preset_name(preset()) : nullptr); }

private:
	const EngineEntry* e_;
	void* p_;
};

// An effect instance.
class Effect {
public:
	explicit Effect(const EffectEntry& entry) :
	    e_(&entry), p_(entry.create()) {}
	~Effect() {
		if (p_) e_->destroy(p_);
	}
	Effect(const Effect&) = delete;
	Effect& operator=(const Effect&) = delete;

	[[nodiscard]] const EffectInfo& info() const { return *e_->info; }
	void init(int sample_rate) { e_->init(p_, sample_rate); }
	void reset() { e_->reset(p_); }
	void process(const float* const* in, float* const* out) { e_->process(p_, in, out); }
	[[nodiscard]] int num_params() const { return e_->info->num_params; }
	[[nodiscard]] const ParamDesc* param_desc(int i) const { return e_->param_desc(i); }
	[[nodiscard]] float* param(int i) { return e_->param(p_, i); }
	bool set_param(int i, float v) { return e_->set_param(p_, i, v); }

private:
	const EffectEntry* e_;
	void* p_;
};

} // namespace striso::sdk

#endif // STRISO_SDK_ENGINES_H_
