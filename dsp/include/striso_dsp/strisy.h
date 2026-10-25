/**
 * Strisy: hand-optimized C++ port of the Striso internal synth (synth.dsp).
 *
 * Same signal flow and parameters as synth.dsp, restructured for the Cortex-M7:
 * - voice-major processing: each voice runs over the whole block with its state in locals
 *   (registers) instead of all voices advancing one sample at a time
 * - block-constant values computed once per block
 * - silent voices are skipped (GATING)
 * - every filter is a trapezoidal state-variable filter. fast.lib's LPF/HPF/BPF(K,Q) are bilinear
 *   transforms of 2-pole analog filters, which is exactly the SVF with g = K, but the direct form II
 *   biquads lose precision at low cutoffs in real (the 40 Hz accelerometer smoothing had a DC gain
 *   of 1.004 instead of 1). The SVF keeps unity DC gain and real accuracy.
 * - the modulated SVFs get g from a Pade tan (within 2 real ulps of tanf below 16 kHz) with a
 *   single division per sample
 *
 * synth.dsp stays the reference; tools/harness renders both and compares them.
 * Header-only and free of ChibiOS dependencies so the host harness compiles the same code.
 */
#ifndef STRISY_H_
#define STRISY_H_

#include <stdint.h>
#include <math.h>

#include "controls.h"
#include "dspmath.h"
#include "params.h"
#include "svf.h"

// optional profiling hooks (the firmware maps them to the cycle counter)
#ifndef STRISY_PROF_START
#define STRISY_PROF_START()
#define STRISY_PROF_MARK(k)
#endif

