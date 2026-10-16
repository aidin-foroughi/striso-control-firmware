/**
 * Modal resonator voice: struck and bowed bars, tines, glasses and bells.
 * Build with `make VOICE=modal`.
 *
 * Every voice is a bank of NMODES damped complex resonators ("modes"), one per partial:
 *   z[n+1] = r*e^(jw) * z[n] + b*noise[n],   output Im(z)
 * written as the coupled form re' = c*re - s*im + b*x, im' = s*re + c*im (c = r cos w, s = r sin w).
 * The pole radius is exactly sqrt(c^2 + s^2), so the mode is passive for any coefficients with
 * c^2 + s^2 < 1 and keeps its frequency accurate down to the lowest notes in float.
 *
 * Excitation:
 * - strike: a positive vpres spike adds an impulse to every mode (into re: the mode starts in sine
 *   phase, no click). Mode amplitudes = material strike weights * mallet spectrum; strike strength
 *   sets loudness and mallet brightness. The mallet also damps what was already ringing. Only
 *   presses that start from a light touch strike, so pressure wobbles while holding don't.
 * - bow/rub: while a key is held, pressure sets a target amplitude per mode (bow spectrum, brighter
 *   with more pressure). Once per block each mode's exact amplitude |z| is compared with its target
 *   and its pole radius is raised slightly above 1 (negative damping, bounded) while below target,
 *   so the modes self-sustain like a bowed bar or rubbed glass; a little noise feeds the modes to
 *   start them and gives the rubbing texture. Above target, the bow damps gently.
 * - release: extra damping ("damper") whose time is set by the pedal: T60 0.25 s at pedal 0,
 *   0.6 s at the default 3/16, 22 s at 1 (then the material's own decay dominates).
 *
 * Materials (but_y, followed while the key is held, frozen after release): -1 marimba (wood),
 * -0.5 kalimba (steel tine), 0 vibraphone (aluminium bar), +0.5 glass (wine glass, split mode
 * doublets), +1 bell (church bell, strike note = note). In between, every mode's ratio, weights and
 * damping are interpolated (partials glide), with a dead zone at each material so the pure ones are
 * easy to find.
 * Gyro: vibraphone motor, a shared tremolo whose depth and speed follow the rotation.
 *
 * Cost: per mode-sample 2 mul + 3 fma + 1 add (~9 flops, ~9 instructions with the loop overhead
 * in groups of 4 modes), 24 modes; modes outside 16 Hz..20 kHz are skipped. Coefficients use
 * polynomial sin/cos, no libm, and are updated for half of the modes per block (each mode at
 * ~760 Hz). Master: shared tremolo and a one-block lookahead limiter at 4 (0.65 ms latency).
 */
#ifndef VOICE_MODAL_H_
#define VOICE_MODAL_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace modal {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 8;
static const int NMODES = 24;
static const int NSLOT = NMODES + 4;   // + 4 dummy (always zero) slots to pad the mode list
static const int NMAT = 5;

using strisy::rmax;
using strisy::rmin;

// ---------------------------------------------------------------------------------------------
// Materials: per mode {frequency ratio, strike weight, bow weight, decay multiplier}; decay rate
// (1/s, amplitude e-folding) = (s0 + s1*f + s2*f^2) * mult, f in kHz. Slot 0 is always the note.

struct Mode { float ratio, w, b, d; };
struct Material {
	float s0, s1, s2;   // damping curve
	float hard;         // mallet hardness (strike brightness)
	float bright;       // bow brightness
	Mode m[NMODES];
};

