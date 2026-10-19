/**
 * Reed: physically modelled single-reed woodwind, from clarinet to saxophone.
 * Build with `make VOICE=reed` (API: see the comment at the top of synth_strisy.cpp).
 *
 * Model (per voice):
 *
 *   breath (gamma) --> reed: mass-spring reed (displacement x, resonance >= 3.4 kHz and >= 6 f0,
 *                      lip damping q = 0.5) and Bernoulli flow
 *                        u = zeta * max(1 + x, 0) * sqrt|gamma - p| * sign(gamma - p)
 *                      solved exactly per sample for the mouthpiece pressure p = V + u
 *                      (Guillemain, Kergomard & Voinier 2005: explicit quadratic solution).
 *                      The reed beats (closes) for part of each period when blown harder.
 *
 *   bore: two waveguides meeting at the mouthpiece, like a string blown at a point
 *     main tube   (round trip D1) -> bell / tone holes: -g * 2-pole Butterworth lowpass; above its
 *                 cutoff (0.9-2.3 kHz by tilt/breath, >= 2 f0) the bore has no resonances, so the reed can't squeak
 *     short tube  (round trip D2) -> open end: -gs * one-pole lowpass (the missing apex of a cone)
 *   D1 + D2 = one period. With D2 = D1 (beta = 1/2) the pair is exactly a cylinder: clarinet, odd
 *   harmonics. A short D2 (beta = 0.28) behaves like a truncated cone: saxophone, all harmonics.
 *   The apex lowpass (6 f0, faded in with the morph) evens out the cone's impedance peaks so the
 *   fundamental register is preferred.
 *
 *   output: wave transmitted at the bell + some internal wave (body) + a presence band (2.2-2.6 kHz)
 *   whose level grows with breath (soft = dark, loud = bright), + breath noise.
 *
 * Tuning: the loop delay is the period minus the loop filters' phase delays at f0 (computed per
 * block with polynomial sin/cos/atan) minus a measured correction table (reed dynamics and the
 * inharmonicity of the lossy apex; tools/harness/out/reed_tune.h, within ~5 cents up to ~C7 at
 * normal pressures). The main delay is read with 3rd order Lagrange interpolation, the short one
 * linearly; both delay lengths are ramped per sample inside the block, so vibrato and tilt are
 * click free.
 *
 * Controls:
 *   pres   breath. Below ~0.12 (clarinet) .. 0.2 (sax) the reed doesn't speak: breath noise and a
 *          whispery bore resonance. Above it the tone blooms and gets louder and brighter;
 *          pushed hard (> ~1.05) a growl (hummed roughness) sets in. The register stays on the
 *          fundamental over the whole pressure/tilt range (tested on notes 36..91).
 *   vpres  tonguing: a fast strike gives a pressure kick and a short tongue/breath noise burst
 *          (only while the key is pressed; a stale vpres on a released voice is ignored)
 *   but_y  backward = clarinet (cylindrical, hollow, odd harmonics, tighter embouchure),
 *          forward = saxophone (conical, all harmonics, buzzier, more open embouchure).
 *          High notes (above ~E5) are pulled towards the middle of the morph.
 *   note   pitch, including vibrato/bends (sideways tilt)
 *   shake  (acc_abs > ~1.15) adds growl
 * The per-sample loop has no divisions and no libm: one hardware vsqrt per sample on the board.
 */
#ifndef VOICE_REED_H_
#define VOICE_REED_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace reed {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 10;

// delay lines: main tube up to (1 - BETA_SAX) * period, short tube up to period / 2.
// The lowest note (MIDI 20, 26 Hz) has a period of 1881 samples.
static const int MAIN_LEN = 2048;
static const int SHORT_LEN = 2048;
static const real NOTE_MIN = 18.0f;   // lower notes are clamped (period limited by the buffers)
static const real NOTE_MAX = 122.0f;

// ---------------------------------------------------------------------------------------------
// Voice parameters (tuned on the host harness)