namespace strisy {

// silent voices are skipped (host experiments: -DSTRISY_GATING=0 runs every voice all the time)
#ifdef STRISY_GATING
static constexpr bool GATING = STRISY_GATING;
#else
static constexpr bool GATING = true;
#endif

// -DSTRISY_OSC_FAUST=1: float oscillator phase with the exact expressions of the Faust code
// (bit-identical output, used by the harness); default: fixed-point phase
#ifdef STRISY_OSC_FAUST
static constexpr bool OSC_FAUST = STRISY_OSC_FAUST;
#else
static constexpr bool OSC_FAUST = false;
#endif

static constexpr int TABLESIZE = 4096;
static constexpr int MAX_VOICES = 16;

// Control rate divider: smoothers, envelope and filter coefficients run every CTRL samples and are
// linearly interpolated to audio rate. Their inputs are smoothed below ~100 Hz; against the
// double-precision reference the deviation is about -60 dB at CTRL = 8 (6.1 kHz control rate) and
// -64 dB at CTRL = 4. CTRL = 1 is the exact per-sample path.
#ifdef STRISY_CTRL
static constexpr int CTRL = STRISY_CTRL;
#else
static constexpr int CTRL = 8;
#endif
static constexpr int NCTRL = BLOCK / CTRL;
static_assert(BLOCK % CTRL == 0, "STRISY_CTRL must divide the block size");
static constexpr int NCV = 9;         // interpolated control values per voice
static constexpr int PHASE_FRAC = 19; // fixed-point oscillator phase: 13 integer bits (period <= 4096 plus jumps)

// ---------------------------------------------------------------------------------------------
// Parameters of synth.dsp (its hsliders are compiled as constants)

static constexpr real minFreq = 200.0f;
static constexpr real filtQ = 1.0f;
static constexpr real filtFF = 1.0f;
static constexpr real filt2Freq = 3000.0f;
static constexpr real filt2Q = 2.0f;
static constexpr real filt2level = 0.8f;
static constexpr real bfQ[3] = {5.0f, 8.0f, 8.0f};
static constexpr real bflevel = 6.0f;
static constexpr real accDecay = 0.10f;

// Gating threshold on the voice level envelope (output is level * filters, so about -120 dB)
static constexpr real GATE_LEVEL = 1e-6f;

struct VoiceState {
	real phase;        // oscillator phase (OSC_FAUST)
	uint32_t phase_fx; // oscillator phase, fixed point with PHASE_FRAC fraction bits
	real note_prev;
	real env;            // velocity envelope (envdecay)
	SvfState by;         // tilt smoothing lowpass
	SvfState lv;         // level smoothing lowpass
	SvfState lp;         // main lowpass
	SvfState bp;         // tilt band-pass formant
	SvfState body[3];    // rotation body filters (TDF-II states s1, s2)
	real ctrl_prev[NCV]; // last control point of the previous block
	int active;
};

// per-voice intermediate values between the control pass and the audio pass
struct Scratch {
	real osc[BLOCK];                // oscillator output
	real cby[NCTRL], clevel[NCTRL]; // control rate tilt and level
	real ctrl[NCTRL][NCV];          // control rate values before interpolation
};

struct Engine {
	real pi_sr;             // pi / SR
	real c_period, c_inc;   // oscillator constants as in the Faust code
	real acc_fac, half_fac; // envdecay factors
	SvfCoef acc1, acc2, by_lp, lv_lp, master_hp;
	SvfCoef bp_rest;   // tilt band-pass coefficients without positive tilt
	real K_body[3][2]; // body filter K for negative/positive rotation
	SvfState acc_s[2];
	real vdacc_env;
	SvfState hp_s;
	int nvoices;
	GlobalControls g;
	VoiceControls vc[MAX_VOICES];
	VoiceState vs[MAX_VOICES];
	real vdacc[BLOCK]; // shared accelerometer shake signal for the current block
	real mix[BLOCK];
	Scratch scratch;
#ifdef STRISY_TEST_SIGNAL
	uint32_t test_n;
	real test[BLOCK];
#endif
	float table[TABLESIZE]; // noise wavetable
};

[[nodiscard]] static inline real K_f0(const Engine& e, real f0) { return tan0(e.pi_sr * f0); }

// rand_hoaglin from fastpow.h, same seed, so the noise table equals synth.dsp's
static inline void fill_noise_table(float* table) {
	int32_t state = -2014852483;
	for (int i = 0; i < TABLESIZE; i++) {
		state = (int32_t)((uint32_t)state * 1078318381u);
		table[i] = 9.3132257504915938e-10f * state;
	}
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	real sr = (real)sample_rate;
	e.pi_sr = 3.1415926535897931f / sr;
	e.c_period = 0.061156102924877762f * sr; // 0.5*SR/8.1757989 as in the Faust code
	e.c_inc = 8.1757989159999997f / sr;
	e.acc_fac = powf(0.5f, 1.0f / (sr * accDecay));
	e.half_fac = 0.7f / 64.0f / sr; // halftime2fac_fast with decaytime's 64/note folded in
	e.acc1 = svf_from_g(K_f0(e, 40), 1.31f);
	e.acc2 = svf_from_g(K_f0(e, 40), 0.54f);
	// tilt and level smoothers run at the control rate
	e.by_lp = svf_from_g(K_f0(e, 20 * CTRL), 0.71f);
	e.lv_lp = svf_from_g(K_f0(e, 100 * CTRL), 1.0f);
	e.master_hp = svf_from_g(K_f0(e, 80), 1.31f);
	e.bp_rest = svf_pade(e.pi_sr * (filt2Freq * 0.0f + minFreq), filt2Q);
	const real fb[3][2] = {{900, 1700}, {300, 600}, {1300, 2600}};
	for (int k = 0; k < 3; k++)
		for (int s = 0; s < 2; s++) e.K_body[k][s] = K_f0(e, fb[k][s]);
	e.acc_s[0] = e.acc_s[1] = e.hp_s = SvfState();
	e.vdacc_env = 0;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int v = 0; v < e.nvoices; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.vs[v] = VoiceState();
#ifdef STRISY_TEST_INIT_PHASE
		e.vs[v].phase = STRISY_TEST_INIT_PHASE; // harness: same sound, different noise realization
		e.vs[v].phase_fx = (uint32_t)(STRISY_TEST_INIT_PHASE * (1u << PHASE_FRAC));
#endif
	}
#ifdef STRISY_TEST_SIGNAL
	e.test_n = 0;
#endif
	fill_noise_table(e.table);
}

