/**
 * Bowed string voice engine: a digital waveguide string driven by a friction bow, with a small
 * modal body. Build with `make VOICE=bowed`.
 *
 * String: two velocity-wave delay loops meeting at the bow, one for the neck side (bow -> nut ->
 * bow, read with a 4-tap Lagrange fractional delay that carries the pitch and vibrato) and one for
 * the bridge side (bow -> bridge -> bow, linear interpolation, its length sets the bow position).
 * The nut reflects with -1, the bridge with -g*lowpass (one pole): the string losses. The loss
 * filter's phase delay at the fundamental is subtracted from the loop so the tuning is exact.
 *
 * Bow: explicit friction junction in the style of McIntyre/Schumacher/Woodhouse and Smith's
 * reflection-function bow. With v_h the string velocity at the bow without bow force and
 * D = v_bow - v_h, the bow adds f = R(|D|)*D to both outgoing waves:
 *   sticking  |D| <= ds:  R = 1 (the string moves with the bow)
 *   slipping  |D| >  ds:  f = ds * (c + a*t) / (c + t), t = |D| - ds, i.e. a hyperbolic friction
 *                         curve falling from static (ds) to dynamic friction (a*ds)
 * ds is the maximum static friction force (bow force), a = mu_d/mu_s, c the steepness.
 * R stays in [0, 1], so the junction is a contraction (it never adds energy beyond the bow's own
 * motion) for any bow force and speed: with the loss filter's gain below 1 the loop is stable.
 * The model is homogeneous: scaling bow speed, force and c together scales the motion, so the
 * timbre only depends on the force/speed ratio (Schelleng), the bow position and the losses.
 *
 * Body: parallel resonator bank (state-variable band-passes) at violin body modes, scaled down
 * in frequency for low notes (viola, cello, bass sized bodies), plus a direct path.
 *
 * Cost (per voice-sample): string+bow ~50 FP ops incl. 1 division, body ~55 FP ops.
 * Coefficients at block rate, bow parameters ramped per sample. Delay lines are static arrays in
 * normal RAM (12 KB per voice), the Engine (DTCM) is small.
 */
#ifndef VOICE_BOWED_H_
#define VOICE_BOWED_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace bowed {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 6;
static const int NECK_LEN = 2048;    // >= longest string loop (note 18: 2060 samples, clamped)
static const int BRIDGE_LEN = 1024;  // >= max bow distance from bridge (beta <= 0.4)
static const uint32_t NECK_MASK = NECK_LEN - 1, BRIDGE_MASK = BRIDGE_LEN - 1;
static const int NB = 5;             // body modes

// delay lines: normal (cached) RAM, not in the DTCM Engine
static float neck_buf[MAX_VOICES][NECK_LEN] __attribute__((aligned(32)));
static float bridge_buf[MAX_VOICES][BRIDGE_LEN] __attribute__((aligned(32)));

// test hook: per sample bow speed, string velocity at the bow, friction reflection R
#ifndef BOWED_TRACE
#define BOWED_TRACE(v, vbow, vstring, R)
#endif

using strisy::rmax;
using strisy::rmin;

// tunable parameters (set in init, the host harness may override them)
struct Params {
	real kappa_lo, kappa_hi;  // bow force/speed ratio times beta, at light and full pressure
	real kappa_over;          // extra force when pressing beyond 1
	real fric_a, fric_c, fric_cd; // dynamic/static friction ratio, curve width (rel. to speed, force)
	real noise0, noise_acc;   // bow noise (relative jitter of the friction force)
	real acc_force, acc_speed;// accent (pressure velocity) boost of force and speed
	real beta0, beta_y, beta_p;  // bow position: centre, octaves per but_y, octaves per pressure
	real eps_lo, eps_hi;      // loss filter time constant / period at light / full pressure
	real nut_round;           // finger lowpass time constant relative to the bridge one
	real t60_40;              // string decay time (s) at note 40, halves every 2 octaves
	real rel0, rel_ped, rel_lift; // release decay: base, pedal, quick lift-off factor
	real trem;                // shake -> bow speed modulation depth
	real body_rot;            // rotation -> body mode shift (octaves)
	real direct;              // body direct path
	real out_gain;
};