static const Material materials[NMAT] = {
	{	// marimba: tuned rosewood bar 1 : 3.93 : 9.2, weak torsional modes, short woody knock
		2.5f, 10.0f, 5.0f, 0.7f, 0.8f, {
		{1.000f, 1.00f, 1.00f, 1.0f}, {3.930f, 0.60f, 0.25f, 1.0f}, {9.200f, 0.35f, 0.08f, 1.0f},
		{15.80f, 0.18f, 0.02f, 1.0f}, {2.710f, 0.06f, 0.03f, 2.5f}, {5.850f, 0.07f, 0.02f, 2.5f},
		{12.30f, 0.08f, 0.01f, 2.0f}, {22.60f, 0.10f, 0.00f, 1.0f}, {4.600f, 0.04f, 0.00f, 3.0f},
		{7.400f, 0.05f, 0.00f, 3.0f}, {10.70f, 0.05f, 0.00f, 3.0f}, {18.40f, 0.06f, 0.00f, 2.0f},
		{26.50f, 0.06f, 0.00f, 1.5f}, {30.30f, 0.05f, 0.00f, 1.5f}, {34.80f, 0.05f, 0.00f, 1.5f},
		{39.70f, 0.04f, 0.00f, 1.5f}, {2.020f, 0.03f, 0.02f, 3.0f}, {13.60f, 0.03f, 0.00f, 3.0f},
		{20.10f, 0.04f, 0.00f, 2.0f}, {44.60f, 0.03f, 0.00f, 1.5f}, {49.90f, 0.03f, 0.00f, 1.5f},
		{6.600f, 0.03f, 0.00f, 3.0f}, {8.300f, 0.03f, 0.00f, 3.0f}, {24.40f, 0.04f, 0.00f, 2.0f}}},
	{	// kalimba: clamped-free steel tine 1 : 6.27 : 17.55 : 34.4, two vibration planes (slow beats)
		0.8f, 2.5f, 1.0f, 1.0f, 1.0f, {
		{1.000f, 1.00f, 1.00f, 1.0f}, {6.267f, 0.40f, 0.10f, 1.0f}, {17.55f, 0.20f, 0.02f, 1.0f},
		{34.39f, 0.10f, 0.00f, 1.0f}, {1.003f, 0.10f, 0.10f, 1.2f}, {2.940f, 0.05f, 0.02f, 2.0f},
		{4.100f, 0.04f, 0.01f, 2.0f}, {6.300f, 0.10f, 0.02f, 1.0f}, {9.800f, 0.04f, 0.00f, 2.0f},
		{12.60f, 0.04f, 0.00f, 2.0f}, {21.90f, 0.04f, 0.00f, 1.5f}, {27.50f, 0.03f, 0.00f, 1.5f},
		{14.90f, 0.02f, 0.00f, 2.0f}, {24.80f, 0.02f, 0.00f, 2.0f}, {30.10f, 0.02f, 0.00f, 2.0f},
		{40.20f, 0.02f, 0.00f, 2.0f}, {45.50f, 0.02f, 0.00f, 2.0f}, {51.30f, 0.02f, 0.00f, 2.0f},
		{56.84f, 0.03f, 0.00f, 1.0f}, {8.200f, 0.02f, 0.00f, 2.0f}, {11.30f, 0.02f, 0.00f, 2.0f},
		{19.60f, 0.02f, 0.00f, 2.0f}, {3.500f, 0.02f, 0.00f, 2.0f}, {5.100f, 0.02f, 0.00f, 2.0f}}},
	{	// vibraphone: aluminium bar tuned 1 : 4 : 10, long sustain
		0.5f, 1.0f, 0.4f, 1.2f, 1.0f, {
		{1.000f, 1.00f, 1.00f, 1.0f}, {4.000f, 0.55f, 0.20f, 1.0f}, {10.00f, 0.30f, 0.06f, 1.0f},
		{17.60f, 0.12f, 0.02f, 1.2f}, {26.80f, 0.06f, 0.00f, 1.2f}, {2.800f, 0.04f, 0.02f, 2.0f},
		{5.750f, 0.04f, 0.01f, 2.0f}, {8.900f, 0.03f, 0.00f, 2.0f}, {13.40f, 0.04f, 0.00f, 2.0f},
		{3.400f, 0.02f, 0.00f, 2.0f}, {20.50f, 0.02f, 0.00f, 2.0f}, {22.90f, 0.02f, 0.00f, 2.0f},
		{31.70f, 0.02f, 0.00f, 2.0f}, {36.90f, 0.02f, 0.00f, 2.0f}, {41.40f, 0.02f, 0.00f, 2.0f},
		{7.000f, 0.02f, 0.00f, 2.0f}, {11.80f, 0.02f, 0.00f, 2.0f}, {15.10f, 0.02f, 0.00f, 2.0f},
		{24.60f, 0.02f, 0.00f, 2.0f}, {29.00f, 0.02f, 0.00f, 2.0f}, {33.80f, 0.02f, 0.00f, 2.0f},
		{6.400f, 0.02f, 0.00f, 2.0f}, {19.00f, 0.02f, 0.00f, 2.0f}, {38.50f, 0.02f, 0.00f, 2.0f}}},
	{	// glass: wine glass (n,0) modes as slightly split doublets (shimmering beats), very long decay
		0.35f, 0.5f, 0.12f, 1.6f, 1.4f, {
		{1.000f, 1.00f, 1.00f, 1.0f}, {1.0017f, 0.45f, 0.50f, 1.0f}, {2.420f, 0.55f, 0.30f, 1.0f},
		{2.4255f, 0.35f, 0.20f, 1.0f}, {4.370f, 0.35f, 0.10f, 1.0f}, {4.381f, 0.25f, 0.06f, 1.0f},
		{6.820f, 0.22f, 0.04f, 1.0f}, {6.836f, 0.16f, 0.03f, 1.0f}, {9.790f, 0.14f, 0.02f, 1.0f},
		{9.812f, 0.10f, 0.01f, 1.0f}, {13.30f, 0.09f, 0.00f, 1.0f}, {13.33f, 0.07f, 0.00f, 1.0f},
		{17.40f, 0.06f, 0.00f, 1.0f}, {1.580f, 0.06f, 0.02f, 1.5f}, {3.350f, 0.05f, 0.01f, 1.5f},
		{5.500f, 0.04f, 0.00f, 1.5f}, {8.200f, 0.04f, 0.00f, 1.5f}, {11.40f, 0.03f, 0.00f, 1.5f},
		{15.20f, 0.03f, 0.00f, 1.5f}, {21.90f, 0.04f, 0.00f, 1.0f}, {26.80f, 0.03f, 0.00f, 1.0f},
		{17.44f, 0.04f, 0.00f, 1.0f}, {21.95f, 0.03f, 0.00f, 1.0f}, {32.00f, 0.02f, 0.00f, 1.0f}}},
	{	// bell: minor-third church bell, ratios relative to the nominal (strike note = note):
		// hum 0.25, prime 0.5, tierce 0.59, quint 0.75, nominal 1, ... with doublets (warble)
		0.25f, 0.7f, 0.18f, 1.4f, 1.2f, {
		{1.000f, 1.00f, 1.00f, 1.0f}, {0.250f, 0.55f, 0.20f, 0.6f}, {0.500f, 0.50f, 0.30f, 0.8f},
		{0.5915f, 0.65f, 0.25f, 0.8f}, {0.753f, 0.25f, 0.10f, 0.9f}, {1.257f, 0.45f, 0.15f, 1.0f},
		{1.331f, 0.30f, 0.05f, 1.0f}, {1.505f, 0.30f, 0.05f, 1.0f}, {2.083f, 0.25f, 0.02f, 1.0f},
		{2.717f, 0.18f, 0.00f, 1.0f}, {3.398f, 0.12f, 0.00f, 1.0f}, {4.107f, 0.09f, 0.00f, 1.0f},
		{1.0021f, 0.35f, 0.40f, 1.0f}, {0.2506f, 0.25f, 0.08f, 0.6f}, {0.5013f, 0.20f, 0.10f, 0.8f},
		{0.5928f, 0.25f, 0.10f, 0.8f}, {1.722f, 0.12f, 0.00f, 1.0f}, {1.888f, 0.10f, 0.00f, 1.0f},
		{2.390f, 0.10f, 0.00f, 1.0f}, {3.030f, 0.08f, 0.00f, 1.0f}, {3.750f, 0.07f, 0.00f, 1.0f},
		{4.850f, 0.06f, 0.00f, 1.0f}, {5.600f, 0.05f, 0.00f, 1.0f}, {6.400f, 0.04f, 0.00f, 1.0f}}},
};