// accelerometer shake signal shared by all voices: acc_abs -> 2 lowpasses -> min(.,2) -> envdecay
static inline void process_global(Engine& e) {
	const real x = e.g.acc_abs;
	const SvfCoef c1 = e.acc1, c2 = e.acc2;
	real a1 = e.acc_s[0].ic1, a2 = e.acc_s[0].ic2, b1 = e.acc_s[1].ic1, b2 = e.acc_s[1].ic2;
	real env = e.vdacc_env;
	for (int n = 0; n < BLOCK; n++) {
		real v1, y1, u1, y2;
		SVF_TICK(x, a1, a2, c1, v1, y1);
		SVF_TICK(y1, b1, b2, c2, u1, y2);
		env = rmax(rmin(y2, 2.0f), env) * e.acc_fac;
		e.vdacc[n] = env;
	}
	e.acc_s[0] = {a1, a2};
	e.acc_s[1] = {b1, b2};
	e.vdacc_env = env;
#ifdef STRISY_TEST_SIGNAL
	// deterministic broadband test signal that is exactly the same in real and double (see tools/harness)
	for (int n = 0; n < BLOCK; n++, e.test_n++)
		e.test[n] = (real)((int)(((e.test_n % 65536u) * 7919u) % 65536u)) * (1.0f / 32768.0f) - 1.0f;
#endif
}

// one voice over a block, added into e.mix
static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const VoiceControls c = e.vc[v];

	const real vpres1 = c.vpres - 0.02f > 0.0f ? c.vpres - 0.02f : 0.0f;
	const real vplev = vpres1 / (0.5f + vpres1);
	if constexpr (GATING) {
		if (!s.active) {
			if (c.pres <= 0.0f && vplev <= 0.0f) {
				s.note_prev = c.note;
				return;
			}
			s.active = 1;
		}
	}
	// block-rate values
	const real p2 = fastpow2(0.083333333333333329f * c.note);
	const real periodf = e.c_period / p2;
	const int period = (int)(periodf < (real)TABLESIZE ? periodf : (real)TABLESIZE);
	const real fperiod = (real)period;
	const real inc = e.c_inc * (p2 * fperiod);
	const real freq = 8.1757989159999997f * p2;
	const real fbase = freq > minFreq ? freq : minFreq;
	const real pres = c.pres;
	const real pres2 = pres * pres;
	const real dmin = 16.0f * pres < 0.5f * (pres + 1.0f) ? 16.0f * pres : 0.5f * (pres + 1.0f);
	const real dped = 0.2f * e.g.pedal + 0.01f;
	const real env_fac = 1.0f - e.half_fac * (c.note / (dmin > dped ? dmin : dped));
	const real env_fac0 = fabsf(c.note - s.note_prev) < 1.0f ? env_fac : 0.0f; // resetni on note jump
	const real by_in = c.but_y;
	const real rot[3] = {e.g.rot_y, e.g.rot_x, e.g.rot_z};
	// body filters: fast.lib BPF(K, Q) as transposed direct form II, 4 operations per sample
	// (b1 = 0, b2 = -b0). They are fixed per block and between 300 and 2600 Hz, where float
	// precision is not an issue (unlike the 20-100 Hz smoothers, which stay SVFs).
	real bb0[3], ba1[3], ba2[3], bgain[3];
	for (int k = 0; k < 3; k++) {
		real ar = fabsf(rot[k]);
		real K = e.K_body[k][rot[k] > 0.0f] * (1.0f + 0.5f * ar);
		real KQ = K / bfQ[k];
		real norm = 1.0f / (1.0f + KQ + K * K);
		bb0[k] = KQ * norm;
		ba1[k] = 2.0f * (K * K - 1.0f) * norm;
		ba2[k] = (1.0f - KQ + K * K) * norm;
		bgain[k] = ar * bflevel;
	}
	const SvfCoef byc = e.by_lp, lvc = e.lv_lp;
	const real pi_sr = e.pi_sr;
	const float* __restrict__ table = e.table;
	const real* __restrict__ vdacc = e.vdacc;
	real* __restrict__ mix = e.mix;
#ifdef STRISY_TEST_SIGNAL
	const real* __restrict__ test = e.test;
