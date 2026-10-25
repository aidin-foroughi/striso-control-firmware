// Adapter<Traits>: a voice engine written in the voice_<name>.h shape as a striso::dsp::VoiceEngine,
// and EffectAdapter<Traits> for an effect in the fx.h shape. The engine header stays as it is; a
// traits struct (engines/<name>.h) names its functions and declares what the engine reads, costs
// and needs:
//
//   struct traits::<name> {
//       STRISO_ENGINE_FUNCTIONS(<name>)  // Engine, init(e, sr, voices), process(e, out), active_voices(e)
//       STRISO_ENGINE_PRESETS(<name>)    // (engines with presets) num_presets, preset_name, set_preset
//       using FastMemory = ...;          // the engine's regions, or NoMemory; Engine has a pointer
//       using LargeMemory = ...;         //   to each it uses: FastMemory* fast, LargeMemory* large
//       static constexpr const char* name = "<name>", *summary = "...";
//       static constexpr uint32_t inputs = in_pressure | ...;
//       static constexpr Budget budget = {max_voices, instructions per block, tail seconds};
//       static constexpr Heroes heroes = {{"cutoff", ...}};                        // optional, up to 8
//       static constexpr PresetHeroes preset_heroes[] = {{"pad", {{...}}}, ...};  // optional
//   };
//
// The engine's struct Engine has `strisy::GlobalControls g; strisy::VoiceControls vc[MAX_VOICES];
// int nvoices;` (or striso::dsp::VoiceControls vc[] to get gate, strike and release too), and,
// with presets, `int preset`. Live parameters come from strisy::EngineParams<Engine>.
#ifndef STRISO_DSP_ADAPTER_H_
#define STRISO_DSP_ADAPTER_H_

#include <cstddef>
#include <type_traits>

#include "engine.h"

#define STRISO_ENGINE_FUNCTIONS(ns) \
	using Engine = ::ns::Engine; \
	static_assert(std::is_same_v<::ns::real, float>, "the contract's samples are float"); \
	static void init(Engine& e, int sample_rate, int voices) { ::ns::init(e, sample_rate, voices); } \
	static void process(Engine& e, float* out) { ::ns::process(e, out); } \
	[[nodiscard]] static int active_voices(const Engine& e) { return ::ns::active_voices(e); }

#define STRISO_ENGINE_PRESETS(ns) \
	static constexpr int num_presets = ::ns::NUM_PRESETS; \
	[[nodiscard]] static const char* preset_name(int i) { return ::ns::preset_name(i); } \
	static void set_preset(Engine& e, int i) { ::ns::set_preset(e, i); }