struct VoiceState {
	int active, quiet;
	int slip;                 // bow state: slipping (1) or sticking (0)
	int cnt;                  // samples since the last slip onset (pitch tracking)
	real corr;                // adaptive tuning correction, fraction of the period
	uint32_t w, seed;         // delay write position, noise generator
	real note_prev, note_body;
	real P;                   // smoothed pressure
	real acc;                 // accent envelope from positive pressure velocity
	real lift;                // quick release memory (negative pressure velocity)
	real by;                  // smoothed but_y
	real vb, ds, cw, nz, dn;  // per-sample ramped values at the end of the previous block
	real y, ydc, yn;          // loss filter states (bridge, its DC path, finger)
	real hp;                  // DC blocker state
	real bd[NB][2];           // body SVF states
};

struct BodyCoef { real a, ga, g2a, gk; };

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	real sr, inv_sr, c_period, two_pi_sr, pi_sr, hp_k, dc_k;
	real shake;               // smoothed accelerometer magnitude
	Params p;
	VoiceState vs[MAX_VOICES];
	real sbuf[BLOCK];         // string output of the current voice
};

// ---------------------------------------------------------------------------------------------
// helpers (no libm)

// 2^x for x in [-60, 60]: fastpow2 is only accurate for positive arguments
static inline real exp2_fast(real x) {
	x = rmin(rmax(x, -60.0f), 60.0f);
	return strisy::fastpow2(x + 64.0f) * 5.421010862e-20f;  // 2^-64
}

// sin and cos for |x| <= pi/2 (as in voice_sine.h)
static inline real cos_poly(real x) {
	real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
static inline real sin_poly(real x) {
	real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}

// atan(y/x) for x > 0, y >= 0 (Abramowitz-Stegun 4.4.49, error 1e-5)
static inline real atan_pos(real y, real x) {
	bool inv = y > x;
	real t = inv ? x / y : y / x;
	real t2 = t * t;
	real a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	return inv ? 1.5707963f - a : a;
}

// square root as the single FPU instruction (sqrtf may call libm for the errno case)
static inline float fsqrt(float x) {
#if defined(__arm__) && defined(__ARM_FP)
	float r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
	return r;
#else
	return __builtin_sqrtf(x);
#endif
}

static inline bool bad(real x, real lim) { return !(fabsf(x) < lim); }  // also true for NaN

// body modes of a violin: frequency (Hz), Q, gain
static const real body_f[NB] = {275.0f, 460.0f, 565.0f, 1150.0f, 2700.0f};
static const real body_q[NB] = {12.0f, 16.0f, 14.0f, 6.0f, 1.8f};
static const real body_g[NB] = {0.8f, 1.0f, 0.9f, 0.55f, 0.75f};

static inline void reset_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	for (int i = 0; i < NECK_LEN; i++) neck_buf[v][i] = 0.0f;
	for (int i = 0; i < BRIDGE_LEN; i++) bridge_buf[v][i] = 0.0f;
	int active = s.active;
	uint32_t seed = s.seed;
	real note_prev = s.note_prev, note_body = s.note_body, dn = s.dn, corr = s.corr;
	s = VoiceState();
	s.active = active;
	s.seed = seed;
	s.note_prev = note_prev;
	s.note_body = note_body;
	s.dn = dn;
	s.corr = corr;
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.c_period = e.sr / 8.1757989156f;
	e.two_pi_sr = 6.2831853f / e.sr;
	e.pi_sr = 3.14159265f / e.sr;
	e.hp_k = 6.2831853f * 25.0f / e.sr;  // DC blocker, 25 Hz
	e.dc_k = 6.2831853f * 4.0f / e.sr;   // loss filter DC path, 4 Hz
	e.shake = 1.0f;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	Params& p = e.p;
	p.kappa_lo = 0.65f; p.kappa_hi = 1.15f; p.kappa_over = 1.0f;
	p.fric_a = 0.35f; p.fric_c = 0.3f; p.fric_cd = 0.7f;
	p.noise0 = 0.03f; p.noise_acc = 0.5f;
	p.acc_force = 2.0f; p.acc_speed = 0.2f;
	p.beta0 = 0.11f; p.beta_y = 1.3f; p.beta_p = 0.5f;
	p.eps_lo = 0.012f; p.eps_hi = 0.005f;
	p.nut_round = 1.0f;
	p.t60_40 = 4.0f;
	p.rel0 = 0.12f; p.rel_ped = 3.0f; p.rel_lift = 6.0f;
	p.trem = 1.3f;
	p.body_rot = 0.7f;
	p.direct = 0.3f;
	p.out_gain = 5.0f;
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.vs[v] = VoiceState();
		e.vs[v].seed = 0x9e3779b9u * (uint32_t)(v + 1);
		e.vs[v].note_prev = e.vs[v].note_body = 69.0f;
		e.vs[v].dn = 100.0f;
		e.vs[v].corr = 0.003f;
		reset_voice(e, v);
	}
}

