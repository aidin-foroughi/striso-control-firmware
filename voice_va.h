/**
 * va: expressive virtual analog voice. Build with `make VOICE=va`.
 *
 * Per voice:
 *   oscillators: A = band-limited saw + pulse (PWM) + square sub one octave down (all from one phase)
 *                B = saw hard-synced to A (ratio swept by forward tilt, pressure and strike)
 *                C = free running saw, detuned a few cents (chorus; widened by gyro rotation)
 *                every discontinuity (saw wraps, pulse edges, sub flips, sync resets) gets a polyBLEP
 *                correction; the output is delayed by one sample so sync resets are corrected exactly
 *   filter:      4-pole zero-delay-feedback (TPT) ladder; the feedback is solved linearly and then
 *                soft clipped, so loud notes squash the resonance like an analog ladder. Every stage
 *                is a passive one-pole and the feedback is bounded: stable for any control values.
 *   output:      post-filter soft saturation (driven harder with pressure and strikes, little
 *                aliasing because it acts on the low-passed signal), then the VCA level
 *   modulation:  slow random pitch and cutoff drift per voice, vibrato from shaking
 *   envelope:    level follows pressure (smoothed, no stepping); a strike adds an accent transient
 *                (louder, brighter, more sync sweep) that decays in ~0.16 s; after release the level
 *                and the cutoff decay together (pedal sets the tail length, a fast release shortens it)
 * Master: DC blocker and a gentle soft clip (output can never exceed 4.2).
 *
 * Cost: about 80 FP operations per voice-sample at audio rate (oscillators ~15, ladder + saturation
 * ~50), plus control-rate coefficients every 8 samples and block-rate pitch. No libm, no per-sample
 * divisions. Silent voices are skipped.
 */
#ifndef VOICE_VA_H_
#define VOICE_VA_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace va {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 16;
static const int CTRL = 8;               // control rate divider (6.1 kHz)
static const int NCTRL = BLOCK / CTRL;
static const int NCV = 4;                // interpolated control values: G, N, drive, gain
static_assert(BLOCK % CTRL == 0, "CTRL must divide BLOCK");

using strisy::rmax;
using strisy::rmin;

// overall voice level (tuned for peaks ~2-5 on the default performance)
#ifndef VA_GAIN
#define VA_GAIN 2.3f
#endif

struct VoiceState {
	uint32_t phA, phB, phC;     // oscillator phases, 32 bit fixed point (wrap = one period)
	uint32_t incA, incB, incC;  // increments at the end of the previous block
	real sub;                   // sub oscillator sign (+1/-1)
	real prev;                  // oscillator output delayed by one sample (BLEP pre-corrections)
	real s1, s2, s3, s4;        // ladder stage states
	real lv1, lv2;              // pressure/level smoother (control rate SVF)
	real amp;                   // level after the release logic
	real acc;                   // strike accent envelope
	real ty1, ty2;              // tilt (but_y) smoother (block rate SVF)
	real ctrl_prev[NCV];        // last control point of the previous block
	real dr1[3], dr2[3];        // drift noise (two one-poles each): pitch A, pitch C, cutoff
	uint32_t rng;
	real vib_ph, pwm_ph;        // LFO phases [0, 1)
	real note_prev;
	real vneg;                  // release speed latch (largest negative vpres while releasing)
	int active;
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	real pi_sr, ph_scale, ctrl_dt, block_dt;
	real vib_inc, pwm_inc;
	strisy::SvfCoef lv_c, ty_c;
	real shake_pk, shake, gyro;  // global motion envelopes
	real dc, dc_c;              // master DC blocker
	VoiceState vs[MAX_VOICES];
	real osc[BLOCK];
	real vout[BLOCK];
	real mix[BLOCK];
	real ctrl[NCTRL][NCV];
};

// soft clipper: x - 4/27 x^3 on |x| <= 1.5, +-1 beyond (C1 continuous, slope 1 at 0)
static inline real sat(real x) {
	x = rmin(rmax(x, -1.5f), 1.5f);
	return x * (1.0f - 0.148148148f * x * x);
}

static inline real note_freq(real note) { return 8.1757989f * strisy::fastpow2(note * (1.0f / 12)); }

// parabolic sine of a phase in [0, 1)
static inline real psin(real ph) {
	real q = ph - 0.5f;
	return -8.0f * q * (1.0f - 2.0f * fabsf(q));
}

