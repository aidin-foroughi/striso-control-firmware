// Live parameters: what a parameter is (ParamDesc), the per-engine table (EngineParams<Engine>,
// which an engine with parameters specializes) and the one rule for writing a value (write_param:
// the firmware's apply_params and every host use it).
#ifndef STRISO_DSP_PARAMS_H_
#define STRISO_DSP_PARAMS_H_

#include <stdint.h>

namespace strisy {

// Live-editable parameters (tools/app). An engine with parameters specializes
// strisy::EngineParams<its Engine>; the default here means "no parameters".
// ptr() returns the live value, changed() is called (in the synth thread) after it was written,
// so the engine can recompute derived values. Values are clamped to [min, max] before writing.
// What a parameter's values mean; the number is what the USB console reports (tools/striso.py).
enum class ParamKind : uint8_t {
	continuous = 0, // any value in [min, max]
	steps = 1,      // integers (writes are rounded)
	toggle = 2,     // on/off: min or max
};

struct ParamDesc {
	const char* name;  // short id, e.g. "cutoff"
	const char* group; // UI group, e.g. "filter"
	float min, max;
	ParamKind kind;
};

template <class E> struct EngineParams {
	static constexpr int count = 0;
	[[nodiscard]] static const ParamDesc* desc(int) { return nullptr; }
	[[nodiscard]] static float* ptr(E&, int) { return nullptr; }
	static void changed(E&, int) {}
};

// A write as the firmware applies it: NaN is ignored, the value is clamped to [min, max] and
// rounded unless the parameter is continuous. Returns false (nothing written) for NaN or a missing
// parameter; the caller then calls EngineParams<E>::changed().
[[nodiscard]] inline bool write_param(const ParamDesc* d, float* p, float value) {
	if (!d || !p || value != value) return false;
	float v = value < d->min ? d->min : (value > d->max ? d->max : value);
	if (d->kind != ParamKind::continuous) v = (float)(int)(v + (v >= 0 ? 0.5f : -0.5f));
	*p = v;
	return true;
}

} // namespace strisy

#endif // STRISO_DSP_PARAMS_H_