static const real BETA_CLAR = 0.5f;      // apex/period ratio at full backward tilt (cylinder)
#ifndef REED_BSAX
#define REED_BSAX 0.28f
#endif
#ifndef REED_ZS
#define REED_ZS 0.14f
#endif
static const real BETA_SAX = REED_BSAX;   // at full forward tilt (cone)
static const real P_TH = 0.12f;          // pressure where gamma reaches G_TH (about where the reed speaks)
static const real G_TH = 0.36f;
static const real G_SLOPE = 0.46f;       // gamma per unit pressure above P_TH
static const real G_MAX = 0.94f;         // gamma > 1 would choke the reed
static const real GS = 0.997f;           // short tube end reflection
#ifndef REED_FR
#define REED_FR 4000.0f
#endif
#ifndef REED_QR
#define REED_QR 0.5f
#endif
#ifndef REED_FRK
#define REED_FRK 6.0f
#endif
#ifndef REED_FC0
#define REED_FC0 1100.0f
#endif
#ifndef REED_FC1
#define REED_FC1 1800.0f
#endif
#ifndef REED_FCTRACK
#define REED_FCTRACK 2.0f
#endif
#ifndef REED_GTH
#define REED_GTH 1.05f
#endif
#ifndef REED_NG
#define REED_NG 0.02f
#endif
#ifndef REED_NO
#define REED_NO 0.04f
#endif
#ifndef REED_FCS
#define REED_FCS 6.0f
#endif

static inline real fsqrt(real x) {
#if defined(__arm__) && defined(__ARM_FP)
	real r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
	return r;
#else
	return __builtin_sqrtf(x);
#endif
}

static inline real clampr(real x, real lo, real hi) { return strisy::rmin(strisy::rmax(x, lo), hi); }

// measured tuning correction: samples of extra loop delay (reed dynamics, inharmonicity of the
// lossy cone apex), for the clarinet (m = 0), middle and saxophone (m = 1) points of the morph, at notes
// 20 + 7k (k = 0..13), interpolated linearly. Measured with tools/harness/out/reed_tune.h.
static const real CORR_CLAR[14] = {1.02f, 1.29f, 1.35f, 1.35f, 1.34f, 1.27f, 1.15f, 0.91f, 0.83f, 0.57f, 0.38f, 0.36f, 0.41f, 0.38f};
static const real CORR_SAX[14] = {-4.92f, -2.94f, -1.73f, -0.89f, -0.22f, 0.29f, 0.66f, 0.96f, 1.01f, 0.61f, 0.43f, 0.33f, 0.29f, 0.48f};
static const real CORR_MID[14] = {0.33f, 0.56f, 0.47f, 0.49f, 0.46f, 0.50f, 0.63f, 0.79f, 1.00f, 0.65f, 0.45f, 0.37f, 0.44f, 0.43f};
static inline real tuning_corr(real note, real m) {
	real k = clampr((note - 20.0f) * (1.0f / 7.0f), 0.0f, 12.999f);
	int i = (int)k;
	real f = k - (real)i;
	real c0 = CORR_CLAR[i] + f * (CORR_CLAR[i + 1] - CORR_CLAR[i]);
	real ch = CORR_MID[i] + f * (CORR_MID[i + 1] - CORR_MID[i]);
	real c1 = CORR_SAX[i] + f * (CORR_SAX[i + 1] - CORR_SAX[i]);
	// quadratic through m = 0, 1/2, 1
	return c0 + m * (4.0f * ch - 3.0f * c0 - c1) + m * m * (2.0f * c0 + 2.0f * c1 - 4.0f * ch);
}

// 2^p for any |p| < 60 (strisy::fastpow2 is only valid for p >= 0), relative error ~1e-7; block rate
static inline real pow2(real p) {
	p = clampr(p, -60.0f, 60.0f);
	int w = (int)p;
	if ((real)w > p) w--;
	const real z = p - (real)w;
	const real f = 1.0f + z * (0.693146839f + z * (0.240231090f + z * (0.0554789288f + z * (0.00968616940f + z * (0.00123824704f + z * 0.000218713408f)))));
	union { uint32_t i; float f; } e = { (uint32_t)(w + 127) << 23 };
	return f * e.f;
}