static inline void reset_voice(VoiceState& s) {
	s.prev = 0.0f;
	s.s1 = s.s2 = s.s3 = s.s4 = 0.0f;
	s.lv1 = s.lv2 = 0.0f;
	s.amp = s.acc = 0.0f;
	s.ty1 = 0.0f; s.ty2 = 0.0f;
	for (int k = 0; k < NCV; k++) s.ctrl_prev[k] = 0.0f;
	for (int k = 0; k < 3; k++) s.dr1[k] = s.dr2[k] = 0.0f;
	s.vneg = 0.0f;
	s.sub = 1.0f;
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	const real sr = (real)sample_rate;
	e.pi_sr = 3.14159265f / sr;
	e.ph_scale = 4294967296.0f / sr;
	e.ctrl_dt = (real)CTRL / sr;
	e.block_dt = (real)BLOCK / sr;
	e.vib_inc = 5.3f * e.block_dt;
	e.pwm_inc = 0.37f * e.block_dt;
	e.lv_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * CTRL * 70.0f), 0.5f);   // 70 Hz, critically damped
	e.ty_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * BLOCK * 12.0f), 0.5f);  // 12 Hz
	e.shake_pk = e.shake = e.gyro = 0.0f;
	e.dc = 0.0f;
	e.dc_c = 2.0f * 3.14159265f * 12.0f / sr;   // 12 Hz
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int v = 0; v < MAX_VOICES; v++) {
		VoiceState& s = e.vs[v];
		s = VoiceState();
		reset_voice(s);
		s.rng = 0x9E3779B9u * (uint32_t)(v + 1) + 12345u;
		// free running analog oscillators: different start phases per voice
		s.phA = s.rng * 2654435761u;
		s.phB = s.phA;
		s.phC = s.rng * 40503u;
		s.vib_ph = 0.13f * (real)v; s.vib_ph -= (real)(int)s.vib_ph;
		s.pwm_ph = 0.29f * (real)v; s.pwm_ph -= (real)(int)s.pwm_ph;
		s.note_prev = 69.0f;
		s.active = 0;
	}
	for (int v = 0; v < e.nvoices; v++) e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
}

static inline void process_global(Engine& e) {
	// shaking -> vibrato depth: peak hold with ~0.4 s decay, then smoothed so the depth never jumps
	const real sh = rmin(rmax(e.g.acc_abs - 1.1f, 0.0f), 1.0f);
	e.shake_pk = rmax(sh, e.shake_pk * (1.0f - e.block_dt * 2.5f));
	e.shake += 0.03f * (e.shake_pk - e.shake);
	// gyro rotation -> chorus (detune) depth
	const real r = rmin(rmax(rmax(fabsf(e.g.rot_x), fabsf(e.g.rot_y)), fabsf(e.g.rot_z)), 1.0f);
	e.gyro += 0.01f * (r - e.gyro);
}

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	const real pres = rmin(rmax(c.pres, 0.0f), 1.5f);
	const real vpres = rmin(rmax(c.vpres, -1.0f), 1.0f);
	const real vp1 = rmax(vpres - 0.02f, 0.0f);
	const real vplev = vp1 / (0.4f + vp1);
	const real note = rmin(rmax(c.note, 0.0f), 132.0f);
	const bool held = pres > 0.0f;
	if (!s.active) {
		if (!held && vplev <= 0.0f) { s.note_prev = note; return; }
		s.active = 1;
	}

	// ---- block rate -------------------------------------------------------------------------
	const bool jump = fabsf(note - s.note_prev) >= 1.0f;   // reassigned to a new key: new note
	s.note_prev = note;
	const real by = rmin(rmax(c.but_y, -1.0f), 1.0f);
	if (jump) { s.ty1 = 0.0f; s.ty2 = by; s.vneg = 0.0f; }
	real y;
	{
		real ty1 = s.ty1, ty2 = s.ty2, t1;
		SVF_TICK(by, ty1, ty2, e.ty_c, t1, y);
		s.ty1 = ty1; s.ty2 = ty2;
	}
	const real yn = rmax(-y, 0.0f), yp = rmax(y, 0.0f);

	// analog drift: 2x one-pole lowpassed noise (~0.5 Hz), about unit standard deviation
	real drift[3];
	for (int k = 0; k < 3; k++) {
		s.rng = s.rng * 1664525u + 1013904223u;
		const real wn = (real)(int32_t)s.rng * 4.656613e-10f;
		s.dr1[k] += 0.002f * (wn - s.dr1[k]);
		s.dr2[k] += 0.002f * (s.dr1[k] - s.dr2[k]);
		drift[k] = rmin(rmax(75.0f * s.dr2[k], -2.5f), 2.5f);
	}