// ---------------------------------------------------------------------------------------------
// Tuning constants

static const real F_LO0 = 16.0f, F_LO1 = 24.0f;        // modes fade in above 16..24 Hz
static const real F_HI0 = 14000.0f, F_HI1 = 20000.0f;  // and out between 14 and 20 kHz
static const real STRIKE_THR = 0.03f;   // vpres above this is a strike
static const real STRIKE_SPAN = 0.7f;   // vpres - thr giving full strike strength
static const real STRIKE_REARM = 0.02f; // strike strength below this ends a strike
static const real STRIKE_LEVEL = 1.7f;  // mode energy of a full strike (sqrt of sum of squares)
static const real MALLET_DAMP = 0.6f;   // fraction of the ringing a full strike stops
static const real BOW_LEVEL = 1.1f;     // bow target (rms of the mode amplitudes) at pres 1
static const real BOW_NOISE = 0.25f;    // noise share of the bow excitation
static const real BOW_GROW = 60.0f;     // max negative damping of the bow (1/s)
static const real BOW_DAMP = 8.0f;      // max damping by the bow above target, per unit pressure
static const real ENERGY_CAP = 9.0f;    // soft cap on a voice's mode energy (sum |z|^2)
static const real ENERGY_BAD = 1e4f;    // above this (or NaN) the voice is reset
static const real GATE_ENERGY = 1e-10f; // released voice below this goes idle
#ifndef MODAL_LIMIT
#define MODAL_LIMIT 4.0f
#endif
static const real LIMIT = MODAL_LIMIT;  // master limiter threshold
#ifndef MODAL_CLIP
#define MODAL_CLIP 7.5f
#endif
static const real CLIP = MODAL_CLIP;    // final safety clamp