namespace striso::dsp {

namespace detail {

template <class Native, class FastMemory, class LargeMemory>
inline void attach(Native& e, FastMemory& f, LargeMemory& l) {
	if constexpr (std::is_same_v<FastMemory, NoMemory>) (void)f;
	else e.fast = &f;
	if constexpr (std::is_same_v<LargeMemory, NoMemory>) (void)l;
	else e.large = &l;
}

// the live parameters of a native engine, saved and written back (reset() keeps them)
template <class Native> struct SavedParams {
	using P = strisy::EngineParams<Native>;
	float v[P::count > 0 ? P::count : 1];
	void save(Native& e) {
		for (int i = 0; i < P::count; i++) v[i] = *P::ptr(e, i);
	}
	void restore(Native& e) const {
		for (int i = 0; i < P::count; i++) {
			*P::ptr(e, i) = v[i];
			P::changed(e, i);
		}
	}
};

// the optional hero declarations of a traits struct
template <class T> constexpr Heroes heroes() {
	if constexpr (requires { T::heroes; }) return T::heroes;
	else return {};
}
template <class T> constexpr const PresetHeroes* preset_heroes() {
	if constexpr (requires { T::preset_heroes; }) return T::preset_heroes;
	else return nullptr;
}
template <class T> constexpr int num_preset_heroes() {
	if constexpr (requires { T::preset_heroes; }) return static_cast<int>(std::extent_v<decltype(T::preset_heroes)>);
	else return 0;
}

template <class T> constexpr int num_presets() {
	if constexpr (requires { T::num_presets; }) return T::num_presets;
	else return 0;
}

// the size of an adapter: the native engine and the sample rate (EngineInfo::state_bytes)
template <class Native> constexpr size_t state_bytes() {
	constexpr size_t a = alignof(Native) > alignof(int) ? alignof(Native) : alignof(int);
	return (sizeof(Native) + sizeof(int) + a - 1) / a * a;
}

} // namespace detail

template <class T> struct Adapter {
	using Traits = T;
	using Native = typename T::Engine;
	using FastMemory = typename T::FastMemory;
	using LargeMemory = typename T::LargeMemory;
	using Params = strisy::EngineParams<Native>;
	static constexpr bool has_presets = requires { T::num_presets; };
	static_assert(PlainData<Native>, "an engine is plain data (it lives in zero-filled DTCM, no constructors run)");


	Native e;
	int sample_rate;

	void init(FastMemory& f, LargeMemory& l, int sr, int voices) {
		detail::attach(e, f, l);
		T::init(e, sr, voices);
		sample_rate = sr;
	}

	void reset() {
		detail::SavedParams<Native> saved;
		saved.save(e);
		const int p = preset();
		T::init(e, sample_rate, e.nvoices);
		if constexpr (has_presets) T::set_preset(e, p);
		saved.restore(e);
	}

	void process(const VoiceControls* vc, const GlobalControls& g, float* const* out) {
		e.g = g;
		for (int v = 0; v < e.nvoices; v++) {
			if constexpr (std::is_same_v<std::remove_cvref_t<decltype(e.vc[0])>, VoiceControls>) e.vc[v] = vc[v];
			else e.vc[v] = key_controls(vc[v]);
		}
		T::process(e, out[0]);
	}

	[[nodiscard]] int active_voices() const { return T::active_voices(e); }
	[[nodiscard]] int voices() const { return e.nvoices; }

	[[nodiscard]] static constexpr int num_presets() { return detail::num_presets<T>(); }
	[[nodiscard]] static const char* preset_name(int i) {
		if constexpr (has_presets) return i >= 0 && i < T::num_presets ? T::preset_name(i) : nullptr;
		else return (void)i, nullptr;
	}
	void set_preset(int i) {
		if constexpr (has_presets) T::set_preset(e, i);
		else (void)i;
	}
	[[nodiscard]] int preset() const {
		if constexpr (has_presets) return e.preset;
		else return -1;
	}

	static constexpr int num_params = Params::count;
	[[nodiscard]] static const ParamDesc* param_desc(int i) { return Params::desc(i); }
	[[nodiscard]] float* param(int i) { return Params::ptr(e, i); }
	[[nodiscard]] bool set_param(int i, float v) {
		if (!strisy::write_param(Params::desc(i), Params::ptr(e, i), v)) return false;
		Params::changed(e, i);
		return true;
	}
	static constexpr EngineInfo info = {T::name, T::summary, 1, T::inputs, T::budget, detail::state_bytes<Native>(),
	    std::is_empty_v<typename T::FastMemory> ? 0 : sizeof(typename T::FastMemory),
	    std::is_empty_v<typename T::LargeMemory> ? 0 : sizeof(typename T::LargeMemory), detail::num_presets<T>(),
	    strisy::EngineParams<typename T::Engine>::count, detail::heroes<T>(), detail::preset_heroes<T>(), detail::num_preset_heroes<T>()};
};


// An effect in the fx.h shape: struct traits::<name> {
//     using Engine; using FastMemory; using LargeMemory;
//     static constexpr const char* name, *summary; static constexpr int inputs, outputs;
//     static constexpr uint32_t instr_per_block;
//     static void init(Engine&, int sample_rate); static void process(Engine&, const float* const* in, float* const* out);
// };
template <class T> struct EffectAdapter {
	using Traits = T;
	using Native = typename T::Engine;
	using FastMemory = typename T::FastMemory;
	using LargeMemory = typename T::LargeMemory;
	using Params = strisy::EngineParams<Native>;
	static_assert(PlainData<Native>, "an effect is plain data");


	Native e;
	int sample_rate;

	void init(FastMemory& f, LargeMemory& l, int sr) {
		detail::attach(e, f, l);
		T::init(e, sr);
		sample_rate = sr;
	}
	void reset() {
		detail::SavedParams<Native> saved;
		saved.save(e);
		T::init(e, sample_rate);
		saved.restore(e);
	}
	void process(const float* const* in, float* const* out) { T::process(e, in, out); }

	static constexpr int num_params = Params::count;
	[[nodiscard]] static const ParamDesc* param_desc(int i) { return Params::desc(i); }
	[[nodiscard]] float* param(int i) { return Params::ptr(e, i); }
	[[nodiscard]] bool set_param(int i, float v) {
		if (!strisy::write_param(Params::desc(i), Params::ptr(e, i), v)) return false;
		Params::changed(e, i);
		return true;
	}
	static constexpr EffectInfo info = {T::name, T::summary, T::inputs, T::outputs, T::instr_per_block,
	    detail::state_bytes<Native>(), std::is_empty_v<typename T::FastMemory> ? 0 : sizeof(typename T::FastMemory),
	    std::is_empty_v<typename T::LargeMemory> ? 0 : sizeof(typename T::LargeMemory),
	    strisy::EngineParams<typename T::Engine>::count, detail::heroes<T>()};
};


} // namespace striso::dsp

#endif // STRISO_DSP_ADAPTER_H_
