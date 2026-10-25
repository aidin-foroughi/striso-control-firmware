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

// One block of a parameter's glide from a towards its written value t: the fraction k of the way
// (an engine's glide speed), but at least kGlideStep of the declared range, and onto t once that
// reaches it. The exponential alone never arrives: it stops a few ulps short and a slow one is
// still audibly off long after the edit (a self-oscillating bore or string hears 0.2%). With the
// floor a glide over the whole range arrives within 1/kGlideStep + ln(1 / (k kGlideStep)) / k blocks
// (a 0.006 glide: 470 blocks, 0.31 s); while the fraction moves more than the floor, a step is the
// exponential's, bit for bit. A NaN target is ignored; a NaN value takes the target.
inline constexpr float kGlideStep = 1e-3f;
[[nodiscard]] inline float glide_param(float a, float t, float k, const ParamDesc& d) {
	if (t != t) return a;
	const float diff = t - a;
	if (diff != diff) return t;
	const float floor = kGlideStep * (d.max - d.min);
	if (diff <= floor && diff >= -floor) return t;
	const float step = k * diff;
	if (step < floor && step > -floor) return a + (diff > 0.0f ? floor : -floor);
	return a + step;
}

} // namespace strisy

#endif // STRISO_DSP_PARAMS_H_