struct VoiceState {
	real re[NSLOT], im[NSLOT];       // mode states (slots NMODES.. are always 0)
	real a[NMODES];                  // smoothed per-sample decay of each mode (log pole radius)
	real cc[NMODES], cs[NMODES], cb[NMODES];  // coefficients per mode (updated every other block)
	real bsp[NMODES];                // bow spectrum per mode
	// mode list of the current block: active slots (padded to a multiple of 4) and coefficients
	real lc[NSLOT], ls[NSLOT], lb[NSLOT];
	uint8_t lidx[NSLOT];
	int nact;
	uint32_t on_mask;               // slots active in the last block
	uint32_t parity;                // which half of the modes is updated this block
	real morph;                     // smoothed but_y
	real note_prev;
	real sdone;                     // strike strength already delivered in the current strike
	real pbase;                     // pressure before the current rise
	uint32_t rng;
	int active;
};

struct Scratch {
	real f[NMODES], fade[NMODES], w[NMODES];
	real x[BLOCK];                  // bow noise
	real out[BLOCK];                // voice output
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	real sr, inv_sr, w_hz;          // w_hz = 2 pi / sr
	real sig_rel;                   // release damping of the block (1/s)
	real trem_depth, trem_phase, trem_gain;
	real lim_gain, peak_prev;
	real mix[BLOCK];
	real delay[BLOCK];              // limiter lookahead
	Scratch sc;
	VoiceState vs[MAX_VOICES];
};

// ---------------------------------------------------------------------------------------------
// Helpers (no libm)

// cos and sin for |x| <= pi/2 (Taylor to x^8 / x^9, error below 4e-6), as in voice_sine.h
static inline real cos_poly(real x) {
	real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
static inline real sin_poly(real x) {
	real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
// sin(2 pi p) for any p in [0, 1)
static inline real sin_turn(real p) {
	real x = 6.2831853f * p - 3.1415927f;   // [-pi, pi), sin(x + pi) = -sin(x)
	if (x > 1.5707963f) x = 3.1415927f - x;
	else if (x < -1.5707963f) x = -3.1415927f - x;
	return -sin_poly(x);
}
// 1/sqrt(x) for x > 0, relative error ~5e-6
static inline real rsqrt_fast(real x) {
	union { float f; uint32_t i; } u = { x };
	u.i = 0x5f375a86u - (u.i >> 1);
	real y = u.f;
	y = y * (1.5f - 0.5f * x * y * y);
	y = y * (1.5f - 0.5f * x * y * y);
	return y;
}
static inline real clampr(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }

static inline void reset_voice(VoiceState& s) {
	for (int k = 0; k < NSLOT; k++) { s.re[k] = 0.0f; s.im[k] = 0.0f; }
	for (int k = 0; k < NMODES; k++) { s.a[k] = 0.0f; s.cc[k] = 0.0f; s.cs[k] = 0.0f; s.cb[k] = 0.0f; s.bsp[k] = 0.0f; }
	s.nact = 0;
	s.on_mask = 0;
	s.sdone = 0.0f;
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.w_hz = 6.2831853f / e.sr;
	e.sig_rel = 10.0f;
	e.trem_depth = 0.0f; e.trem_phase = 0.0f; e.trem_gain = 1.0f;
	e.lim_gain = 1.0f;
	e.peak_prev = 0.0f;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int n = 0; n < BLOCK; n++) { e.mix[n] = 0.0f; e.delay[n] = 0.0f; }
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		VoiceState& s = e.vs[v];
		reset_voice(s);
		s.morph = 0.0f;
		s.note_prev = 69.0f;
		s.pbase = 0.0f;
		s.rng = 0x9e3779b9u * (uint32_t)(v + 1);
		s.active = 0;
	}
}