// sin/cos for |x| <= pi/2 (as in voice_sine.h)
static inline real cos_poly(real x) {
	real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
static inline real sin_poly(real x) {
	real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
// atan for t >= 0, error < 1e-5 (odd minimax polynomial on [0, 1] + reflection); block rate only
static inline real atan_pos(real t) {
	bool inv = t > 1.0f;
	real z = inv ? 1.0f / t : t;
	real z2 = z * z;
	real r = z * (0.99997726f + z2 * (-0.33262347f + z2 * (0.19354346f + z2 * (-0.11643287f + z2 * (0.05265332f + z2 * (-0.01172120f))))));
	return inv ? 1.5707963f - r : r;
}

// atan2 from atan_pos; block rate only
static inline real atan2_r(real y, real x) {
	const real ax = fabsf(x), ay = fabsf(y);
	if (ax + ay <= 0.0f) return 0.0f;
	real a = ax >= ay ? atan_pos(ay / ax) : 1.5707963f - atan_pos(ax / ay);
	if (x < 0.0f) a = 3.1415927f - a;
	return y < 0.0f ? -a : a;
}

struct VoiceState {
	int active;
	int quiet;           // consecutive quiet blocks after release
	uint32_t widx;       // delay write index
	uint32_t rng;        // noise generator
	uint32_t gph;        // growl oscillator phase
	real note_prev;
	real pres_s;         // smoothed pressure
	real gam;            // breath gamma at the end of the last block
	real by_s;           // smoothed tilt
	real tongue;         // articulation envelope
	real growl;          // smoothed growl amount
	real d1, d2;         // delay lengths at the end of the last block
	real lp, lp2;        // bell lowpass state
	real pr1, pr2;       // presence filter state
	real ls;             // short tube lowpass state
	real x1, x2;         // reed displacement x(n-1), x(n-2)
	real pm1, gm1;       // p(n-1), gamma(n-1)
	real n1, n1b, n2;    // noise filter states
	real gn;             // growl noise lowpass
	real outg;           // output gain at the end of the last block
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	real sr;
	real shake;          // smoothed shake amount from acc_abs
	real dc1;            // master DC blocker state
	real dcx;
	VoiceState vs[MAX_VOICES];
	real mix[BLOCK];
};

// delay lines in normal (cached AXI) RAM, outside the DTCM Engine
static float reed_main[MAX_VOICES][MAIN_LEN];
static float reed_short[MAX_VOICES][SHORT_LEN];

static inline void reset_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	uint32_t seed = 0x9E3779B9u * (uint32_t)(v + 1);
	real note = s.note_prev;
	s = VoiceState();
	s.rng = seed;
	s.note_prev = note;
	s.d1 = 100.0f; s.d2 = 100.0f;
	for (int i = 0; i < MAIN_LEN; i++) reed_main[v][i] = 0.0f;
	for (int i = 0; i < SHORT_LEN; i++) reed_short[v][i] = 0.0f;
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	e.shake = 0.0f;
	e.dc1 = 0.0f; e.dcx = 0.0f;
	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.vs[v].note_prev = 69.0f;
		reset_voice(e, v);
	}
}