// one voice over a block, added into out
static inline void process_voice(Engine& e, int v, real trem, real body_shift, real* __restrict__ out) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	const Params& p = e.p;
	const real pres = rmin(rmax(c.pres, 0.0f), 1.5f);  // (also maps NaN to 0)
	if (!s.active) {
		if (pres <= 0.0f) {
			s.note_prev = c.note;
			return;
		}
		s.active = 1;
		s.quiet = 0;
	}
	const real note = rmin(rmax(c.note, 18.0f), 116.0f);
	const bool jump = fabsf(note - s.note_prev) >= 1.0f;
	s.note_prev = c.note;

	// --- block rate control
	s.P += 0.3f * (pres - s.P);
	if (pres <= 0.0f && s.P < 1e-4f) s.P = 0.0f;
	const real P = s.P;
	const real Pn = rmin(P, 1.0f);
	const real vp = rmin(rmax(c.vpres, -1.5f), 1.5f);
	s.acc = rmax(s.acc * 0.984f, vp - 0.04f);               // ~40 ms accent decay
	const real lift_now = rmax(-vp - 0.04f, 0.0f);
	s.lift = pres > 0.0f ? rmax(s.lift * 0.95f, lift_now) : rmax(s.lift, lift_now);
	s.by += 0.15f * (rmin(rmax(c.but_y, -1.0f), 1.0f) - s.by);
	if (jump) s.note_body = note;
	s.note_body += 0.005f * (note - s.note_body);           // slow: vibrato doesn't move the body

	const real p2 = strisy::fastpow2(note * (1.0f / 12.0f));
	const real f0 = 8.1757989156f * p2;
	const real L = rmin(e.c_period / p2, (real)(NECK_LEN - 8));
	const real w0 = e.two_pi_sr * f0;                         // <= 0.86 rad
	// bow position: tilt forward = towards the bridge (sul ponticello), back = sul tasto;
	// a light bow drifts towards the fingerboard (flautando), a heavy one towards the bridge
	const real eb = rmin(rmax(-p.beta_y * s.by + p.beta_p * (0.5f - Pn), -1.0f), 1.8f);
	// 0.055 .. 0.38, at least 2 samples from the bridge (the highest notes)
	// (and on the longest strings not too close to it: the motion gets irregular there)
	const real beta = rmax(p.beta0 * exp2_fast(eb), rmax(2.2f / L, 0.03f + 6e-5f * L));
	// string losses: a heavier bow gives sharper corners (brighter)
	// corner rounding time (one pole time constant) as a fraction of the period, like the
	// finite bow width; the sampled loop needs no less than about a third of a sample
	const real tau = rmax((p.eps_lo + (p.eps_hi - p.eps_lo) * Pn) * L, 0.3f);
	const real pole = tau / (1.0f + tau);
	const real t60s = rmin(rmax(p.t60_40 * exp2_fast((40.0f - note) * (1.0f / 24.0f)), 0.3f), 8.0f);
	real t60 = t60s;
	if (pres <= 0.0f) {
		const real ped = e.g.pedal;
		t60 = rmin(t60s, (p.rel0 + p.rel_ped * ped * ped) * (1.0f + p.rel_lift * rmin(s.lift, 1.0f)));
	}
	const real gl = exp2_fast(-9.9657843f / (f0 * t60));     // loop gain per period for t60
	const real gp = gl * (1.0f - pole);
	// tuning: subtract the loss filter's phase delay at the fundamental
	// the finger (nut side) also rounds the corners: a second one pole lowpass there
	const real taun = tau * p.nut_round;
	const real polen = taun / (1.0f + taun);
	const real sw0 = sin_poly(w0), cw0 = cos_poly(w0);
	const real dfilt = (atan_pos(pole * sw0, 1.0f - pole * cw0) + atan_pos(polen * sw0, 1.0f - polen * cw0)) / w0;
	// The bow itself shifts the pitch (flattening, more with rounder corners and a bow near the
	// bridge). The slip onsets mark the periods: an adaptive correction keeps the measured period
	// at L (time constant ~40 ms, a fraction of a cent of jitter), kept across notes.
	if (jump) s.cnt = 0;
	const real Lstr = rmax(L - dfilt - s.corr * L, 3.4f);
	const real Db = rmin(rmax(beta * Lstr, 1.0f), rmin(Lstr - 2.2f, (real)(BRIDGE_LEN - 4)));
	const real Dn = Lstr - Db;
	const int ib = (int)Db;
	const real fb = Db - (real)ib;

	// bow speed and force (homogeneous: the ratio sets the timbre, the speed the loudness)
	// (pressing beyond 1 adds force, not speed: scratchier, not louder)
	const real amp = Pn * (0.3f + 0.7f * Pn);
	const real acc = rmin(s.acc, 1.5f);
	const real vb = amp * (1.0f + p.acc_speed * acc) * trem;
	// bow force: kappa = force/speed*beta. The Helmholtz motion needs kappa between a minimum that
	// grows towards the bridge (~beta^-0.55 here) and a maximum ~1.3 (above it: raucous, scratchy).
	// Light pressure sits just above the minimum (soft, flautando), full pressure near the
	// maximum (bright, focused), pressing beyond 1 or striking (accent) goes over it (scratch).
	// (towards the fingerboard the upper limit drops a little too)
	const real bq = fsqrt(p.beta0 / beta);
	const real kfl = rmin(p.kappa_lo * bq, 1.05f);
	const real khi = p.kappa_hi * rmin(bq * fsqrt(fsqrt(bq)), 1.0f);  // (beta0/beta)^0.375
	const real kap = kfl + (khi - kfl) * Pn * fsqrt(Pn) + p.kappa_over * rmax(P - 1.0f, 0.0f);
	const real rho = kap / beta * (1.0f + p.acc_force * acc);
	const real ds = rho * amp;
	// friction curve width. A steep curve (hysteresis: the slip starts with a jump) keeps the
	// Helmholtz motion clean on long strings, but on short ones (a few samples per slip) the jump
	// snaps the slip onsets to the sample grid, which adds rough subharmonic sidebands: there,
	// and near the bridge, the curve is made smooth enough to remove the jump.
	const real soft = rmin(rmax((90.0f - L) * 0.02f, 0.0f), 1.0f)
		* rmin(rmax(rmax((0.2f - beta) * 10.0f, (24.0f - L) * 0.125f), 0.0f), 1.0f);
	// the top octave (period < 15 samples) gets an even smoother curve: a gentler oscillator
	// that stays at the loop's pitch instead of locking to the sample grid
	const real soft2 = soft + rmin(rmax((15.0f - L) * 0.3f, 0.0f), 2.5f);
	// long strings (bass register) are steadier with a wider curve
	const real cw = p.fric_c * rmax(L * (1.0f / 250.0f), 1.0f) * amp + p.fric_cd * soft2 * ds + 1e-7f;
	// bow noise: jitter of the static friction. On long strings the stick phase approaches the
	// release threshold slowly, so the same jitter shakes the slip timing (the pitch) much more
	const real nz = rmin(p.noise0 * rmin(200.0f / L, 1.0f) + p.noise_acc * acc + 0.3f * rmax(P - 0.9f, 0.0f), 0.9f);

	// body coefficients
	BodyCoef bc[NB];
	{
		const real sz = rmin(exp2_fast((s.note_body - 55.0f) * 0.055f), 1.0f) * body_shift;
		for (int k = 0; k < NB; k++) {
			real g = strisy::tan0(rmin(e.pi_sr * body_f[k] * sz, 1.2f));
			real kq = 1.0f / body_q[k];
			real a = 1.0f / (1.0f + g * (g + kq));
			bc[k] = {a, g * a, g * g * a, body_g[k] * kq};
		}
	}

	// --- string and bow, per sample
	const real r = 1.0f / BLOCK;
	if (jump) s.dn = Dn;  // new note: no glide through the jump
	const real dvb = (vb - s.vb) * r, dds = (ds - s.ds) * r, dcw = (cw - s.cw) * r, dnz = (nz - s.nz) * r;
	const real ddn = (Dn - s.dn) * r;
	float* __restrict__ nbuf = neck_buf[v];
	float* __restrict__ bbuf = bridge_buf[v];
	real* __restrict__ sb = e.sbuf;
	uint32_t w = s.w, seed = s.seed;
	int slip = s.slip, cnt = s.cnt, tsum = 0, tn = 0;
	// onsets closer than 0.8 L to the last one are ignored (extra slips); periods between
	// floor(0.93 L) and ceil(1.08 L) are measured (both neighbours of a fractional period)
	const int cmin = (int)(0.8f * L), clo = (int)(0.93f * L), chi = (int)(1.08f * L) + 1;
	real ydc = s.ydc, yn = s.yn;
	const real polen1 = 1.0f - polen;
	const real dcg = gl - 1.0f, dck = e.dc_k;
	real y = s.y, vbn = s.vb, dsn = s.ds, cwn = s.cw, nzn = s.nz, dn = s.dn;
	const real fa = p.fric_a, fa1 = 1.0f - p.fric_a;
	real peak = 0.0f;
	for (int n = 0; n < BLOCK; n++) {
		vbn += dvb; dsn += dds; cwn += dcw; nzn += dnz; dn += ddn;
		// neck side: 3rd order Lagrange at delay dn (taps at i-1 .. i+2, d = fr + 1)
		const int i = (int)dn;
		const real fr = dn - (real)i;
		const real fm1 = fr - 1.0f, fm2 = fr - 2.0f, fp1 = fr + 1.0f;
		const real cd = fm1 * fm2, ab = fp1 * fr;
		const uint32_t r0 = w - (uint32_t)(i - 1);
		const real nut_in = (-1.0f / 6.0f) * fr * cd * nbuf[r0 & NECK_MASK]
			+ 0.5f * fp1 * cd * nbuf[(r0 - 1) & NECK_MASK]
			- 0.5f * ab * fm2 * nbuf[(r0 - 2) & NECK_MASK]
			+ (1.0f / 6.0f) * ab * fm1 * nbuf[(r0 - 3) & NECK_MASK];
		// bridge side: linear interpolation at the block-constant delay Db
		const real b0 = bbuf[(w - (uint32_t)ib) & BRIDGE_MASK];
		const real b1 = bbuf[(w - (uint32_t)ib - 1u) & BRIDGE_MASK];
		const real br_in = b0 + fb * (b1 - b0);
		y = gp * br_in + pole * y;                    // loss filter
		ydc += dck * (br_in - ydc);                   // its DC gain is restored to 1 (rigid bridge at DC)
		yn = polen * yn + polen1 * nut_in;             // finger
		const real bref = dcg * ydc - y, nref = -yn;
		const real delta = vbn - (bref + nref);
		// friction with bow noise (jitter of the static friction)
		seed = seed * 1664525u + 1013904223u;
		const real dse = dsn * (1.0f + nzn * (real)(int32_t)seed * 4.6566129e-10f);
		// slip branch: relative speed u >= 0 with x = u + dse*(a + (1-a)*c/(c+u))
		const real x = fabsf(delta);
		const real A = x - fa * dse, B = A - cwn;
		const real disc = B * B + 4.0f * cwn * (A - fa1 * dse);
		const real u = 0.5f * (B + fsqrt(rmax(disc, 0.0f)));
		const int slip_new = (x > dse) | (slip & (disc >= 0.0f) & (u > 0.0f));
		cnt++;
		if (slip_new & !slip) {  // slip onset: one per period in Helmholtz motion
			if (cnt > cmin) {
				if (cnt >= clo && cnt <= chi) { tsum += cnt; tn++; }
				cnt = 0;
			}
		}
		slip = slip_new;
		const real f = slip ? (delta > 0.0f ? x - u : u - x) : delta;
		BOWED_TRACE(v, vbn, bref + nref + f, slip ? 0.0f : 1.0f);
		nbuf[w & NECK_MASK] = bref + f;
		bbuf[w & BRIDGE_MASK] = nref + f;
		w++;
		sb[n] = br_in;
		peak = rmax(peak, fabsf(br_in));
	}
	s.w = w; s.seed = seed; s.y = y; s.slip = slip; s.cnt = cnt < 1000000 ? cnt : 1000000; s.ydc = ydc; s.yn = yn;
	if (tn > 0) {
		const real kad = rmin(0.25f, L * e.inv_sr * 25.0f);  // per period: T / 40 ms
		s.corr = rmin(rmax(s.corr + kad * ((real)tsum - (real)tn * L) / L, -0.02f), 0.04f);
	}
	s.vb = vbn; s.ds = dsn; s.cw = cwn; s.nz = nzn; s.dn = dn;

	// --- body, per sample. The output is scaled with beta so the bow position is a timbre
	// control rather than a loudness one (the string velocity grows as 1/beta)
	{
		const real gain = p.out_gain * beta;
		const real dir = p.direct;
		const real hk = e.hp_k;
		real hp = s.hp;
		real q01 = s.bd[0][0], q02 = s.bd[0][1], q11 = s.bd[1][0], q12 = s.bd[1][1];
		real q21 = s.bd[2][0], q22 = s.bd[2][1], q31 = s.bd[3][0], q32 = s.bd[3][1];
		real q41 = s.bd[4][0], q42 = s.bd[4][1];
		real opeak = 0.0f;
		for (int n = 0; n < BLOCK; n++) {
			const real xin = sb[n] - hp;
			hp += hk * xin;
			real v1, v2, acc_o = dir * xin;
			SVF_TICK(xin, q01, q02, bc[0], v1, v2); acc_o += bc[0].gk * v1;
			SVF_TICK(xin, q11, q12, bc[1], v1, v2); acc_o += bc[1].gk * v1;
			SVF_TICK(xin, q21, q22, bc[2], v1, v2); acc_o += bc[2].gk * v1;
			SVF_TICK(xin, q31, q32, bc[3], v1, v2); acc_o += bc[3].gk * v1;
			SVF_TICK(xin, q41, q42, bc[4], v1, v2); acc_o += bc[4].gk * v1;
			const real o = gain * acc_o;
			sb[n] = o;
			opeak = rmax(opeak, fabsf(o));
		}
		s.hp = hp;
		s.bd[0][0] = q01; s.bd[0][1] = q02; s.bd[1][0] = q11; s.bd[1][1] = q12;
		s.bd[2][0] = q21; s.bd[2][1] = q22; s.bd[3][0] = q31; s.bd[3][1] = q32;
		s.bd[4][0] = q41; s.bd[4][1] = q42;
		peak = rmax(peak, opeak);
	}

	// --- safety: non-finite or runaway state resets the voice (cannot happen in normal operation)
	if (bad(y, 1e3f) || bad(s.hp + s.bd[0][0] + s.bd[1][0] + s.bd[2][0] + s.bd[3][0] + s.bd[4][0], 1e4f) || bad(peak, 1e3f)) {
		reset_voice(e, v);
		return;
	}
	for (int n = 0; n < BLOCK; n++) out[n] += sb[n];
	// --- gating: idle once released and the whole loop has decayed
	if (pres <= 0.0f && peak < 2e-6f) {
		if (++s.quiet * BLOCK > (int)L + 2 * BLOCK) s.active = 0;
	} else {
		s.quiet = 0;
	}
}