// Run the mode list over the block into out (overwritten). Four modes per pass keep their states
// and coefficients in registers; the modes are independent chains the in-order core can interleave.
template <bool INPUT>
static inline void run_modes(VoiceState& s, const real* __restrict__ x, real* __restrict__ out) {
	for (int n = 0; n < BLOCK; n++) out[n] = 0.0f;
	for (int m = 0; m < s.nact; m += 4) {
		const int i0 = s.lidx[m], i1 = s.lidx[m + 1], i2 = s.lidx[m + 2], i3 = s.lidx[m + 3];
		real r0 = s.re[i0], q0 = s.im[i0], r1 = s.re[i1], q1 = s.im[i1];
		real r2 = s.re[i2], q2 = s.im[i2], r3 = s.re[i3], q3 = s.im[i3];
		const real c0 = s.lc[m], c1 = s.lc[m + 1], c2 = s.lc[m + 2], c3 = s.lc[m + 3];
		const real s0 = s.ls[m], s1 = s.ls[m + 1], s2 = s.ls[m + 2], s3 = s.ls[m + 3];
		const real b0 = s.lb[m], b1 = s.lb[m + 1], b2 = s.lb[m + 2], b3 = s.lb[m + 3];
		// unrolled by 2 so the new states can alternate registers instead of being moved
#pragma GCC unroll 2
		for (int n = 0; n < BLOCK; n++) {
			real t0, t1, t2, t3;
			if (INPUT) {
				const real xn = x[n];
				t0 = c0 * r0 - s0 * q0 + b0 * xn;
				t1 = c1 * r1 - s1 * q1 + b1 * xn;
				t2 = c2 * r2 - s2 * q2 + b2 * xn;
				t3 = c3 * r3 - s3 * q3 + b3 * xn;
			} else {
				t0 = c0 * r0 - s0 * q0;
				t1 = c1 * r1 - s1 * q1;
				t2 = c2 * r2 - s2 * q2;
				t3 = c3 * r3 - s3 * q3;
			}
			q0 = s0 * r0 + c0 * q0;
			q1 = s1 * r1 + c1 * q1;
			q2 = s2 * r2 + c2 * q2;
			q3 = s3 * r3 + c3 * q3;
			r0 = t0; r1 = t1; r2 = t2; r3 = t3;
			out[n] += (q0 + q1) + (q2 + q3);
		}
		s.re[i0] = r0; s.im[i0] = q0; s.re[i1] = r1; s.im[i1] = q1;
		s.re[i2] = r2; s.im[i2] = q2; s.re[i3] = r3; s.im[i3] = q3;
	}
}

