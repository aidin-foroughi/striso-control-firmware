// Numeric helpers shared by the voice engines and the effects: the sample type, fast exp2 and tan
// approximations, branch-free min/max. No libm calls on the board's hot paths (newlib does parts
// of expf/powf/tanf in software double precision). Namespace strisy: the engines have always
// spelled them strisy::fastpow2 etc. (strisy, the first engine, is where they came from).
#ifndef STRISO_DSP_DSPMATH_H_
#define STRISO_DSP_DSPMATH_H_

#include <stdint.h>
#include <math.h>

namespace strisy {

// sample type: float on the board; the host harness can build with STRISY_REAL=double to check the
// algorithm against Faust's -double output independent of float rounding
#ifdef STRISY_REAL
using real = STRISY_REAL;
#else
using real = float;
#endif

// Approximations from fast.lib / fastpow.h, kept identical so the sound matches synth.dsp

[[nodiscard]] static inline float fastpow2(float p) {
	int w = p;
	float z = p - w;
	union {
		uint32_t i;
		float f;
	} v = {static_cast<uint32_t>((1 << 23) * (p + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z))};
	return v.f;
}

// Taylor approximation of tan used by fast.lib's K_f0
[[nodiscard]] static inline real tan0(real a) {
	real a2 = a * a;
	return ((0.2033f * a2 + 0.31755f) * a2 + 1.0f) * a;
}

// max/min as single vmaxnm/vminnm instructions (a ternary select needs vcmp + vmrs, which waits
// for the FPU pipeline to drain); identical results for the non-NaN values used here
[[nodiscard]] static inline real rmax(real a, real b) { return sizeof(real) == sizeof(float) ? (real)fmaxf((float)a, (float)b) : (real)fmax(a, b); }
[[nodiscard]] static inline real rmin(real a, real b) { return sizeof(real) == sizeof(float) ? (real)fminf((float)a, (float)b) : (real)fmin(a, b); }

} // namespace strisy

#endif // STRISO_DSP_DSPMATH_H_