// breath pressure -> gamma (mouth pressure / reed closing pressure)
static inline real gamma_of(real p) {
	return p < P_TH ? p * (G_TH / P_TH) : G_TH + G_SLOPE * (p - P_TH);
}

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	real pres = clampr(c.pres, 0.0f, 1.5f);
	if (!(pres == pres)) pres = 0.0f;
	real vp = clampr(c.vpres - 0.02f, 0.0f, 1.0f);
	if (!(vp == vp)) vp = 0.0f;
	// velocity only counts while the key is pressed: the firmware leaves the last vpres on a
	// released voice, which must neither wake it nor keep it (and its tongue noise) alive
	const real vplev = pres > 0.0f ? vp / (0.3f + vp) : 0.0f;

	bool wake = false;
	if (!s.active) {
		if (pres <= 0.0f) { s.note_prev = c.note; return; }
		s.active = 1;
		s.quiet = 0;
		wake = true;
	}
	real note = c.note;
	if (!(note == note)) note = 60.0f;
	note = clampr(note, NOTE_MIN, NOTE_MAX);
	const bool jump = fabsf(note - s.note_prev) >= 1.0f;
	s.note_prev = note;

	// ---- block-rate controls
	const real sr = e.sr;
	s.pres_s += 0.25f * (pres - s.pres_s);            // ~2.5 ms
	if (s.pres_s < 1e-6f) s.pres_s = 0.0f;            // no denormals in the decays
	const real ps = s.pres_s;
	s.by_s += 0.03f * (clampr(c.but_y, -1.0f, 1.0f) - s.by_s);  // ~20 ms
	s.tongue = strisy::rmax(s.tongue * 0.96f, vplev);  // ~25 ms decay
	if (s.tongue < 1e-6f) s.tongue = 0.0f;
	const real tg = s.tongue;

	// morph 0 clarinet .. 1 sax. Above ~E5 the cone model overblows easily (real saxophones end
	// around there too), so high notes are pulled towards the middle of the morph
	const real m = (0.5f + 0.5f * s.by_s) * (1.0f - 0.5f * clampr((note - 76.0f) * (1.0f / 16.0f), 0.0f, 1.0f));
	// period in samples and the fundamental's angular frequency
	const real P = (sr * (1.0f / 440.0f)) * pow2((69.0f - note) * (1.0f / 12.0f));
	const real f0 = sr / P;
	const real w0 = 6.2831853f / P;

	// bell / tone hole lattice: 2-pole Butterworth lowpass in the main loop (above the cutoff the
	// bore has no resonances, so the reed can't squeak there); the cutoff tracks pitch for high notes
	real fc = (REED_FC0 + (REED_FC1 - REED_FC0) * m) * (0.85f + 0.3f * ps);
	fc = strisy::rmax(fc, REED_FCTRACK * f0);
	const real K = strisy::tan0(strisy::rmin(fc * 3.1415927f / sr, 1.2f));
	const real bnorm = 1.0f / (1.0f + 1.4142136f * K + K * K);
	const real bb0 = K * K * bnorm;
	const real ba1 = 2.0f * (K * K - 1.0f) * bnorm;
	const real ba2 = (1.0f - 1.4142136f * K + K * K) * bnorm;
	// its phase delay at f0: numerator (1 + z^-1)^2 is 1 sample, plus the denominator's phase
	const real sw = sin_poly(w0), cw = cos_poly(w0);
	const real c2w = 2.0f * cw * cw - 1.0f, s2w = 2.0f * sw * cw;
	const real tau = 1.0f + atan2_r(-(ba1 * sw + ba2 * s2w), 1.0f + ba1 * cw + ba2 * c2w) / w0;

	// reed: mass-spring, centered finite differences (stable for omega*T < 2)
	const real zeta = 0.30f + REED_ZS * m;
	real fr = (REED_FR - 600.0f * m);
	fr = strisy::rmax(fr, REED_FRK * f0);
	const real wr = strisy::rmin(fr * 6.2831853f / sr, 1.4f);
	const real qr = REED_QR;
	const real A = 1.0f / (wr * wr), B = qr / (2.0f * wr);
	const real ia0 = 1.0f / (A + B);
	const real rb = ia0, r1 = (2.0f * A - 1.0f) * ia0, r2 = -(A - B) * ia0;

	// delay lengths: D1 + D2 + tau + correction = P
	const real beta = BETA_CLAR + (BETA_SAX - BETA_CLAR) * m;
	// short tube (cone apex) reflection lowpass: makes the impedance peaks of the higher modes
	// even, so the fundamental is preferred; off for the clarinet (keeps the odd harmonics)
	const real cps = clampr((m - 0.2f) * 2.5f, 0.0f, 1.0f) * pow2(-REED_FCS * f0 * (6.2831853f * 1.442695f) / sr);
	const real cbs = 1.0f - cps;
	const real taus = atan_pos(cps * sw / (1.0f - cps * cw)) / w0;
	real D2 = clampr(beta * P - taus, 1.0f, (real)(SHORT_LEN - 2));
	const real corr = tuning_corr(note, m);
	real D1 = clampr(P - D2 - taus - tau - corr, 2.0f, (real)(MAIN_LEN - 5));
	if (jump || wake) { s.d1 = D1; s.d2 = D2; }  // new note: no delay glide
	// end reflections. At tiny amplitudes the Bernoulli reed is an open end (p = 0), so after
	// release the tubes would ring on their own end losses for seconds: when the breath is gone,
	// add a loss that makes them die out in ~0.1 s (T60), whatever the tube length
	const real rel = clampr(1.0f - ps * 20.0f, 0.0f, 1.0f) * (9.97f / (0.1f * 48828.0f));
	const real g = 0.985f * pow2(-rel * D1);
	const real gs = GS * pow2(-rel * D2);
	const real d1s = s.d1, d2s = s.d2;
	const real dd1 = (D1 - d1s) * (1.0f / BLOCK), dd2 = (D2 - d2s) * (1.0f / BLOCK);

	// breath gamma with tongue kick, interpolated over the block
	const real gam_t = strisy::rmin(gamma_of(ps) * (1.0f + 0.35f * tg), G_MAX);
	const real gam0 = s.gam;
	const real dgam = (gam_t - gam0) * (1.0f / BLOCK);
	// breath noise: relative noise on gamma (turbulence, more when soft) and audible breath
	// (band-limited noise, 500 Hz .. 3 kHz). The audible breath rises with the square of the
	// pressure up to about the speaking threshold, then fades as the reed takes over; the
	// tongue adds a short burst at note onsets.
	const real namt = REED_NG * (0.5f + (1.0f - clampr(ps, 0.0f, 1.0f))) + 0.15f * tg;
	const real nr = clampr(ps * (1.0f / 0.15f), 0.0f, 1.0f);
	const real nout = REED_NO * nr * nr * (1.0f - 0.85f * clampr((ps - 0.1f) * 4.0f, 0.0f, 1.0f)) + 0.06f * tg;
	// growl: pushed hard, or shaking
	const real gr_t = clampr((ps - REED_GTH) * 3.0f, 0.0f, 1.0f) + e.shake;
	s.growl += 0.05f * (strisy::rmin(gr_t, 1.2f) - s.growl);
	if (s.growl < 1e-6f) s.growl = 0.0f;
	const real gdep = 0.45f * s.growl;
	const real gf = clampr(0.5f * f0 + 3.0f, 25.0f, 140.0f);
	const uint32_t ginc = (uint32_t)(gf * (4294967296.0f / 48828.0f));
	// output gain: more dynamic range than the physical amplitude alone
	const real outg_t = 1.2f * (0.30f + 0.70f * clampr(ps, 0.0f, 1.2f));
	// presence band: k * bandpass, Q 0.9, around 2.2 (clarinet) .. 2.6 kHz (sax)
	const strisy::SvfCoef prc = strisy::svf_from_g(strisy::tan0((2200.0f + 400.0f * m) * 3.1415927f / sr), 0.9f);
	// soft = darker (more internal wave), loud = brighter (more presence)
	const real psc = clampr(ps, 0.0f, 1.3f);
	const real kpr = (2.0f + 2.0f * m) * (0.1f + 0.9f * psc * psc) * prc.k;
	const real kbi = (0.5f - 0.3f * m) * (1.8f - psc);
	const real outg0 = s.outg;
	const real doutg = (outg_t - outg0) * (1.0f / BLOCK);

	// ---- per-sample loop
	float* __restrict__ mb = reed_main[v];
	float* __restrict__ sb = reed_short[v];
	real* __restrict__ mix = e.mix;
	uint32_t widx = s.widx, rng = s.rng, gph = s.gph;
	real pr1 = s.pr1, pr2 = s.pr2;
	real lp = s.lp, lp2 = s.lp2, ls = s.ls, x1 = s.x1, x2 = s.x2, pm1 = s.pm1, gm1 = s.gm1;
	real n1 = s.n1, n1b = s.n1b, n2 = s.n2, gn = s.gn;
	real d1 = d1s, d2 = d2s, gam = gam0, og = outg0;
	real peak = 0.0f;
	for (int n = 0; n < BLOCK; n++) {
		d1 += dd1; d2 += dd2; gam += dgam; og += doutg;
		// noise: white -> band 500 Hz .. 3 kHz
		rng = rng * 1664525u + 1013904223u;
		const real wn = (real)(int32_t)rng * (1.0f / 2147483648.0f);
		n1 += 0.32f * (wn - n1);     // 2 x one-pole lowpass at ~3 kHz
		n1b += 0.32f * (n1 - n1b);
		n2 += 0.064f * (n1b - n2);   // minus a lowpass at ~500 Hz
		const real nz = n1b - n2;
		// growl modulator: parabolic sine (the hum) plus a little slow noise
		gph += ginc;
		const real gs_ = (real)(int32_t)gph * (1.0f / 2147483648.0f);
		const real gsin = 4.0f * gs_ * (1.0f - fabsf(gs_));
		gn += 0.01f * (wn - gn);
		const real gmod = gsin + 2.0f * gn;
		const real gamn = gam * (1.0f + namt * nz + gdep * gmod);

		// main tube: Lagrange-3 read at delay d1 (taps i0+1 .. i0+4 samples back)
		const int i0 = (int)d1 - 1;
		const real d = d1 - (real)i0;          // in [1, 2)
		const real dm1 = d - 1.0f, dm2 = d - 2.0f, dm3 = d - 3.0f;
		const real p01 = d * dm1, p23 = dm2 * dm3;
		const real h0 = -(dm1 * p23) * (1.0f / 6.0f);
		const real h1 = (d * p23) * 0.5f;
		const real h2 = -(p01 * dm3) * 0.5f;
		const real h3 = (p01 * dm2) * (1.0f / 6.0f);
		const uint32_t b0 = widx - (uint32_t)i0;
		const real bi = h0 * mb[b0 & (MAIN_LEN - 1)] + h1 * mb[(b0 - 1u) & (MAIN_LEN - 1)]
				+ h2 * mb[(b0 - 2u) & (MAIN_LEN - 1)] + h3 * mb[(b0 - 3u) & (MAIN_LEN - 1)];
		// bell reflection
		const real yl = bb0 * bi + lp;
		lp = 2.0f * bb0 * bi - ba1 * yl + lp2;
		lp2 = bb0 * bi - ba2 * yl;
		const real am = -g * yl;
		// short tube: linear read at delay d2 (>= 1)
		const int j0 = (int)d2;
		const real fr2 = d2 - (real)j0;
		const uint32_t c0 = widx - (uint32_t)j0;
		const real s0 = sb[c0 & (SHORT_LEN - 1)], s1 = sb[(c0 - 1u) & (SHORT_LEN - 1)];
		ls += cbs * (s0 + fr2 * (s1 - s0) - ls);
		const real bm = -gs * ls;
		const real V = am + bm;

		// reed displacement from the previous pressure difference
		const real x = rb * (pm1 - gm1) + r1 * x1 + r2 * x2;
		x2 = x1; x1 = x;
		const real W = zeta * strisy::rmax(1.0f + x, 0.0f);
		const real X = gamn - V;
		const real aX = fabsf(X);
		const real uabs = 0.5f * W * (fsqrt(W * W + 4.0f * aX) - W);
		const real u = X >= 0.0f ? uabs : -uabs;
		const real p = V + u;
		pm1 = p; gm1 = gamn;

		mb[widx & (MAIN_LEN - 1)] = bm + u;    // a+ = p - a-
		sb[widx & (SHORT_LEN - 1)] = am + u;   // b+ = p - b-
		widx++;

		// output: bell transmission (bright) plus some internal wave (body), a presence band
		// (radiation / bell brightness, grows with breath) and breath noise
		const real tx = bi + am;
		real v1, v2;
		SVF_TICK(tx, pr1, pr2, prc, v1, v2);
		(void)v2;
		const real y = og * (tx + kbi * bi + kpr * v1 + nout * nz);
		mix[n] += y;
		peak = strisy::rmax(peak, fabsf(y));
	}
	s.widx = widx; s.rng = rng; s.gph = gph;
	s.pr1 = pr1; s.pr2 = pr2;
	s.lp = lp; s.lp2 = lp2; s.ls = ls; s.x1 = x1; s.x2 = x2; s.pm1 = pm1; s.gm1 = gm1;
	s.n1 = n1; s.n1b = n1b; s.n2 = n2; s.gn = gn;
	s.d1 = D1; s.d2 = D2; s.gam = gam_t; s.outg = outg_t;

	// safety: reset a voice whose state went non-finite or absurd (also catches NaN)
	if (!(fabsf(lp) + fabsf(x1) + fabsf(pm1) + peak < 1000.0f)) {
		reset_voice(e, v);
		s.note_prev = note;
		return;
	}
	// go idle once released and quiet for longer than a period
	if (pres <= 0.0f && peak < 2e-5f) {
		if (++s.quiet > (int)(P * (1.0f / BLOCK)) + 3) s.active = 0;
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
	// shake: accelerometer magnitude above rest, smoothed per block
	const real sh = clampr((e.g.acc_abs - 1.15f) * 0.8f, 0.0f, 1.0f);
	e.shake += (sh > e.shake ? 0.2f : 0.02f) * (sh - e.shake);
	if (!(e.shake == e.shake)) e.shake = 0.0f;
	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);
	// master: DC blocker (~10 Hz) and a soft safety limiter
	real s1 = e.dc1, xp = e.dcx;
	for (int n = 0; n < BLOCK; n++) {
		real x = e.mix[n];
		x = fabsf(x) < 1e4f ? x : 0.0f;  // also catches NaN
		s1 = 0.99871f * s1 + x - xp;
		xp = x;
		// soft limit above 4 (asymptote 8), then a hard safety clamp
		const real a = fabsf(s1);
		const real o = a > 4.0f ? 4.0f + (a - 4.0f) / (1.0f + 0.25f * (a - 4.0f)) : a;
		output[n] = clampr(s1 < 0.0f ? -o : o, -8.0f, 8.0f);
	}
	if (!(fabsf(s1) < 1e6f) || fabsf(s1) + fabsf(xp) < 1e-9f) { s1 = 0.0f; xp = 0.0f; }  // NaN guard; exact 0 when silent
	e.dc1 = s1; e.dcx = xp;
}

} // namespace reed

#endif // VOICE_REED_H_