// strike loudness (mode energy) for strength 0..1
static inline real strike_level(real st) { return STRIKE_LEVEL * st * (1.5f - 0.5f * st); }

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	Scratch& w = e.sc;
	const real pres = clampr(c.pres, 0.0f, 1.5f);
	const bool held = pres > 0.0f;
	const real starget = clampr((c.vpres - STRIKE_THR) * (1.0f / STRIKE_SPAN), 0.0f, 1.0f);

	if (!s.active) {
		if (!held && starget <= STRIKE_REARM) {
			s.note_prev = c.note;
			s.morph = clampr(c.but_y, -1.0f, 1.0f);
			s.sdone = 0.0f;
			s.pbase = pres;
			return;
		}
		reset_voice(s);
		s.active = 1;
		s.morph = clampr(c.but_y, -1.0f, 1.0f);
	} else if (fabsf(c.note - s.note_prev) >= 1.0f) {
		// new note on a sounding voice: fade the old one out over this block with its old modes
		// (about -45 dB), then start the new one next block (its strike is still pending then)
		for (int m = 0; m < s.nact; m++) { s.lc[m] *= 0.85f; s.ls[m] *= 0.85f; }
		run_modes<false>(s, w.x, w.out);
		for (int n = 0; n < BLOCK; n++) e.mix[n] += w.out[n];
		reset_voice(s);
		s.note_prev = c.note;
		s.morph = clampr(c.but_y, -1.0f, 1.0f);
		s.pbase = 0.0f;   // a new note may strike even when the voice was taken from a held key
		return;
	}
	s.note_prev = c.note;

	// --- material morph -----------------------------------------------------------------------
	// the material follows the tilt while the key is held and stays put after release
	if (held) s.morph += 0.08f * (clampr(c.but_y, -1.0f, 1.0f) - s.morph);   // ~20 Hz smoothing
	real u = (s.morph + 1.0f) * 2.0f;                               // 0..4
	int mi = (int)u;
	if (mi > NMAT - 2) mi = NMAT - 2;
	if (mi < 0) mi = 0;
	real t = clampr((u - (real)mi - 0.12f) * (1.0f / 0.76f), 0.0f, 1.0f);  // dead zone at materials
	t = t * t * (3.0f - 2.0f * t);
	const Material& MA = materials[mi];
	const Material& MB = materials[mi + 1];
	const real ms0 = MA.s0 + t * (MB.s0 - MA.s0);
	const real ms1 = MA.s1 + t * (MB.s1 - MA.s1);
	const real ms2 = MA.s2 + t * (MB.s2 - MA.s2);
	const real mhard = MA.hard + t * (MB.hard - MA.hard);
	const real mbright = MA.bright + t * (MB.bright - MA.bright);

	const real f0 = 8.1757989f * strisy::fastpow2(c.note * (1.0f / 12.0f));
	const real sig_rel = held ? 0.0f : e.sig_rel;

	// --- strike ---------------------------------------------------------------------------------
	// A strike is a fast pressure rise. Its strength is delivered as it grows (a rise over several
	// control updates adds up). Only rises that start from a light touch strike: pressure wobbles
	// while holding (bowing) must not retrigger the mallet.
	real dlev = 0.0f;
	if (starget <= STRIKE_REARM) {
		s.sdone = 0.0f;
		s.pbase = pres;
	} else if (starget > s.sdone + 0.005f) {
		const real from_light = clampr((0.3f - s.pbase) * 4.0f, 0.0f, 1.0f);
		dlev = from_light * (strike_level(starget) - strike_level(s.sdone));
		s.sdone = starget;
	}
	const real keep = 1.0f - MALLET_DAMP * (dlev * (1.0f / STRIKE_LEVEL));
	const real fcs = mhard * (250.0f + 0.8f * f0) * (1.0f + starget * (3.0f + 9.0f * starget));
	const real inv_fcs = 1.0f / fcs;

	// --- bow ------------------------------------------------------------------------------------
	const real bowlev = held ? BOW_LEVEL * pres * (0.4f + 0.6f * pres) : 0.0f;
	const bool bowing = bowlev > 1e-4f;
	const real fcb = f0 * mbright * (1.5f + 5.0f * pres * pres);
	const real inv_fcb = 1.0f / fcb;

	// Coefficients are updated for half of the modes per block (even and odd slots alternately,
	// ~760 Hz per mode, plenty for vibrato and the bow loop); modes that just switched on at once.
	s.parity ^= 1u;
	// pass 1: frequencies and band limits of all modes, strike spectrum
	real ssum1 = 0.0f, ssum2 = 0.0f;
	uint32_t mask = 0;
	for (int k = 0; k < NMODES; k++) {
		const Mode& A = MA.m[k];
		const Mode& B = MB.m[k];
		const real f = f0 * (A.ratio + t * (B.ratio - A.ratio));
		const real fade = rmin(clampr((f - F_LO0) * (1.0f / (F_LO1 - F_LO0)), 0.0f, 1.0f),
		                       clampr((F_HI1 - f) * (1.0f / (F_HI1 - F_HI0)), 0.0f, 1.0f));
		w.f[k] = f;
		w.fade[k] = fade;
		if (fade > 0.0f) mask |= 1u << k;
		if (dlev > 0.0f) {
			const real y = f * inv_fcs;
			const real ss = (A.w + t * (B.w - A.w)) * fade / (1.0f + y * y);
			w.w[k] = ss;
			ssum1 += ss;
			ssum2 += ss * ss;
		}
	}
	// strike amplitudes: energy normalized, peak limited for bright spectra
	real snorm = 0.0f;
	if (dlev > 0.0f && ssum2 > 1e-12f) snorm = dlev / rmax(ssum2 * rsqrt_fast(ssum2), 0.4f * ssum1);

	// modes that went silent (out of range) are cleared so they don't come back stale
	uint32_t off = s.on_mask & ~mask;
	for (int k = 0; off; k++, off >>= 1)
		if (off & 1) { s.re[k] = 0.0f; s.im[k] = 0.0f; s.a[k] = 0.0f; s.bsp[k] = 0.0f; }
	const uint32_t was_on = s.on_mask;
	s.on_mask = mask;
	const uint32_t upd = mask & ((s.parity ? 0xAAAAAAAAu : 0x55555555u) | ~was_on);

	// bow spectrum (updated modes), normalized to unit energy over all modes
	real bsum = 0.0f;
	for (int k = 0; k < NMODES; k++) {
		if (!(mask & (1u << k))) continue;
		if (bowing && (upd & (1u << k))) {
			const real y = w.f[k] * inv_fcb;
			s.bsp[k] = (MA.m[k].b + t * (MB.m[k].b - MA.m[k].b)) * w.fade[k] / (1.0f + y * y);
		}
		bsum += s.bsp[k] * s.bsp[k];
	}
	const real bnorm = bowing && bsum > 1e-12f ? bowlev * rsqrt_fast(bsum) : 0.0f;

	// pass 2: strike, then coefficients of the updated modes
	const real bdamp = -BOW_DAMP * pres;
	int m = 0;
	for (int k = 0; k < NMODES; k++) {
		if (!(mask & (1u << k))) continue;
		real re = s.re[k], im = s.im[k];
		if (dlev > 0.0f) {
			re = re * keep + snorm * w.w[k];
			im = im * keep;
			s.re[k] = re; s.im[k] = im;
		}
		if (upd & (1u << k)) {
			const real f = w.f[k];
			// rotation
			const real wk = f * e.w_hz;
			real cw, sw;
			if (wk > 1.5707963f) { const real x = 3.1415927f - wk; cw = -cos_poly(x); sw = sin_poly(x); }
			else { cw = cos_poly(wk); sw = sin_poly(wk); }
			const real g = 1.5f - 0.5f * (cw * cw + sw * sw);   // unit magnitude
			cw *= g; sw *= g;
			// damping: material curve, release damper (stronger for high modes), bow
			const real fk = f * 0.001f;
			const real dm = MA.m[k].d + t * (MB.m[k].d - MA.m[k].d);
			real sigma = (ms0 + fk * (ms1 + fk * ms2)) * dm + sig_rel * (1.0f + 0.5f * fk);
			real b = 0.0f;
			if (bowing) {
				const real T = bnorm * s.bsp[k];
				if (T > 1e-5f) {
					const real mag2 = re * re + im * im;
					const real beta = clampr(BOW_GROW * (1.0f - mag2 / (T * T)), bdamp, BOW_GROW);
					// noise input for an rms of BOW_NOISE*T from noise alone (uniform noise, var 1/3)
					const real an = rmax(sigma * e.inv_sr, 1e-6f);
					b = BOW_NOISE * 1.7320508f * T * 2.0f * (an * rsqrt_fast(an));
					sigma -= beta;
				}
			}
			// decay changes (damper on release, bow) are smoothed over a few updates: a step in the
			// decay rate is a kink in the envelope, audible as a tick on loud notes
			const real at = clampr(sigma * e.inv_sr, -0.0015f, 0.03f);
			const real a = (was_on & (1u << k)) ? s.a[k] + 0.3f * (at - s.a[k]) : at;
			s.a[k] = a;
			const real r = 1.0f + a * (-1.0f + a * (0.5f - a * (1.0f / 6.0f)));   // exp(-a)
			s.cc[k] = r * cw;
			s.cs[k] = r * sw;
			s.cb[k] = b;
		}
		s.lidx[m] = (uint8_t)k;
		s.lc[m] = s.cc[k];
		s.ls[m] = s.cs[k];
		s.lb[m] = s.cb[k];
		m++;
	}
	for (int p = 0; (m & 3) != 0; p++, m++) {   // pad with dummy modes
		s.lidx[m] = (uint8_t)(NMODES + p);
		s.lc[m] = 0.0f; s.ls[m] = 0.0f; s.lb[m] = 0.0f;
	}
	s.nact = m;

	// --- audio ----------------------------------------------------------------------------------
	if (bowing) {
		uint32_t rng = s.rng;
		for (int n = 0; n < BLOCK; n++) {
			rng = rng * 1664525u + 1013904223u;
			w.x[n] = (real)(int32_t)rng * 4.6566129e-10f;
		}
		s.rng = rng;
		run_modes<true>(s, w.x, w.out);
	} else {
		run_modes<false>(s, w.x, w.out);
	}

	// --- guard, energy cap, gating ----------------------------------------------------------------
	real energy = 0.0f;
	for (int k = 0; k < NMODES; k++) {
		const real m2 = s.re[k] * s.re[k] + s.im[k] * s.im[k];
		// flush modes that decayed below -200 dB before they reach denormals
		if (m2 < 1e-20f) { s.re[k] = 0.0f; s.im[k] = 0.0f; }
		energy += m2;
	}
	if (!(energy < ENERGY_BAD)) {   // NaN, Inf or runaway: drop this block and reset the voice
		reset_voice(s);
		s.active = 0;
		return;
	}
	if (energy > ENERGY_CAP) {
		const real gsc = 3.0f * rsqrt_fast(energy);   // sqrt(ENERGY_CAP / energy)
		for (int k = 0; k < NMODES; k++) { s.re[k] *= gsc; s.im[k] *= gsc; }
	}
	for (int n = 0; n < BLOCK; n++) e.mix[n] += w.out[n];
	if (!held && s.sdone <= 0.0f && energy < GATE_ENERGY) {
		reset_voice(s);
		s.active = 0;
	}
}