static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

static inline void process(Engine& e, real* __restrict__ output) {
	for (int n = 0; n < BLOCK; n++) output[n] = 0.0f;
	// shaking the instrument moves the bow: speed modulation, strong shakes reverse it (tremolo)
	const real acc_abs = rmin(rmax(e.g.acc_abs, 0.0f), 4.0f);
	e.shake += 0.3f * (acc_abs - e.shake);
	if (bad(e.shake, 10.0f)) e.shake = 1.0f;
	const real trem = 1.0f - e.p.trem * rmin(rmax(e.shake - 1.08f, 0.0f), 1.4f);
	// rotation shifts the body resonances
	const real rot = rmin(rmax(e.g.rot_x + e.g.rot_y + e.g.rot_z, -1.0f), 1.0f);
	const real body_shift = exp2_fast(e.p.body_rot * rot);
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v, trem, body_shift, output);
	// safety soft limiter: linear up to 4, then smoothly towards 8 (C1 at the knee)
	for (int n = 0; n < BLOCK; n++) {
		const real x = output[n];
		const real a = rmax(fabsf(x) - 4.0f, 0.0f) * 0.25f;
		output[n] = x * (1.0f + 2.0f * a) / ((1.0f + a) * (1.0f + a));
	}
}

} // namespace bowed

#endif // VOICE_BOWED_H_
