// The trapezoidal state-variable filter every engine uses (coefficients, state, one tick).
#ifndef STRISO_DSP_SVF_H_
#define STRISO_DSP_SVF_H_

#include "dspmath.h"

namespace strisy {

// ---------------------------------------------------------------------------------------------
// Trapezoidal state-variable filter (Simper): with g = tan(pi*F/SR), k = 1/Q,
//   v1 = (ic1 + g*(v0-ic2)) / (1+g(g+k)),  v2 = ic2 + g*v1,  ic1 = 2*v1-ic1,  ic2 = 2*v2-ic2
// written as v1 = a*ic1 + ga*(v0-ic2), v2 = ic2 + ga*ic1 + g2a*(v0-ic2).
// Outputs: lowpass v2, bandpass v1 (peak gain Q), fast.lib BPF k*v1, highpass v0 - k*v1 - v2.

struct SvfCoef {
	real a, ga, g2a, k;
};
struct SvfState {
	real ic1, ic2;
};

[[nodiscard]] static inline SvfCoef svf_from_g(real g, real Q) {
	real k = 1.0f / Q;
	real a = 1.0f / (1.0f + g * (g + k));
	return {a, g * a, g * g * a, k};
}

// per-sample version for modulated cutoffs: g = x*N(x^2)/D(x^2) ([7/6] Pade of tan(x), x = pi*F/SR)
// folded into one division; k is not needed for the lowpass and raw bandpass outputs
[[nodiscard]] static inline SvfCoef svf_pade(real x, real Q) {
	real x2 = x * x;
	real gN = x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2)));
	real D = 135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2));
	real QD = Q * D;
	real r = 1.0f / (QD * D + gN * (gN * Q + D));
	return {QD * D * r, gN * QD * r, gN * gN * Q * r, 0.0f};
}

#define SVF_TICK(v0, st_ic1, st_ic2, c, v1, v2) \
	do { \
		::strisy::real e_ = (v0) - (st_ic2); \
		(v1) = (c).a * (st_ic1) + (c).ga * e_; \
		(v2) = (st_ic2) + (c).ga * (st_ic1) + (c).g2a * e_; \
		(st_ic1) = 2.0f * (v1) - (st_ic1); \
		(st_ic2) = 2.0f * (v2) - (st_ic2); \
	} while (0)

} // namespace strisy

#endif // STRISO_DSP_SVF_H_