#endif

	// The per-sample work is split into passes over the block, each a loop the in-order M7 can keep
	// busy. In one combined loop every sample is a single long dependency chain (tilt -> cutoff ->
	// tan -> division -> filter) and the core mostly waits. Here the recursive parts (smoothers,
	// envelope, oscillator phase, filters) are short chains, and the heavy coefficient math has no
	// sample-to-sample dependency at all, so it is unrolled and interleaved. Same arithmetic as one loop.
	STRISY_PROF_MARK(1); // voice setup (and skipped voices)
	Scratch& w = e.scratch;
	real env = s.env;
	// 1a: control recursions at the control rate (every CTRL samples): tilt smoothing, velocity
	// envelope, level smoothing. Control point k is the value at sample k*CTRL + CTRL-1.
	// The envelope steps are exact: CTRL per-sample steps e = max(v, e)*fac starting from e0 give
	// max(v*fac, e0*fac^CTRL) for v >= 0.
	{
		real fac_pow = 1.0f;
		for (int j = 1; j < CTRL; j++) fac_pow *= env_fac; // fac^(CTRL-1)
		const real facR = fac_pow * env_fac;               // fac^CTRL
		const real vf = vplev * env_fac;
		real by1 = s.by.ic1, by2 = s.by.ic2, lv1 = s.lv.ic1, lv2 = s.lv.ic2;
		for (int k = 0; k < NCTRL; k++) {
			real t1, but_y, level;
			SVF_TICK(by_in, by1, by2, byc, t1, but_y);
			if (k == 0) {
				env = rmax(vplev, env) * env_fac0; // first sample of the block: resetni
				if (CTRL > 1) env = rmax(vf, env * fac_pow);
			} else {
				env = rmax(vf, env * facR);
			}
			SVF_TICK(env + pres2, lv1, lv2, lvc, t1, level);
			w.cby[k] = but_y;
			w.clevel[k] = level;
		}
		s.by = {by1, by2};
		s.lv = {lv1, lv2};
	}
	STRISY_PROF_MARK(2); // 1a control recursions
	// 1b: cutoffs and filter coefficients at the control points, independent per point
	bool any_pos_tilt = false;