#ifdef VA_TEST_CLEAN
	drift[0] = drift[1] = drift[2] = 0.0f;
#endif

	// LFOs: shake vibrato, PWM
	s.vib_ph += e.vib_inc; if (s.vib_ph >= 1.0f) s.vib_ph -= 1.0f;
	s.pwm_ph += e.pwm_inc; if (s.pwm_ph >= 1.0f) s.pwm_ph -= 1.0f;
	const real vib = 0.35f * e.shake * psin(s.vib_ph);

	// brightness of the previous block (for the sync sweep, which is pitch: set per block)
	const real bl0 = rmin(s.amp, 1.5f);
	const real b0 = bl0 * (1.5f - 0.5f * bl0);

	// pitches
	const real nA = note + 0.03f * drift[0] + vib;
#ifdef VA_TEST_CLEAN
	const real nC = nA;
#else
	const real nC = nA + 0.07f + 0.025f * drift[1] + 0.22f * e.gyro;
#endif
	const real sync_st = yp * (5.0f + 12.0f * yp + 9.0f * b0 + 10.0f * s.acc);
	const real fmaxA = 0.45f * 48828.0f, fmaxB = 0.35f * 48828.0f;
	const real fA = rmin(note_freq(nA), fmaxA);
	const real fB = rmin(fA * strisy::fastpow2(sync_st * (1.0f / 12)), fmaxB);
	const real fC = rmin(note_freq(nC), fmaxA);
	const uint32_t iA1 = (uint32_t)(fA * e.ph_scale) | 1u;
	const uint32_t iB1 = (uint32_t)(fB * e.ph_scale) | 1u;
	const uint32_t iC1 = (uint32_t)(fC * e.ph_scale) | 1u;
	if (jump || s.incA == 0) { s.incA = iA1; s.incB = iB1; s.incC = iC1; }
	const int32_t dA = (int32_t)(iA1 - s.incA) / BLOCK;
	const int32_t dB = (int32_t)(iB1 - s.incB) / BLOCK;
	const int32_t dC = (int32_t)(iC1 - s.incC) / BLOCK;
	const real invA = 1.0f / (real)iA1, invB = 1.0f / (real)iB1, invC = 1.0f / (real)iC1;

	// oscillator mix (timbre morph by tilt): back = hollow PWM square + sub, centre = two detuned
	// saws + sub, forward = sync lead
	const real lSaw = 0.5f * (1.0f - 0.75f * yn) * (1.0f - 0.65f * yp);
	const real lC = 0.36f * (1.0f - 0.5f * yn) * (1.0f - 0.6f * yp);
	const real lPul = 0.45f * yn;
	const real lSub = 0.5f * (0.2f + 0.25f * yn) * (1.0f - 0.6f * yp);
	const real lB = 0.5f * yp;
	const real pwf = 0.5f + 0.13f * psin(s.pwm_ph);
	const uint32_t pw = (uint32_t)(pwf * 4294967296.0f);
	const real M32 = 2.3283064e-10f;   // 2^-32
	const real cA = 2.0f * lSaw * M32, cB = 2.0f * lB * M32, cC = 2.0f * lC * M32;
	const real K0 = -(lSaw + lB + lC) - lPul * (2.0f * pwf - 1.0f);   // zero-mean pulse
	const real hA = -2.0f * lSaw + 2.0f * lPul;   // saw wrap + pulse rising edge
	const real hB = -2.0f * lB, hC = -2.0f * lC, hP = -2.0f * lPul;

	// filter resonance and input gain (partial bass compensation)
	const real kres = 4.0f * (0.2f + 0.72f * yn - 0.08f * yp);
	const real din = 1.0f + 0.45f * kres;

	// envelope rates
	if (vpres > 0.1f) s.vneg = 0.0f;
	if (held && vpres < 0.0f) s.vneg = rmax(s.vneg, -vpres);
	const real ped = rmin(rmax(e.g.pedal, 0.0f), 1.0f);
	const real rel_tc = (0.10f + 2.9f * ped * ped) / (1.0f + 3.0f * s.vneg);
	const real fac = 1.0f - e.ctrl_dt / (held ? 0.045f : rel_tc);
	const real acc_tc = 0.04f * strisy::fastpow2((132.0f - note) * (1.0f / 36));   // 0.16 s at note 60
	const real fac_acc = 1.0f - e.ctrl_dt / acc_tc;
	const real pc = pres * (0.3f + 0.7f * pres);
	const real cut0 = 0.75f * note + 26.0f + 0.3f * drift[2] - 9.0f * yn + 5.0f * yp;

	// ---- control rate: level, accent, cutoff -> ladder coefficients ---------------------------
	{
		real lv1 = s.lv1, lv2 = s.lv2, amp = s.amp, acc = s.acc;
		const strisy::SvfCoef lc = e.lv_c;
		const real pi_sr = e.pi_sr;
		for (int k = 0; k < NCTRL; k++) {
			acc = rmax(acc * fac_acc, vplev);
			real t1, ls;
			SVF_TICK(pc + 0.75f * acc, lv1, lv2, lc, t1, ls);
			amp = rmax(rmax(ls, 0.0f), amp * fac);
			const real bl = rmin(amp, 1.5f);
			const real b = bl * (1.5f - 0.5f * bl);
			const real cut = rmin(cut0 + 46.0f * b + 28.0f * acc, 128.0f);
			const real f = rmin(note_freq(cut), 16000.0f);
			// G = g/(1+g), g = tan(x) as the [7/6] Pade of strisy::svf_pade, one division
			const real x = pi_sr * f, x2 = x * x;
			const real gN = x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2)));
			const real D = 135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2));
			const real G = gN / (D + gN);
			const real G2 = G * G;
			const real dr = 0.6f + 0.9f * b + 0.7f * acc;
			real* cv = e.ctrl[k];
			cv[0] = G;
			cv[1] = 1.0f / (1.0f + kres * G2 * G2);
			cv[2] = dr;
			cv[3] = VA_GAIN * amp / (0.5f + 0.5f * dr);
		}
		s.lv1 = lv1; s.lv2 = lv2; s.amp = amp; s.acc = acc;
	}

	// ---- audio rate: oscillators ---------------------------------------------------------------
	{
		uint32_t pa = s.phA, pb = s.phB, pq = s.phC;
		uint32_t ia = s.incA, ib = s.incB, ic = s.incC;
		real sub = s.sub, prev = s.prev;
		real* __restrict__ osc = e.osc;
		for (int n = 0; n < BLOCK; n++) {
			ia += (uint32_t)dA; ib += (uint32_t)dB; ic += (uint32_t)dC;
			pa += ia;
			real cur = K0 + (pa < pw ? lPul : -lPul);
			// polyBLEP for a jump h at time d (in samples) before the current sample:
			// previous sample += h/2 d^2, current sample -= h/2 (1-d)^2
			if (pa < ia) {   // A wrapped: saw, pulse rise, sub flip and B's sync reset coincide
				const real d = rmin((real)pa * invA, 1.0f);
				sub = -sub;
				real h = hA + 2.0f * lSub * sub;
				const uint32_t bat = pb + (uint32_t)((1.0f - d) * (real)ib);  // B's phase at the reset
				if (bat < pb) {   // B wrapped by itself just before the reset
					const real dw = rmin(d + (real)bat * invB, 1.0f), ew = 1.0f - dw;
					prev += 0.5f * hB * dw * dw;
					cur -= 0.5f * hB * ew * ew;
				}
				h -= 2.0f * lB * ((real)bat * M32);
				pb = (uint32_t)(d * (real)ib);
				const real e1 = 1.0f - d;
				prev += 0.5f * h * d * d;
				cur -= 0.5f * h * e1 * e1;
			} else {
				pb += ib;
				if (pb < ib) {
					const real d = rmin((real)pb * invB, 1.0f), e1 = 1.0f - d;
					prev += 0.5f * hB * d * d;
					cur -= 0.5f * hB * e1 * e1;
				}
			}
			if ((uint32_t)(pa - pw) < ia) {   // pulse falling edge
				const real d = rmin((real)(uint32_t)(pa - pw) * invA, 1.0f), e1 = 1.0f - d;
				prev += 0.5f * hP * d * d;
				cur -= 0.5f * hP * e1 * e1;
			}
			pq += ic;
			if (pq < ic) {
				const real d = rmin((real)pq * invC, 1.0f), e1 = 1.0f - d;
				prev += 0.5f * hC * d * d;
				cur -= 0.5f * hC * e1 * e1;
			}
			cur += sub * lSub + cA * (real)pa + cB * (real)pb + cC * (real)pq;
			osc[n] = prev;
			prev = cur;
		}
		s.phA = pa; s.phB = pb; s.phC = pq;
		s.incA = iA1; s.incB = iB1; s.incC = iC1;
		s.sub = sub; s.prev = prev;
	}

	// ---- audio rate: ladder, saturation, VCA ------------------------------------------------
	real sum = 0.0f;
	{
		real s1 = s.s1, s2 = s.s2, s3 = s.s3, s4 = s.s4;
		real G = s.ctrl_prev[0], N = s.ctrl_prev[1], dr = s.ctrl_prev[2], gn = s.ctrl_prev[3];
		const real* __restrict__ osc = e.osc;
		real* __restrict__ out = e.vout;
		const real kr = kres;
		for (int k = 0; k < NCTRL; k++) {
			const real* cur = e.ctrl[k];
			const real r = 1.0f / CTRL;
			const real dG = (cur[0] - G) * r, dN = (cur[1] - N) * r, dd = (cur[2] - dr) * r, dg = (cur[3] - gn) * r;
#pragma GCC unroll 8
			for (int j = 0; j < CTRL; j++) {
				if (j < CTRL - 1) { G += dG; N += dN; dr += dd; gn += dg; }
				else { G = cur[0]; N = cur[1]; dr = cur[2]; gn = cur[3]; }
				const int n = k * CTRL + j;
				const real x = osc[n] * din;
				const real H = 1.0f - G;
				const real S = (((G * s1 + s2) * G + s3) * G + s4) * H;   // y4 = G^4 u + S
				const real ul = (x - kr * S) * N;                          // linear ZDF solution
				const real G2 = G * G;
				const real u = x - kr * sat(G2 * G2 * ul + S);            // saturated feedback
				real vv, y1, y2, y3, y4;
				vv = (u - s1) * G; y1 = vv + s1; s1 = y1 + vv;
				vv = (y1 - s2) * G; y2 = vv + s2; s2 = y2 + vv;
				vv = (y2 - s3) * G; y3 = vv + s3; s3 = y3 + vv;
				vv = (y3 - s4) * G; y4 = vv + s4; s4 = y4 + vv;
				const real o = sat(y4 * dr) * gn;
				out[n] = o;
				sum += o;
			}
		}
		s.s1 = s1; s.s2 = s2; s.s3 = s3; s.s4 = s4;
		s.ctrl_prev[0] = G; s.ctrl_prev[1] = N; s.ctrl_prev[2] = dr; s.ctrl_prev[3] = gn;
	}

	// guard: non-finite or absurd state -> reset this voice and drop its block
	const real st = fabsf(s.s1) + fabsf(s.s2) + fabsf(s.s3) + fabsf(s.s4) + fabsf(s.prev) + fabsf(s.amp) + fabsf(s.lv1) + fabsf(s.lv2);
	if (!(fabsf(sum) < 1e4f) || !(st < 1e3f)) {
		reset_voice(s);
		s.active = 0;
		return;
	}
	real* __restrict__ mix = e.mix;
	const real* __restrict__ out = e.vout;
	for (int n = 0; n < BLOCK; n++) mix[n] += out[n];

	// go idle once released and faded out (about -100 dB); the state is kept
	if (!held && vplev <= 0.0f && s.amp < 1e-5f && s.acc < 1e-5f) s.active = 0;
}

static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

static inline void process(Engine& e, real* __restrict__ output) {
	process_global(e);
	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);
	// master: DC blocker, then a gentle soft clip with a ceiling of 4.2 (the board plays x0.25 and
	// hard clips at 1 at the usual volume, so this keeps even stress playing out of hard clipping)
	real dc = e.dc;
	const real dcc = e.dc_c;
	for (int n = 0; n < BLOCK; n++) {
		const real x = e.mix[n];
		dc += dcc * (x - dc);
		output[n] = 4.2f * sat((x - dc) * (1.0f / 4.2f));
	}
	e.dc = (fabsf(dc) < 1e3f) ? dc : 0.0f;
}

} // namespace va

#endif // VOICE_VA_H_