static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

static inline void process(Engine& e, real* __restrict__ output) {
	// release damping from the pedal: T60 0.25 s (pedal 0) .. 0.58 s (default 3/16) .. 22 s (1)
	const real ped = clampr(e.g.pedal, 0.0f, 1.0f);
	e.sig_rel = 27.631f / strisy::fastpow2(6.5f * ped);

	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);

	// vibraphone motor: shared tremolo, depth and speed from the gyro (0 when the sensor is off)
	const real rot = fabsf(e.g.rot_x) + fabsf(e.g.rot_y) + fabsf(e.g.rot_z);
	const real dt = rmin(1.5f * rot, 1.0f);
	e.trem_depth += (dt > e.trem_depth ? 0.05f : 0.0015f) * (dt - e.trem_depth);
	real tg1 = 1.0f;
	if (e.trem_depth > 1e-4f) {
		e.trem_phase += (3.0f + 4.0f * e.trem_depth) * BLOCK * e.inv_sr;
		if (e.trem_phase >= 1.0f) e.trem_phase -= 1.0f;
		tg1 = 1.0f - 0.55f * e.trem_depth * (0.5f + 0.5f * sin_turn(e.trem_phase));
	} else {
		e.trem_depth = 0.0f;
	}
	const real tg0 = e.trem_gain;
	const real dtg = (tg1 - tg0) * (1.0f / BLOCK);

	// tremolo, then peak of the block
	real peak = 0.0f, sabs = 0.0f;
	real tg = tg0;
	for (int n = 0; n < BLOCK; n++) {
		tg += dtg;
		const real x = e.mix[n] * tg;
		e.mix[n] = x;
		const real a = fabsf(x);
		sabs += a;
		peak = rmax(peak, a);
	}
	e.trem_gain = tg1;
	if (!(sabs < 1e9f)) {   // non-finite: silence the block and reset everything
		for (int n = 0; n < BLOCK; n++) { output[n] = 0.0f; e.mix[n] = 0.0f; e.delay[n] = 0.0f; }
		for (int v = 0; v < e.nvoices; v++) { reset_voice(e.vs[v]); e.vs[v].active = 0; }
		e.lim_gain = 1.0f;
		e.trem_gain = 1.0f;
		e.peak_prev = 0.0f;
		return;
	}
	// limiter with one block lookahead: the output is the previous block, its gain ramps from the
	// gain chosen for it (g0) to one that also suits the next block (g1); both are below what the
	// delayed block needs, so the ramp never overshoots. Instant attack, ~0.5 s recovery.
	const real need = rmax(peak, e.peak_prev);
	const real gt = need > LIMIT ? LIMIT / need : 1.0f;
	const real g0 = e.lim_gain;
	const real g1 = rmin(gt, g0 + 0.0015f);
	const real dg = (g1 - g0) * (1.0f / BLOCK);
	real g = g0;
	for (int n = 0; n < BLOCK; n++) {
		g += dg;
		output[n] = clampr(e.delay[n] * g, -CLIP, CLIP);
		e.delay[n] = e.mix[n];
	}
	e.lim_gain = g1;
	e.peak_prev = peak;
}

} // namespace modal

#endif // VOICE_MODAL_H_