#pragma GCC unroll 4
	for (int k = 0; k < NCTRL; k++) {
		const real but_y = w.cby[k], level = w.clevel[k];
		// main lowpass cutoff, opened by level and shaking, damped by negative tilt
		real negy = rmax(-but_y, 0.0f);
		real acc = rmax(vdacc[k * CTRL + CTRL - 1] - 1.0f, 0.0f);
		real o = level * (1.0f - negy) + acc;
		real f = rmin(fbase * (filtFF * (o * o) + 1.0f), 16000.0f);
		SvfCoef lc = svf_pade(pi_sr * f, filtQ + 8.0f * negy);
		real f2lev = rmax(but_y, 0.0f) * but_y * pres;
		real dg = 1.0f - 0.5f * negy;
		real* cv = w.ctrl[k];
		cv[0] = lc.a;
		cv[1] = lc.ga;
		cv[2] = lc.g2a;
		cv[6] = (dg * dg) * level;
		cv[7] = (filt2level * f2lev) * level;
		cv[8] = level;
		any_pos_tilt |= f2lev > 0.0f;
	}
	// band-pass formant from positive tilt. Without positive tilt (f2lev = 0) its gain is 0 and its
	// cutoff is minFreq, so the coefficients are the precomputed e.bp_rest (same expression, exact)
	if (any_pos_tilt) {
#pragma GCC unroll 4
		for (int k = 0; k < NCTRL; k++) {
			const real but_y = w.cby[k];
			real f2lev = rmax(but_y, 0.0f) * but_y * pres;
			SvfCoef bcf = svf_pade(pi_sr * (filt2Freq * f2lev + minFreq), filt2Q);
			real* cv = w.ctrl[k];
			cv[3] = bcf.a;
			cv[4] = bcf.ga;
			cv[5] = bcf.g2a;
		}
	} else {
		for (int k = 0; k < NCTRL; k++) {
			real* cv = w.ctrl[k];
			cv[3] = e.bp_rest.a;
			cv[4] = e.bp_rest.ga;
			cv[5] = e.bp_rest.g2a;
		}
	}
	STRISY_PROF_MARK(7); // 1b coefficients
	{                    // 2a: periodic noise oscillator
		if constexpr (OSC_FAUST) {
			// same float expressions as the Faust code (bit-identical), for comparisons in the harness
			real phase = s.phase;
			for (int n = 0; n < BLOCK; n++) {
				phase = inc + (phase - (real)(period * ((inc + phase) > fperiod)));
				int i0 = (int)phase;
				int i1 = i0 + 1; // (i0 + 1) % period; the slow path only runs right after a big pitch jump
				if (i1 >= period) {
					i1 -= period;
					if (i1 >= period) i1 %= period;
				}
				i0 &= TABLESIZE - 1; // phase == period == 4096 can only happen for notes below ~12 Hz
				real t0 = table[i0];
				w.osc[n] = t0 + (phase - (real)i0) * (table[i1] - t0);
			}
			s.phase = phase;
		} else {
			// same oscillator with a fixed-point phase: integer compare and index instead of a float
			// compare (which stalls on the FPSCR transfer) and float/int conversions. More accurate
			// than the float accumulator; a different but equivalent noise realization.
			const uint32_t inc_fx = (uint32_t)(inc * (real)(1u << PHASE_FRAC) + 0.5f);
			const uint32_t period_fx = (uint32_t)period << PHASE_FRAC;
			uint32_t phase = s.phase_fx;
			for (int n = 0; n < BLOCK; n++) {
				phase += inc_fx;
				if (phase > period_fx) phase -= period_fx;
				int i0 = (int)(phase >> PHASE_FRAC);
				int i1 = i0 + 1; // (i0 + 1) % period; the slow path only runs right after a big pitch jump
				if (i1 >= period) {
					i1 -= period;
					if (i1 >= period) i1 %= period;
				}
				i0 &= TABLESIZE - 1;
				real t0 = table[i0];
				real frac = (real)(int32_t)(phase & ((1u << PHASE_FRAC) - 1)) * (1.0f / (1u << PHASE_FRAC));
				w.osc[n] = t0 + frac * (table[i1] - t0);
			}
			s.phase_fx = phase;
		}
#ifdef STRISY_TEST_SIGNAL
		for (int n = 0; n < BLOCK; n++) w.osc[n] = test[n];
#endif
	}
	STRISY_PROF_MARK(4); // 2a oscillator
	// The control values are interpolated linearly to audio rate inside the filter loops, starting
	// from the previous block's last control point; the last sample of each interval gets the
	// control value itself (with CTRL = 1 there is no interpolation). The filters are split over
	// two loops so each keeps its states, coefficients and increments in the 32 FPU registers.
	{ // 2b: main lowpass and tilt band-pass
		real lp1 = s.lp.ic1, lp2 = s.lp.ic2, bp1 = s.bp.ic1, bp2 = s.bp.ic2;
		real la = s.ctrl_prev[0], lga = s.ctrl_prev[1], lg2a = s.ctrl_prev[2];
		real ba = s.ctrl_prev[3], bga = s.ctrl_prev[4], bg2a = s.ctrl_prev[5];
		real lg = s.ctrl_prev[6], bg = s.ctrl_prev[7];
		for (int k = 0; k < NCTRL; k++) {
			const real* cur = w.ctrl[k];
			const real r = 1.0f / CTRL;
			const real dla = (cur[0] - la) * r, dlga = (cur[1] - lga) * r, dlg2a = (cur[2] - lg2a) * r;
			const real dba = (cur[3] - ba) * r, dbga = (cur[4] - bga) * r, dbg2a = (cur[5] - bg2a) * r;
			const real dlg = (cur[6] - lg) * r, dbg = (cur[7] - bg) * r;
#pragma GCC unroll 8
			for (int j = 0; j < CTRL; j++) {
				if (j < CTRL - 1) {
					la += dla;
					lga += dlga;
					lg2a += dlg2a;
					ba += dba;
					bga += dbga;
					bg2a += dbg2a;
					lg += dlg;
					bg += dbg;
				} else {
					la = cur[0];
					lga = cur[1];
					lg2a = cur[2];
					ba = cur[3];
					bga = cur[4];
					bg2a = cur[5];
					lg = cur[6];
					bg = cur[7];
				}
				const int n = k * CTRL + j;
				const real osc = w.osc[n];
				real t1, t2, lo, bo;
				const SvfCoef lc = {la, lga, lg2a, 0.0f};
				const SvfCoef bcf = {ba, bga, bg2a, 0.0f};
				SVF_TICK(osc, lp1, lp2, lc, t1, lo);
				SVF_TICK(osc, bp1, bp2, bcf, bo, t2);
				mix[n] += lo * lg + bo * bg;
			}
		}
		s.lp = {lp1, lp2};
		s.bp = {bp1, bp2};
		s.ctrl_prev[0] = la;
		s.ctrl_prev[1] = lga;
		s.ctrl_prev[2] = lg2a;
		s.ctrl_prev[3] = ba;
		s.ctrl_prev[4] = bga;
		s.ctrl_prev[5] = bg2a;
		s.ctrl_prev[6] = lg;
		s.ctrl_prev[7] = bg;
	}
	STRISY_PROF_MARK(3); // 2b lowpass + band-pass
	// 2c: body filters driven by rotation, scaled by level. With no rotation at all (e.g. motion
	// sensor off) their gains are exactly 0, so the loop is skipped; the filter states freeze and the
	// level interpolation start point jumps to the last control value.
	if (bgain[0] == 0.0f && bgain[1] == 0.0f && bgain[2] == 0.0f) {
		s.ctrl_prev[8] = w.ctrl[NCTRL - 1][8];
	} else {
		// TDF-II bandpass: y = b0*x + s1, s1 = s2 - a1*y, s2 = -b0*x - a2*y
		real q01 = s.body[0].ic1, q02 = s.body[0].ic2, q11 = s.body[1].ic1, q12 = s.body[1].ic2, q21 = s.body[2].ic1, q22 = s.body[2].ic2;
		real lv = s.ctrl_prev[8];
		for (int k = 0; k < NCTRL; k++) {
			const real cur = w.ctrl[k][8];
			const real dlv = (cur - lv) * (1.0f / CTRL);
#pragma GCC unroll 8
			for (int j = 0; j < CTRL; j++) {
				lv = j < CTRL - 1 ? lv + dlv : cur;
				const int n = k * CTRL + j;
				const real osc = w.osc[n];
				real r0 = bb0[0] * osc + q01;
				q01 = q02 - ba1[0] * r0;
				q02 = -bb0[0] * osc - ba2[0] * r0;
				real r1 = bb0[1] * osc + q11;
				q11 = q12 - ba1[1] * r1;
				q12 = -bb0[1] * osc - ba2[1] * r1;
				real r2 = bb0[2] * osc + q21;
				q21 = q22 - ba1[2] * r2;
				q22 = -bb0[2] * osc - ba2[2] * r2;
				mix[n] += (bgain[0] * r0 + bgain[1] * r1 + bgain[2] * r2) * lv;
			}
		}
		s.body[0] = {q01, q02};
		s.body[1] = {q11, q12};
		s.body[2] = {q21, q22};
		s.ctrl_prev[8] = lv;
	}
	STRISY_PROF_MARK(5); // 2c body filters
	s.note_prev = c.note;
	s.env = env;
	if constexpr (GATING) {
		const real level = w.ctrl[NCTRL - 1][8];
		// go idle once released and faded out. The state is frozen, not cleared: the filters stay
		// "warm" with noise like in the continuously running original, so a new note sounds the same.
		if (pres <= 0.0f && vplev <= 0.0f && fabsf(level) < GATE_LEVEL && env < GATE_LEVEL) {
			s.active = 0;
		}
	}
}

[[nodiscard]] static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

// whole block: output gets BLOCK samples
static inline void process(Engine& e, real* __restrict__ output) {
	STRISY_PROF_START();
	process_global(e);
	STRISY_PROF_MARK(0); // global
	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);
	const SvfCoef h = e.master_hp;
	real h1 = e.hp_s.ic1, h2 = e.hp_s.ic2;
	for (int n = 0; n < BLOCK; n++) {
		real x = 1.37f * e.mix[n], v1, v2;
		SVF_TICK(x, h1, h2, h, v1, v2);
		output[n] = x - h.k * v1 - v2;
	}
	e.hp_s = {h1, h2};
	STRISY_PROF_MARK(6); // master (and last voice tail)
}

} // namespace strisy

#endif // STRISY_H_
