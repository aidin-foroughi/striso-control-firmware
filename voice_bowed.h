/**
 * Bowed string voice engine: a digital waveguide string driven by a friction bow, with a small
 * modal body. Build with `make VOICE=bowed`; presets with set_preset() (host: -DBOWED_PRESET=n).
 * Presets: violin, viola, cello, double bass, string section, sul ponticello, flautando,
 * con sordino, pizzicato.
 *
 * String: two velocity-wave delay loops meeting at the bow, one for the neck side (bow -> finger
 * -> bow, read with a 4-tap Lagrange fractional delay that carries the pitch and vibrato) and one
 * for the bridge side (bow -> bridge -> bow, linear interpolation; its length is the bow
 * position). Both ends reflect through a one-pole lowpass whose time constant is a fixed
 * fraction of the period (corner rounding, like a finite bow width: the same timbre in every
 * register); the bridge one also sets the decay, with a separate DC path so the bridge stays
 * rigid at DC. The filters' phase delay at the fundamental is subtracted from the loop.
 *
 * Bow: explicit friction junction (McIntyre/Schumacher/Woodhouse, Smith). With v_h the string
 * velocity at the bow without bow force and D = v_bow - v_h, the bow adds the force f to both
 * outgoing waves:
 *   sticking: f = D (the string moves with the bow) while |D| <= ds (maximum static friction)
 *   slipping: hyperbolic friction mu(u) = a + (1-a)*c/(c+u) of the slip speed u, solved exactly
 *             (one square root) from |D| = u + ds*mu(u); f = sign(D) * (|D| - u)
 * with hysteresis (a slip continues until its branch disappears). Slip onsets and captures are
 * blended at sub-sample precision (first order anti-aliasing of the friction jump); without it
 * they snap to the sample grid, which adds a rough, raspy layer of subharmonic sidebands.
 * |f| <= |D| and sign(f) = sign(D), so the junction is a contraction for any bow force and speed:
 * with the loss filters' gain below 1 the loop is stable. The model is homogeneous: scaling bow
 * speed, force and c together scales the motion, so the timbre depends only on the force/speed
 * ratio (Schelleng), the bow position and the losses; the speed sets the loudness.
 * Pitch: the bow flattens the string a little (more with round corners and near the bridge). The
 * slip onsets mark the periods, and a slow adaptive correction keeps them at the note's period.
 *
 * Onset (v6): a bowed note starts already in Helmholtz motion. At the key press both delay lines
 * are loaded with the steady sawtooth waves for the note's pitch and bow position (the stick
 * phase begins), and the bow force is eased for the first two periods so the first corner
 * releases on time: a clean, pitched start within a fraction of a period, no transient from rest.
 * Loudness is an output gain that follows the pressure as fast as the key; the string itself
 * runs at a nominal bow speed (the model is homogeneous), so dynamics never disturb its motion.
 *
 * Pizzicato (preset / "pizz" parameter): no bow; a strike (or a new note) plucks the string with
 * a half-sine velocity pulse at the bow position, the held string rings, a release damps it.
 *
 * Body: tone shelf (pressure, bow position) and a parallel bank of 5 resonators at violin body
 * modes scaled by the instrument size (viola, cello and bass sized bodies), plus a direct path.
 * Section: two more "players" for the whole engine, read from a short delay of the mix with slow
 * independent delay modulation (a few cents of detune, 10-30 ms of timing spread).
 *
 * Cost per voice-sample: string+bow loop ~185 instructions on the usual path (~65 FP incl. 1 vsqrt; 1 division only
 * at slip onsets), body loop ~55 (~35 FP); block-rate coefficients ~12 divisions. Silent voices are
 * skipped. Section: ~20 instructions per sample for the whole engine. Memory: delay lines in ram1
 * (AHB SRAM1/2), 100 KB for 10 voices; section delay 8 KB in AXI SRAM; Engine (DTCM) ~2.5 KB.
 */
#ifndef VOICE_BOWED_H_
#define VOICE_BOWED_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace bowed {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 10;
static const int NECK_LEN = 2048;    // >= longest string loop (note 18: 2060 samples, clamped)
static const int BRIDGE_LEN = 512;   // >= max bow distance from the bridge (beta <= 0.22: 450)
static const uint32_t NECK_MASK = NECK_LEN - 1, BRIDGE_MASK = BRIDGE_LEN - 1;
static const int NB = 5;             // body modes
static const int ENS_LEN = 2048;     // section delay (42 ms)
static const uint32_t ENS_MASK = ENS_LEN - 1;

// delay lines in AHB SRAM1/2 (ram1): 100 KB for 10 voices
static float neck_buf[MAX_VOICES][NECK_LEN] __attribute__((aligned(32), section(".ram1_clear")));
static float bridge_buf[MAX_VOICES][BRIDGE_LEN] __attribute__((aligned(32), section(".ram1_clear")));
// section delay of the mix: normal static array (AXI SRAM), 8 KB
static float ens_buf[ENS_LEN] __attribute__((aligned(32)));

// test hook: per sample bow speed, string velocity at the bow, friction reflection R
#ifndef BOWED_TRACE
#define BOWED_TRACE(v, vbow, vstring, R)
#endif

using strisy::rmax;
using strisy::rmin;

// ---------------------------------------------------------------------------------------------
// Sound design parameters (all floats: they are the live parameters, see EngineParams below)

struct Params {
	real gain;            // output level
	// bow
	real force_lo, force_hi; // bow force/speed ratio (x beta, "kappa") at light and full pressure
	real force_over;      // grit when pressing beyond 1 (0 = none; beyond 1 is louder, fuller, brighter)
	real speed_curve;     // bow speed from pressure: P * (1 - c + c * P)
	real rosin;           // bow noise (jitter of the static friction)
	// bow position
	real pos;             // bow-bridge distance / string length at flat tilt and medium pressure
	real pos_tilt;        // octaves of position per unit tilt (forward = bridge)
	real pos_press;       // octaves towards the fingerboard from full to light pressure
	// string
	real damp;            // string decay time (s) at note 40 (halves every 2 octaves)
	real bright;          // corner sharpness (string loss filters): > 1 brighter
	real tone_lo, tone_hi; // tone shelf above 1.3 kHz at light / full pressure
	// body
	real body_size;       // body mode frequencies x (violin 1)
	real body_track;      // 0: fixed body, 1: the body shrinks for low notes (any register)
	real body_amt;        // resonant body level
	real body_q;          // body resonance sharpness x
	real hill;            // bridge hill (2-3 kHz brilliance of a violin body)
	real direct;          // unfiltered string in the output
	// accent (strike)
	real acc_force;       // firmer bow at a strike (limited to the top of the clean range)
	real acc_time;        // accent decay (s)
	real acc_noise;       // extra bow noise at a strike
	real acc_bright;      // extra brightness at a strike
	// release
	real rel;             // ring time (s) after a slow release (the bow stops on the string)
	real rel_ped;         // + pedal^2 * this
	real rel_lift;        // x (1 + this * lift) after a quick lift-off
	// vibrato (automatic, delayed)
	real vib;             // depth (cents)
	real vib_hz;
	real vib_delay;       // seconds after the note start until full depth
	// motion
	real trem;            // shake -> bow speed modulation (strong shakes reverse the bow)
	real body_rot;        // rotation -> body mode shift (octaves)
	// section
	real ens;             // level of the two extra players (0 = solo)
	real ens_detune;      // their pitch wander (cents)
	real ens_spread;      // their timing spread (ms)
	// pizzicato
	real pizz;            // 1: plucked instead of bowed
	real pizz_decay;      // ring time (s) at note 60 while held
	real pizz_bright;     // pluck sharpness
};

static const int NPARAMS = sizeof(Params) / sizeof(real);
static_assert(sizeof(Params) == NPARAMS * sizeof(real), "Params must hold only floats");

static inline Params violin_params() {
	Params p;
	p.gain = 1.0f;
	p.force_lo = 0.75f; p.force_hi = 0.95f; p.force_over = 1.0f; p.speed_curve = 0.7f; p.rosin = 0.01f;
	p.pos = 0.11f; p.pos_tilt = 1.0f; p.pos_press = 0.5f;
	p.damp = 4.0f; p.bright = 1.0f; p.tone_lo = 0.3f; p.tone_hi = 1.2f;
	p.body_size = 1.0f; p.body_track = 0.0f; p.body_amt = 1.0f; p.body_q = 1.0f; p.hill = 0.75f; p.direct = 0.3f;
	p.acc_force = 0.0f; p.acc_time = 0.02f; p.acc_noise = 0.04f; p.acc_bright = 0.4f;
	p.rel = 0.1f; p.rel_ped = 3.0f; p.rel_lift = 0.0f;
	p.vib = 0.0f; p.vib_hz = 5.5f; p.vib_delay = 0.35f;
	p.trem = 1.3f; p.body_rot = 0.7f;
	p.ens = 0.0f; p.ens_detune = 7.0f; p.ens_spread = 18.0f;
	p.pizz = 0.0f; p.pizz_decay = 1.0f; p.pizz_bright = 1.0f;
	return p;
}

static const int NUM_PRESETS = 9;

static inline const char* preset_name(int i) {
	static const char* const names[NUM_PRESETS] = {
		"violin", "viola", "cello", "bass", "section", "ponticello", "flautando", "sordino", "pizzicato"};
	return (i >= 0 && i < NUM_PRESETS) ? names[i] : names[0];
}

static inline Params preset_params(int i) {
	Params p = violin_params();
	switch (i) {
	case 1:  // viola: a bigger body a fifth lower, darker and more nasal
		p.gain = 1.17f;
		p.body_size = 0.8f; p.hill = 0.55f; p.body_amt = 1.15f;
		p.bright = 0.85f; p.tone_hi = 1.0f; p.damp = 4.5f;
		break;
	case 2:  // cello: low body resonances, warm and round, longer ring
		p.gain = 1.41f;
		p.body_size = 0.45f; p.hill = 0.6f; p.body_amt = 1.1f; p.direct = 0.25f;
		p.bright = 0.8f; p.tone_hi = 1.0f; p.damp = 5.0f;
		break;
	case 3:  // double bass: big body, dark, a little more rosin, long ring
		p.gain = 1.35f;
		p.body_size = 0.3f; p.hill = 0.45f; p.body_amt = 1.1f; p.direct = 0.25f;
		p.bright = 0.65f; p.tone_lo = 0.25f; p.tone_hi = 0.85f; p.damp = 6.0f;
		p.rosin = 0.02f; p.acc_time = 0.02f;
		break;
	case 4:  // string section: three players, the body follows the register, gentle vibrato
		p.gain = 1.5f;
		p.body_track = 1.0f; p.ens = 0.8f; p.ens_detune = 7.0f; p.ens_spread = 18.0f;
		p.vib = 12.0f; p.vib_hz = 5.2f; p.vib_delay = 0.4f;
		p.tone_hi = 1.1f;
		break;
	case 5:  // sul ponticello: bow at the bridge, glassy upper partials
		p.gain = 0.76f;
		p.pos = 0.075f; p.pos_tilt = 0.4f; p.pos_press = 0.2f;
		p.tone_lo = 0.6f; p.tone_hi = 1.6f; p.hill = 1.2f; p.bright = 1.3f;
		break;
	case 6:  // flautando: light fast bow over the fingerboard, soft and airy
		p.gain = 1.2f;
		p.pos = 0.15f; p.pos_tilt = 0.25f; p.pos_press = 0.2f;
		p.force_lo = 0.65f; p.force_hi = 0.85f;
		p.tone_lo = 0.2f; p.tone_hi = 0.55f; p.bright = 0.7f; p.hill = 0.4f;
		p.rosin = 0.02f;
		break;
	case 7:  // con sordino: mute on the bridge, veiled and soft-edged
		p.gain = 1.95f;
		p.hill = 0.12f; p.tone_lo = 0.2f; p.tone_hi = 0.6f; p.body_amt = 0.8f; p.direct = 0.12f;
		p.bright = 0.85f;
		break;
	case 8:  // pizzicato: plucked, any register (the body follows the note)
		p.gain = 0.6f;
		p.pizz = 1.0f; p.body_track = 1.0f; p.pizz_decay = 1.5f; p.pizz_bright = 0.5f;
		p.rel = 0.08f;
		break;
	default:
		break;
	}
	return p;
}

#ifndef BOWED_PRESET
#define BOWED_PRESET 0
#endif

// fixed model constants
static const real BETA_REF = 0.11f;   // bow position the force window was measured at
static const real FRIC_A = 0.35f;     // dynamic / static friction
static const real FRIC_C = 0.3f;      // friction curve width (x bow speed)
static const real FRIC_CD = 0.7f;     // smooth friction for short strings (x force)
static const real EPS_LO = 0.012f, EPS_HI = 0.005f;  // corner rounding / period, light / full bow
static const real ONSET_T = 0.08f;    // firm bow at a note start for this long (s)
static const real ONSET_KK = 0.0f;    // (v5's firm start: not needed with a seeded string)
static const real ONSET_EASE = 0.45f; // seeded start: force eased over the first two periods
static const real ONSET_TASTO = 2.5f; // ... fading out towards the fingerboard (x (bq - 1))

struct VoiceState {
	int active, quiet;
	int slip;                 // bow state: slipping (1) or sticking (0)
	int cnt;                  // samples since the last slip onset (pitch tracking)
	int pzn;                  // pluck pulse samples left
	int seeded;               // this note started from a seeded string
	real corr;                // adaptive tuning correction, fraction of the period
	real dcr;                 // DC reflection of the bridge
	uint32_t w, seed;         // delay write position, noise generator
	real note_prev, note_body;
	real pres_prev, vp_prev;  // raw controls of the previous block (pluck triggers)
	real P;                   // smoothed pressure
	real Ps;                  // slowly smoothed pressure (bow position, corner rounding)
	real acc;                 // accent envelope from positive pressure velocity
	real lift;                // quick release memory (negative pressure velocity)
	real by;                  // smoothed but_y
	real age;                 // seconds since the note started (vibrato delay)
	real vib_ph;              // vibrato phase 0..1
	real vb, ds, cw, nz, dn;  // per-sample ramped values at the end of the previous block
	real y, ydc, yn;          // loss filter states (bridge, its DC path, finger)
	real xprev;               // |D| of the previous sample
	real pza, pzc, pzs, pzcw, pzsw; // pluck pulse: amplitude, phasor, rotation
	real hp, ts;              // DC blocker and tone shelf states
	real shelf;               // tone shelf gain (ramped)
	real gout;                // output gain = dynamics (ramped; held after a release)
	real bd[NB][2];           // body resonator states
};

struct BodyCoef { real b0, a1, a2; };

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	real sr, inv_sr, c_period, two_pi_sr, pi_sr, hp_k, dc_k, ts_k, block_dt;
	real shake;               // smoothed accelerometer magnitude
	Params p;                 // live parameters (written by the app / presets)
	Params ps;                // smoothed copy used by the DSP
	// section
	uint32_t ew;
	real e_ph[2];             // delay modulation phases 0..1
	real e_d[2];              // tap delays at the end of the previous block (samples)
	real e_lvl;               // ramped section level
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
// sin(2 pi ph) for ph in [0, 1)
static inline real sin_turn(real ph) {
	real x = ph < 0.5f ? 1.0f - 4.0f * fabsf(ph - 0.25f) : 4.0f * fabsf(ph - 0.75f) - 1.0f;
	return sin_poly(1.5707963f * x);
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

// 3rd order Lagrange interpolator for the delay (base + 1 + fr), taps at base .. base + 3
static inline void lagrange(real fr, real* h) {
	const real fm1 = fr - 1.0f, fm2 = fr - 2.0f, fp1 = fr + 1.0f;
	const real cd = fm1 * fm2, ab = fp1 * fr;
	h[0] = (-1.0f / 6.0f) * fr * cd;
	h[1] = 0.5f * fp1 * cd;
	h[2] = -0.5f * ab * fm2;
	h[3] = (1.0f / 6.0f) * ab * fm1;
}

static inline bool bad(real x, real lim) { return !(fabsf(x) < lim); }  // also true for NaN
static inline real clampf(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }  // NaN -> lo

// body modes of a violin: frequency (Hz), Q, gain; the last one is the bridge hill
static const real body_f[NB] = {275.0f, 460.0f, 565.0f, 1150.0f, 2700.0f};
static const real body_q[NB] = {12.0f, 16.0f, 14.0f, 6.0f, 1.8f};
static const real body_g[NB] = {0.8f, 1.0f, 0.9f, 0.55f, 1.0f};

static inline void reset_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	for (int i = 0; i < NECK_LEN; i++) neck_buf[v][i] = 0.0f;
	for (int i = 0; i < BRIDGE_LEN; i++) bridge_buf[v][i] = 0.0f;
	int active = s.active;
	uint32_t seed = s.seed;
	real note_prev = s.note_prev, note_body = s.note_body, dn = s.dn, corr = s.corr, dcr = s.dcr, vib_ph = s.vib_ph;
	s = VoiceState();
	s.active = active;
	s.seed = seed;
	s.note_prev = note_prev;
	s.note_body = note_body;
	s.dn = dn;
	s.corr = corr;
	s.dcr = dcr;
	s.vib_ph = vib_ph;
}

// Select a preset; callable between blocks at any time (after init). Sounding voices continue with
// the new parameters (they glide in through the smoothed copy; every state stays bounded).
static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	e.preset = i;
	e.p = preset_params(i);
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.block_dt = (real)BLOCK / e.sr;
	e.c_period = e.sr / 8.1757989156f;
	e.two_pi_sr = 6.2831853f / e.sr;
	e.pi_sr = 3.14159265f / e.sr;
	e.hp_k = 6.2831853f * 25.0f / e.sr;  // DC blocker, 25 Hz
	e.dc_k = 6.2831853f * 4.0f / e.sr;   // loss filter DC path, 4 Hz
	e.ts_k = 6.2831853f * 1300.0f / e.sr;  // tone shelf corner
	e.shake = 1.0f;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	set_preset(e, BOWED_PRESET);
	e.ps = e.p;
	e.ew = 0;
	e.e_ph[0] = 0.0f; e.e_ph[1] = 0.43f;
	e.e_d[0] = e.e_d[1] = 600.0f;
	e.e_lvl = e.p.ens;
	for (int i = 0; i < ENS_LEN; i++) ens_buf[i] = 0.0f;
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.vs[v] = VoiceState();
		e.vs[v].seed = 0x9e3779b9u * (uint32_t)(v + 1);
		e.vs[v].note_prev = e.vs[v].note_body = 69.0f;
		e.vs[v].dn = 100.0f;
		e.vs[v].corr = 0.003f;
		e.vs[v].dcr = 0.999f;
		e.vs[v].vib_ph = 0.137f * (real)v - (real)(int)(0.137f * (real)v);
		reset_voice(e, v);
	}
}

// one voice over a block, added into out
static inline void process_voice(Engine& e, int v, real trem, real body_shift, real* __restrict__ out) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	const Params& p = e.ps;
	const bool pizz = p.pizz >= 0.5f;
	const real pres = clampf(c.pres, 0.0f, 2.0f);  // (also maps NaN to 0)
	// pressure velocity only counts while the key is held: after a release the firmware leaves the
	// last value in place (a stale positive value must not keep accents, noise or plucks alive)
	const real vp = pres > 0.0f ? clampf(c.vpres, -1.5f, 1.5f) : rmin(clampf(c.vpres, -1.5f, 1.5f), 0.0f);
	if (!s.active) {
		if (pres <= 0.0f) {
			s.note_prev = c.note;
			s.pres_prev = 0.0f;
			s.vp_prev = vp;
			return;
		}
		s.active = 1;
		s.quiet = 0;
	}
	const real note0 = clampf(c.note, 18.0f, 116.0f);
	const bool jump = fabsf(note0 - s.note_prev) >= 1.0f;
	s.note_prev = c.note;
	const bool onset = pres > 0.0f && s.pres_prev <= 0.0f;
	if (onset || jump) { s.age = 0.0f; s.seeded = onset && !pizz; }
	s.age = rmin(s.age + e.block_dt, 100.0f);

	// --- block rate control
	// a bowed note starts at once: at a key press the bow is set to the pressure a few ms ahead
	// (the press is still rising: pressure + its velocity's share of the next ~5 ms)
	const bool bow_onset = onset && !pizz;
	if (bow_onset) s.P = pres;
	s.P += 0.3f * (pres - s.P);
	// the bow position and the corner rounding follow the pressure slowly (~40 ms): they move the
	// delay taps, and a fast press would shake the string's motion right at the start
	if (bow_onset) s.Ps = rmin(pres + 0.4f * rmax(vp, 0.0f), 1.0f);
	s.Ps += 0.025f * (rmin(s.P, 1.0f) - s.Ps);
	if (pres <= 0.0f && s.P < 1e-4f) s.P = 0.0f;
	const real P = s.P;
	const real Pn = rmin(P, 1.0f);
	// accent envelope from the pressure velocity: a deadband (ordinary note-ons and sensor jitter,
	// also with a raised velocity sensitivity, don't count), then a soft saturating curve (0 .. 0.8).
	// It only listens at a note start (first 100 ms) or to a real strike while held, so jitter on a
	// sustained note never triggers it.
	{
		const real vin = (s.age < 0.1f || vp > 0.6f) ? rmax(vp - 0.12f, 0.0f) : 0.0f;
		s.acc = rmax(s.acc * exp2_fast(-1.442695f * e.block_dt / rmax(p.acc_time, 0.002f)), vin / (vin + 0.4f));
	}
	const real lift_now = rmax(-vp - 0.04f, 0.0f);
	s.lift = pres > 0.0f ? rmax(s.lift * 0.95f, lift_now) : rmax(s.lift, lift_now);
	s.by += 0.15f * (clampf(c.but_y, -1.0f, 1.0f) - s.by);
	if (jump) s.note_body = note0;
	s.note_body += 0.005f * (note0 - s.note_body);           // slow: vibrato doesn't move the body

	// automatic vibrato, fading in after vib_delay
	s.vib_ph += clampf(p.vib_hz, 0.0f, 12.0f) * e.block_dt;
	s.vib_ph -= (real)(int)s.vib_ph;
	const real vib_amt = clampf((s.age - p.vib_delay) * 3.0f, 0.0f, 1.0f) * clampf(p.vib, 0.0f, 60.0f) * 0.01f;
	const real note = note0 + vib_amt * sin_turn(s.vib_ph);

	const real p2 = strisy::fastpow2(note * (1.0f / 12.0f));
	const real f0 = 8.1757989156f * p2;
	const real L = rmin(e.c_period / p2, (real)(NECK_LEN - 8));
	const real w0 = e.two_pi_sr * f0;                         // <= 0.86 rad
	// bow position: tilt forward = towards the bridge (sul ponticello), back = sul tasto;
	// a light bow drifts towards the fingerboard (flautando), a heavy one towards the bridge
	const real eb = clampf(-p.pos_tilt * s.by + p.pos_press * (0.5f - s.Ps), -0.75f, 0.9f);
	// 0.055 .. 0.2, at least 2 samples from the bridge (the highest notes)
	// (and on the longest strings not too close to it: the motion gets irregular there)
	// (pizzicato: plucked over the end of the fingerboard, 0.15 .. 0.3, tilt forward = nearer the bridge)
	const real beta = pizz ? clampf(0.21f * exp2_fast(-0.5f * s.by), rmax(4.0f / L, 0.15f), 0.3f)
		: clampf(rmax(clampf(p.pos, 0.05f, 0.2f) * exp2_fast(eb), rmax(2.2f / L, 0.03f + 6e-5f * L)), 0.055f, 0.2f);
	// string losses: a heavier bow gives sharper corners (brighter)
	// corner rounding time (one pole time constant) as a fraction of the period, like the
	// finite bow width; the sampled loop needs no less than about a third of a sample
	// (a plucked string has no bow to re-sharpen its corners: much less rounding, so it rings)
	// (and the floor of a third of a sample that keeps the bowed loop clean is not needed either;
	// it would damp the high notes, where it is a large part of the period)
	const real tau = rmax((pizz ? 0.3f * EPS_HI : EPS_LO + (EPS_HI - EPS_LO) * Pn) / clampf(p.bright, 0.3f, 3.0f) * L, pizz ? 0.05f : 0.3f);
	const real pole = tau / (1.0f + tau);
	const real t60s = pizz ? clampf(p.pizz_decay * exp2_fast((60.0f - note) * (1.0f / 36.0f)), 0.05f, 8.0f)
	                       : clampf(p.damp * exp2_fast((40.0f - note) * (1.0f / 24.0f)), 0.3f, 8.0f);
	real t60 = t60s;
	if (pres <= 0.0f) {
		const real ped = clampf(e.g.pedal, 0.0f, 1.0f);
		t60 = rmin(t60s, clampf(p.rel + p.rel_ped * ped * ped, 0.02f, 20.0f) * (1.0f + clampf(p.rel_lift, 0.0f, 20.0f) * rmin(s.lift, 1.0f)));
	}
	real gl = exp2_fast(-9.9657843f / (f0 * t60));     // loop gain per period for t60
	const real sw0 = sin_poly(w0), cw0 = cos_poly(w0);
	// tuning: subtract the loss filters' phase delay at the fundamental
	// (the finger, nut side, rounds the corners like the bridge: a second one pole lowpass there)
	const real polen = pole;
	const real dfilt = 2.0f * atan_pos(pole * sw0, 1.0f - pole * cw0) / w0;
	// The bow itself shifts the pitch (flattening, more with rounder corners and a bow near the
	// bridge). The slip onsets mark the periods: an adaptive correction keeps the measured period
	// at L (time constant ~40 ms, a fraction of a cent of jitter), kept across notes. A plucked
	// string has no bow: no correction.
	if (jump) s.cnt = 0;
	const real Lstr = rmax(L - dfilt - (pizz ? 0.0f : s.corr) * L, 3.4f);
	real Db = rmin(rmax(beta * Lstr, 1.0f), rmin(Lstr - 2.2f, (real)(BRIDGE_LEN - 4)));
	if (pizz) Db = (real)(int)(Db + 0.5f);  // whole samples: no interpolation loss on a free string
	const real Dn = Lstr - Db;
	const int ib = (int)Db;
	const real fb = Db - (real)ib;
	// A free string (plucked, or ringing after the bow left) decays at t60 exactly: the loss of
	// the corner rounding filters and of the two fractional delays at the fundamental is made up.
	// (The loop's other resonances, the harmonics, lose more; the DC reflection stays at dcr.)
	// While bowing these losses are part of the tuned motion.
	if (pizz || pres <= 0.0f) {
		const real h2 = (1.0f - pole) * (1.0f - pole) / (1.0f - 2.0f * pole * cw0 + pole * pole);  // |H(w0)|^2
		const real hlin2 = 1.0f - 2.0f * fb * (1.0f - fb) * (1.0f - cw0);                           // linear interp.
		real hl[4];
		const real dl = Dn - (real)(int)Dn;
		lagrange(dl, hl);
		const real c2 = 2.0f * cw0 * cw0 - 1.0f, s2 = 2.0f * sw0 * cw0;
		const real c3 = c2 * cw0 - s2 * sw0, s3 = s2 * cw0 + c2 * sw0;
		const real lre = hl[0] + hl[1] * cw0 + hl[2] * c2 + hl[3] * c3, lim = hl[1] * sw0 + hl[2] * s2 + hl[3] * s3;
		const real loss = h2 * fsqrt(rmax(hlin2, 0.25f) * rmax(lre * lre + lim * lim, 0.25f));
		gl = rmin(gl / rmax(loss, 0.5f), 0.9995f);  // (every reflection stays below 1)
	}
	const real gp = gl * (1.0f - pole);

	// bow speed and force (homogeneous: the ratio sets the timbre, the speed the loudness)
	// (pressing beyond 1 adds force, not speed: scratchier, not louder)
	const real scur = clampf(p.speed_curve, 0.0f, 1.0f);
	// Beyond pressure 1 the bow gets faster (louder, fuller), saturating softly: x/(1+x) of the
	// excess. The force follows the speed (same force/speed ratio): the motion stays Helmholtz.
	const real over = rmax(P - 1.0f, 0.0f), osat = over / (1.0f + over);
	const real Pe = Pn + 0.35f * osat;
	// Loudness is an output gain, not the bow speed: the model is homogeneous (scaling bow speed,
	// force and friction width together just scales the motion), so the string always runs at a
	// nominal bow speed (1, or 0 once the bow has left) and the pressure's dynamics, as fast as
	// the key, never disturb the string's motion. Timbre still follows the pressure through the
	// force ratio, bow position, corner rounding and tone.
	const real amp_t = Pe * (1.0f - scur + scur * Pe);
	const real amp = pizz ? 0.0f : (pres > 0.0f ? 1.0f : 0.0f);
	const real acc = rmin(s.acc, 1.0f);
	const real vb = amp * trem;  // (constant while bowing: the accent is force and output gain)
	// bow force: kappa = force/speed*beta. The Helmholtz motion needs kappa between a minimum that
	// grows towards the bridge and a maximum ~1.1 (above it: raucous, scratchy). Light pressure
	// sits above the minimum (soft, flautando), full pressure below the maximum (bright,
	// focused), pressing beyond 1 or striking (accent) goes over it (scratch).
	// (towards the fingerboard the upper limit drops a little too)
	const real bq = fsqrt(BETA_REF / beta);
	const real khi = clampf(p.force_hi, 0.3f, 1.6f) * rmin(bq * fsqrt(fsqrt(bq)), 1.0f);  // (ref/beta)^1.375
	const real kfl = rmin(clampf(p.force_lo, 0.3f, 1.6f) * bq, khi - 0.1f);
	// grit (force_over): beyond pressure 1 the force rises gradually towards the raucous limit and
	// the bow noise grows with it, so scratch fades in instead of switching on
	// (force_over 1.0, the default: scratch ~ -26 dB under the tone at pressure 1.5, -23 dB at 2;
	// 2: about v3's level, ~ -19 dB at 1.5; 0: clean)
	const real grit = clampf(p.force_over, 0.0f, 2.0f) * 0.6f * osat;
	const real kap = rmax(kfl + (khi - kfl) * Pn * fsqrt(Pn) + rmin(0.4f * grit, rmax(1.25f - khi, 0.0f)), 0.1f);
	// the accent firms the bow up towards, never past, the top of the clean (Helmholtz) range
	// The start of a note needs a firm bow: while the string is still getting up to speed a light
	// bow slips several times per period (a breathy, scratchy start; Guettler). For the first
	// ~80 ms the force ratio sits near the top of the clean range, then relaxes to the pressure's.
	// (towards the fingerboard less: a firm start there locks into the octave)
	const real of = clampf(1.0f - s.age * (1.0f / ONSET_T), 0.0f, 1.0f) * clampf(ONSET_TASTO * (bq - 1.0f) + 1.0f, 0.0f, 1.0f);
	const real kon = kap + rmax(khi * ONSET_KK - kap, 0.0f) * of;
	real kacc = rmax(rmin(kap + 0.25f * clampf(p.acc_force, 0.0f, 3.0f) * acc, rmax(khi * 1.12f, kap)), kon);
	{	// seeded start: the first corner arrives before the bow has built its usual stick force
		// (the seed is the lossless motion): ease the force for the first ~2 periods so that
		// corner releases the slip on time
		const real ofe = s.seeded ? clampf(1.0f - s.age / (2.0f * L * e.inv_sr), 0.0f, 1.0f) : 0.0f;
		kacc *= 1.0f - ONSET_EASE * ofe;
	}
	const real accf = kacc / kap;
	const real ds = kacc / beta * amp;
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
	const real cw = FRIC_C * rmax(L * (1.0f / 250.0f), 1.0f) * amp + FRIC_CD * soft2 * ds + 1e-7f;
	// bow noise: jitter of the static friction. On long strings the stick phase approaches the
	// release threshold slowly, so the same jitter shakes the slip timing (the pitch) much more
	const real nz = rmin(clampf(p.rosin, 0.0f, 0.2f) * rmin(200.0f / L, 1.0f) + clampf(p.acc_noise, 0.0f, 1.0f) * acc
		+ 0.15f * grit, 0.9f);

	// pizzicato: pluck at a key press, a strike while held, or a new note while held
	if (pizz && pres > 0.0f && (onset || jump || (vp > 0.6f && s.vp_prev <= 0.3f))) {
		// (narrower than the bridge segment: the pulse and its inverted reflection would cancel)
		const real width = clampf(0.06f / clampf(p.pizz_bright, 0.2f, 5.0f) * L, rmin(6.0f, 0.8f * beta * L), rmin(300.0f, 0.8f * beta * L));
		s.pzn = (int)width;
		const real dw = 3.14159265f / (real)s.pzn;
		s.pzcw = cos_poly(dw); s.pzsw = sin_poly(dw);
		s.pzc = 1.0f; s.pzs = 0.0f;
		// plucked velocity pulse: louder with a faster key press; the area grows as width^0.75, so
		// the register balance stays even (low notes have wide, high notes narrow pulses)
		s.pza = (0.35f + 0.65f * rmin(rmax(vp, 0.0f) + 0.4f * pres, 1.0f)) * 22.0f * fsqrt(fsqrt(dw)) / beta * BETA_REF;
	}
	s.pres_prev = pres;
	s.vp_prev = vp;

	// tone shelf above the corner rounding: a light bow (flautando) and the fingerboard are
	// dark, a heavy bow, the bridge (ponticello) and a strike are bright
	const real tlo = clampf(p.tone_lo, 0.0f, 3.0f), thi = clampf(p.tone_hi, 0.0f, 3.0f);
	const real shelf = rmin((tlo + (thi - tlo) * (pizz ? 0.6f : Pn * fsqrt(Pn))) * fsqrt(bq) * (1.0f + 0.5f * osat)
		* (1.0f + clampf(p.acc_bright, 0.0f, 2.0f) * acc), 4.0f);

	// body coefficients (band-passes, transposed direct form II, peak gain = mode gain)
	BodyCoef bc[NB];
	{
		const real track = clampf(p.body_track, 0.0f, 1.0f);
		const real sz = clampf(p.body_size, 0.2f, 1.5f) * rmin(exp2_fast((s.note_body - 55.0f) * 0.055f * track), 1.0f) * body_shift;
		const real bamt = clampf(p.body_amt, 0.0f, 2.0f), qs = clampf(p.body_q, 0.3f, 3.0f);
		for (int k = 0; k < NB; k++) {
			// the bridge hill scales less with the size
			const real f = body_f[k] * (k == NB - 1 ? fsqrt(sz) : sz);
			const real K = strisy::tan0(rmin(e.pi_sr * f, 1.2f));
			const real KQ = K / (body_q[k] * (k == NB - 1 ? 1.0f : qs));
			const real norm = 1.0f / (1.0f + KQ + K * K);
			const real gk = (k == NB - 1 ? clampf(p.hill, 0.0f, 3.0f) : body_g[k] * bamt);
			bc[k] = {gk * KQ * norm, 2.0f * (K * K - 1.0f) * norm, (1.0f - KQ + K * K) * norm};
		}
	}

	// --- string and bow, per sample
	const real r = 1.0f / BLOCK;
	if (jump || bow_onset) s.dn = Dn;  // new note: no glide through the jump
	// Seed: a bowed note starts already in Helmholtz motion (no transient chaos from rest, which
	// is where a scratchy start comes from). With the slip just ended (the stick phase starts
	// now), the waves written into the two delay lines are sawtooths (Smith, McIntyre/Woodhouse):
	//   neck   a(t) - a(t - Dn) = v(t),  bridge  b(t) - b(t - Db) = v(t)
	// for the bow-point velocity v = vb in the stick phase and -vb (1-beta)/beta in the slip
	// phase: slopes -vb/(beta L) and +vb/(beta L), jumps +vb/beta and -vb/beta, zero mean.
	// The bow state starts sticking at the bow speed; the bow then sustains the motion.
	if (bow_onset && vb > 1e-6f) {
		// (the delays include the two corner filters' delay, half each: the loop's period)
		const real Dbe = Db + 0.5f * dfilt, Ls = Dn + Dbe + 0.5f * dfilt, bt = Dbe / Ls;
		const real h = 0.5f * vb / bt, sl = vb / (bt * Ls);
		const uint32_t w0 = s.w;
		float* nb = neck_buf[v];
		float* bb = bridge_buf[v];
		const int na = (int)Dn + 4, nbr = (int)Db + 3, Li = (int)Ls;
		(void)Li;
		const real ph = 0.0f;  // the cycle's phase at t0 (samples after the slip's end)
		for (int k = 1; k <= na; k++) {
			real u = (real)k - ph;                                       // a(t0 - k): a jumps at t0 - ph
			u -= Ls * (real)(int)(u / Ls); if (u <= 0.0f) u += Ls;
			nb[(w0 - (uint32_t)k) & NECK_MASK] = -h + sl * u;
		}
		for (int k = 1; k <= nbr; k++) {
			real u = (real)k - ph - Dbe;                                 // b jumps at t0 - ph - Dbe
			u -= Ls * (real)(int)(u / Ls); if (u <= 0.0f) u += Ls;
			bb[(w0 - (uint32_t)k) & BRIDGE_MASK] = -h + sl * (Ls - u);
		}
		s.y = gl * (-h + sl * (Db - (real)(ib + 1) + 1.0f));
		s.yn = -h + sl * (real)((int)Dn + 1);
		s.ydc = 0.0f;
		s.slip = 0; s.xprev = 0.0f; s.cnt = 0;
		s.vb = vb; s.ds = ds; s.cw = cw; s.nz = nz;
		s.hp = 0.0f;
	}
	const real dvb = (vb - s.vb) * r, dds = (ds - s.ds) * r, dcw = (cw - s.cw) * r, dnz = (nz - s.nz) * r;
	const real ddn = (Dn - s.dn) * r;
	// Lagrange coefficients: with the delay moving less than ~0.8 samples in the block (always
	// except fast glides) the taps stay put for the block and the coefficients are ramped
	// linearly between their values at the first and last sample (d stays within 0.5 .. 2.5)
	const bool lramp = fabsf(Dn - s.dn) < 0.8f;
	const int ilr = (int)(0.5f * (s.dn + Dn));
	real lh[4], ldh[4];
	{
		real ha[4], hb[4];
		lagrange(s.dn + ddn - (real)ilr, ha);
		lagrange(Dn - (real)ilr, hb);
		const real rr = 1.0f / (BLOCK - 1);
		for (int k = 0; k < 4; k++) { ldh[k] = (hb[k] - ha[k]) * rr; lh[k] = ha[k] - ldh[k]; }
	}
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
	// DC reflection -0.999: (almost) rigid. Released, the leftover DC drains faster (for the
	// gating); slewed over ~60 ms so bow bouncing doesn't click
	s.dcr += 0.016f * ((pres > 0.0f ? 0.999f : 0.5f) - s.dcr);
	const real dcg = gl - s.dcr, dck = e.dc_k;
	real y = s.y, vbn = s.vb, dsn = s.ds, cwn = s.cw, nzn = s.nz, dn = s.dn;
	const real fa = FRIC_A, fa1 = 1.0f - FRIC_A;
	real peak = 0.0f, xprev = s.xprev;
	// dynamics: the output gain follows the pressure (bowed) and holds after a release
	// (a strike: a short, louder, brighter start, a martele bite; the bow force stays: extra force
	// at the start breaks the seeded motion after a few periods, the scratch of v5)
	const real gtarget = pizz ? 1.0f : (pres > 0.0f ? amp_t * (1.0f + 0.3f * acc) : s.gout);
	if (bow_onset) s.gout = 0.0f;
	real gn = s.gout;
	const real dgn = (gtarget - gn) * r;
	int pzn = s.pzn;
	real pzc = s.pzc, pzs = s.pzs;
	const real pza = s.pza, pzcw = s.pzcw, pzsw = s.pzsw;
	for (int n = 0; n < BLOCK; n++) {
		vbn += dvb; dsn += dds; cwn += dcw; nzn += dnz; dn += ddn;
		// neck side: 3rd order Lagrange at delay dn (taps at i-1 .. i+2, d = fr + 1)
		real nut_in;
		if (lramp) {
			lh[0] += ldh[0]; lh[1] += ldh[1]; lh[2] += ldh[2]; lh[3] += ldh[3];
			const uint32_t r0 = w - (uint32_t)(ilr - 1);
			nut_in = lh[0] * nbuf[r0 & NECK_MASK] + lh[1] * nbuf[(r0 - 1) & NECK_MASK]
				+ lh[2] * nbuf[(r0 - 2) & NECK_MASK] + lh[3] * nbuf[(r0 - 3) & NECK_MASK];
		} else {
			const int i = (int)dn;
			real h[4];
			lagrange(dn - (real)i, h);
			const uint32_t r0 = w - (uint32_t)(i - 1);
			nut_in = h[0] * nbuf[r0 & NECK_MASK] + h[1] * nbuf[(r0 - 1) & NECK_MASK]
				+ h[2] * nbuf[(r0 - 2) & NECK_MASK] + h[3] * nbuf[(r0 - 3) & NECK_MASK];
		}
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
		real alpha = 1.0f;
		if (slip_new & !slip) {  // slip onset: one per period in Helmholtz motion
			if (cnt > cmin) {
				if (cnt >= clo && cnt <= chi) { tsum += cnt; tn++; }
				cnt = 0;
			}
			// the slip starts somewhere between two samples: at the onset sample the force is
			// blended between stick and slip by the fraction of the interval spent beyond the
			// threshold (first order anti-aliasing of the jump: onsets don't snap to the grid)
			alpha = rmin((x - dse) / rmax(x - xprev, 1e-12f), 1.0f);
		}
		const real fslip = delta > 0.0f ? x - u : u - x;
		// (and at the capture, where the slip branch ends, half of the remaining slip on average)
		const real ucap = (slip & !slip_new) ? 0.25f * rmax(B, 0.0f) : 0.0f;
		slip = slip_new;
		xprev = x;
		real f = slip ? delta + alpha * (fslip - delta) : (delta > 0.0f ? delta - ucap : delta + ucap);
		if (pizz) f = 0.0f;  // no bow on the string
		if (pzn > 0) {  // pluck pulse
			pzn--;
			const real ns = pzs * pzcw + pzc * pzsw;
			pzc = pzc * pzcw - pzs * pzsw;
			pzs = ns;
			f += pza * pzs;
		}
		BOWED_TRACE(v, vbn, bref + nref + f, slip ? 0.0f : 1.0f);
		nbuf[w & NECK_MASK] = bref + f;
		bbuf[w & BRIDGE_MASK] = nref + f;
		w++;
		gn += dgn;
		sb[n] = br_in * gn;
		peak = rmax(peak, fabsf(br_in - ydc));  // (the DC is not heard)
	}
	s.w = w; s.seed = seed; s.y = y; s.slip = slip; s.cnt = cnt < 1000000 ? cnt : 1000000; s.ydc = ydc; s.yn = yn; s.xprev = xprev;
	s.pzn = pzn; s.pzc = pzc; s.pzs = pzs;
	s.gout = gn;
	if (!pizz && tn > 0 && kap * accf < 1.3f) {  // not in raucous motion
		const real kad = rmin(0.25f, L * e.inv_sr * 25.0f);  // per period: T / 40 ms
		s.corr = rmin(rmax(s.corr + kad * ((real)tsum - (real)tn * L) / L, -0.02f), 0.04f);
	}
	s.vb = vbn; s.ds = dsn; s.cw = cwn; s.nz = nzn; s.dn = dn;

	// --- body, per sample. The output is scaled with beta so the bow position is a timbre
	// control rather than a loudness one (the string velocity grows as 1/beta)
	{
		const real gain = 5.0f * clampf(p.gain, 0.0f, 4.0f) * beta;
		const real dir = clampf(p.direct, 0.0f, 2.0f);
		const real hk = e.hp_k;
		real hp = s.hp, ts = s.ts, sh = s.shelf;
		const real dsh = (shelf - sh) * r, tk = e.ts_k;
		real q01 = s.bd[0][0], q02 = s.bd[0][1], q11 = s.bd[1][0], q12 = s.bd[1][1];
		real q21 = s.bd[2][0], q22 = s.bd[2][1], q31 = s.bd[3][0], q32 = s.bd[3][1];
		real q41 = s.bd[4][0], q42 = s.bd[4][1];
		real opeak = 0.0f;
		for (int n = 0; n < BLOCK; n++) {
			const real xdc = sb[n] - hp;
			hp += hk * xdc;
			// tone: high shelf above ~1.3 kHz
			ts += tk * (xdc - ts);
			sh += dsh;
			const real xin = ts + sh * (xdc - ts);
			// band-passes: y = b0 x + s1, s1 = s2 - a1 y, s2 = -b0 x - a2 y
			real acc_o = dir * xin, bx, yk;
			bx = bc[0].b0 * xin; yk = bx + q01; q01 = q02 - bc[0].a1 * yk; q02 = -bx - bc[0].a2 * yk; acc_o += yk;
			bx = bc[1].b0 * xin; yk = bx + q11; q11 = q12 - bc[1].a1 * yk; q12 = -bx - bc[1].a2 * yk; acc_o += yk;
			bx = bc[2].b0 * xin; yk = bx + q21; q21 = q22 - bc[2].a1 * yk; q22 = -bx - bc[2].a2 * yk; acc_o += yk;
			bx = bc[3].b0 * xin; yk = bx + q31; q31 = q32 - bc[3].a1 * yk; q32 = -bx - bc[3].a2 * yk; acc_o += yk;
			bx = bc[4].b0 * xin; yk = bx + q41; q41 = q42 - bc[4].a1 * yk; q42 = -bx - bc[4].a2 * yk; acc_o += yk;
			const real o = gain * acc_o;
			sb[n] = o;
			opeak = rmax(opeak, fabsf(o));
		}
		s.hp = hp; s.ts = ts; s.shelf = sh;
		s.bd[0][0] = q01; s.bd[0][1] = q02; s.bd[1][0] = q11; s.bd[1][1] = q12;
		s.bd[2][0] = q21; s.bd[2][1] = q22; s.bd[3][0] = q31; s.bd[3][1] = q32;
		s.bd[4][0] = q41; s.bd[4][1] = q42;
		peak = rmax(peak, opeak);
	}

	// --- safety: non-finite or runaway state resets the voice (cannot happen in normal operation)
	if (bad(y + s.ydc + s.yn, 1e3f) || bad(s.hp + s.ts + s.bd[0][0] + s.bd[1][0] + s.bd[2][0] + s.bd[3][0] + s.bd[4][0], 1e4f)
		|| bad(peak, 1e3f) || bad(s.pzc + s.pzs, 4.0f)) {
		reset_voice(e, v);
		return;
	}
	for (int n = 0; n < BLOCK; n++) out[n] += sb[n];
	// --- gating: idle once released and the whole loop has decayed
	if (pres <= 0.0f && peak < 1e-5f && pzn == 0) {  // -100 dB re full level
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

#ifdef BOWED_TEST_HOOK
static void bowed_test_hook(Engine& e);
#endif

// section: two more players read from a delay of the mix, each with its own slow delay wander
// (pitch wander = the delay's slope) and timing offset
static inline void process_section(Engine& e, real* __restrict__ output) {
	const Params& p = e.ps;
	const real target = clampf(p.ens, 0.0f, 1.5f);
	const real lvl0 = e.e_lvl;
	e.e_lvl += 0.2f * (target - e.e_lvl);
	uint32_t w = e.ew;
	if (lvl0 < 1e-4f && target <= 0.0f) {  // solo: keep the delay line filled, nothing else
		for (int n = 0; n < BLOCK; n++) ens_buf[(w + n) & ENS_MASK] = output[n];
		e.ew = w + BLOCK;
		return;
	}
	// delay wander: d(t) = base + A sin(2 pi f t), pitch deviation ~ 1731 * 2 pi f A / sr cents
	static const real hz[2] = {0.37f, 0.53f};
	static const real base_k[2] = {0.6f, 1.15f};
	real dd[2], d0[2];
	for (int k = 0; k < 2; k++) {
		e.e_ph[k] += hz[k] * e.block_dt;
		e.e_ph[k] -= (real)(int)e.e_ph[k];
		const real A = clampf(p.ens_detune, 0.0f, 25.0f) * e.sr / (1731.0f * 6.2831853f * hz[k]);
		const real base = clampf(p.ens_spread, 2.0f, 25.0f) * 0.001f * e.sr * base_k[k];
		const real d = clampf(base + rmin(A, base - 4.0f) * sin_turn(e.e_ph[k]), 4.0f, (real)(ENS_LEN - 4));
		d0[k] = e.e_d[k];
		dd[k] = (d - d0[k]) * (1.0f / BLOCK);
		e.e_d[k] = d;
	}
	const real dl = (e.e_lvl - lvl0) * (1.0f / BLOCK);
	real lvl = lvl0;
	for (int n = 0; n < BLOCK; n++) {
		const real x = output[n];
		ens_buf[w & ENS_MASK] = x;
		real sum = 0.0f;
		for (int k = 0; k < 2; k++) {
			d0[k] += dd[k];
			const int id = (int)d0[k];
			const real fr = d0[k] - (real)id;
			const real a = ens_buf[(w - (uint32_t)id) & ENS_MASK], b = ens_buf[(w - (uint32_t)id - 1u) & ENS_MASK];
			sum += a + fr * (b - a);
		}
		lvl += dl;
		// players add up partly incoherently: x (1 + 0.6 lvl) keeps the level about constant
		output[n] = (x + 0.7f * lvl * sum) / (1.0f + 0.6f * lvl);
		w++;
	}
	e.ew = w;
}

static inline void process(Engine& e, real* __restrict__ output) {
#ifdef BOWED_TEST_SWITCH
	{ static int nb = 0; if (++nb % BOWED_TEST_SWITCH == 0) set_preset(e, (e.preset + 1) % NUM_PRESETS); }
#endif
#ifdef BOWED_TEST_HOOK
	bowed_test_hook(e);
#endif
	// smooth the parameters towards the set values (live edits glide in ~20 ms); pizz switches
	{
		real* ps = &e.ps.gain;
		const real* pp = &e.p.gain;
		// the values that reshape the string's motion (force window, bow position, corner
		// rounding) move slower (~150 ms), so the motion and its pitch correction follow smoothly
		// (and the body size: a fast moving resonance bends the pitch of what passes through it)
		const real* slow[5] = {&e.p.force_lo, &e.p.force_hi, &e.p.pos, &e.p.bright, &e.p.body_size};
		for (int i = 0; i < NPARAMS; i++) {
			real x = pp[i];
			if (!(x == x)) x = ps[i];  // ignore NaN writes
			bool sl = false;
			for (int j = 0; j < 5; j++) sl |= pp + i == slow[j];
			ps[i] += (sl ? 0.006f : 0.03f) * (x - ps[i]);
		}
		e.ps.pizz = e.p.pizz >= 0.5f ? 1.0f : 0.0f;
	}
	for (int n = 0; n < BLOCK; n++) output[n] = 0.0f;
	// shaking the instrument moves the bow: speed modulation, strong shakes reverse it (tremolo)
	const real acc_abs = clampf(e.g.acc_abs, 0.0f, 4.0f);
	e.shake += 0.3f * (acc_abs - e.shake);
	if (bad(e.shake, 10.0f)) e.shake = 1.0f;
	const real trem = 1.0f - clampf(e.ps.trem, 0.0f, 2.0f) * rmin(rmax(e.shake - 1.08f, 0.0f), 1.4f);
	// rotation shifts the body resonances
	const real rot = clampf(e.g.rot_x + e.g.rot_y + e.g.rot_z, -1.0f, 1.0f);
	const real body_shift = exp2_fast(clampf(e.ps.body_rot, -2.0f, 2.0f) * rot);
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v, trem, body_shift, output);
	process_section(e, output);
	// safety soft limiter: linear up to 4, then smoothly towards 8 (C1 at the knee)
	for (int n = 0; n < BLOCK; n++) {
		real x = output[n];
		if (!(x == x)) x = 0.0f;
		const real a = rmax(fabsf(x) - 4.0f, 0.0f) * 0.25f;
		output[n] = x * (1.0f + 2.0f * a) / ((1.0f + a) * (1.0f + a));
	}
	if (bad(e.e_d[0] + e.e_d[1] + e.e_lvl, 1e5f)) {  // (cannot happen) reset the section
		e.e_d[0] = e.e_d[1] = 600.0f; e.e_lvl = 0.0f;
		for (int i = 0; i < ENS_LEN; i++) ens_buf[i] = 0.0f;
	}
}

// ---------------------------------------------------------------------------------------------
// live parameter table

struct ParamEntry { strisy::ParamDesc d; real Params::*m; };
static const ParamEntry param_table[] = {
	{{"gain", "level", 0.0f, 4.0f, 0}, &Params::gain},
	{{"force_lo", "bow", 0.3f, 1.6f, 0}, &Params::force_lo},       // force/speed at light pressure
	{{"force_hi", "bow", 0.3f, 1.6f, 0}, &Params::force_hi},       // at full pressure (> 1.1: raucous)
	{{"force_over", "bow", 0.0f, 2.0f, 0}, &Params::force_over},   // grit beyond pressure 1 (0: none)
	{{"speed_curve", "bow", 0.0f, 1.0f, 0}, &Params::speed_curve}, // 0 linear .. 1 quadratic dynamics
	{{"rosin", "bow", 0.0f, 0.2f, 0}, &Params::rosin},             // bow noise texture
	{{"pos", "position", 0.05f, 0.2f, 0}, &Params::pos},           // bow position (fraction of string)
	{{"pos_tilt", "position", 0.0f, 1.5f, 0}, &Params::pos_tilt},  // tilt range (octaves)
	{{"pos_press", "position", 0.0f, 1.0f, 0}, &Params::pos_press},// light bow drifts to the fingerboard
	{{"damp", "string", 0.5f, 10.0f, 0}, &Params::damp},           // ring time (s) at note 40
	{{"bright", "string", 0.3f, 3.0f, 0}, &Params::bright},        // corner sharpness
	{{"tone_lo", "string", 0.0f, 3.0f, 0}, &Params::tone_lo},      // treble at light pressure
	{{"tone_hi", "string", 0.0f, 3.0f, 0}, &Params::tone_hi},      // treble at full pressure
	{{"body_size", "body", 0.2f, 1.5f, 0}, &Params::body_size},    // violin 1, viola .8, cello .45, bass .3
	{{"body_track", "body", 0.0f, 1.0f, 0}, &Params::body_track},  // body follows the register
	{{"body_amt", "body", 0.0f, 2.0f, 0}, &Params::body_amt},
	{{"body_q", "body", 0.3f, 3.0f, 0}, &Params::body_q},
	{{"hill", "body", 0.0f, 3.0f, 0}, &Params::hill},              // bridge hill brilliance
	{{"direct", "body", 0.0f, 2.0f, 0}, &Params::direct},
	{{"acc_force", "accent", 0.0f, 3.0f, 0}, &Params::acc_force},  // strike: bite / scratch
	{{"acc_time", "accent", 0.002f, 0.2f, 0}, &Params::acc_time},
	{{"acc_noise", "accent", 0.0f, 1.0f, 0}, &Params::acc_noise},
	{{"acc_bright", "accent", 0.0f, 2.0f, 0}, &Params::acc_bright},
	{{"rel", "release", 0.02f, 3.0f, 0}, &Params::rel},            // ring after a slow release (s)
	{{"rel_ped", "release", 0.0f, 10.0f, 0}, &Params::rel_ped},    // + pedal
	{{"rel_lift", "release", 0.0f, 20.0f, 0}, &Params::rel_lift},  // x after a quick lift-off
	{{"vibrato", "mod", 0.0f, 60.0f, 0}, &Params::vib},            // automatic vibrato (cents)
	{{"vib_hz", "mod", 2.0f, 9.0f, 0}, &Params::vib_hz},
	{{"vib_delay", "mod", 0.0f, 2.0f, 0}, &Params::vib_delay},
	{{"tremolo", "mod", 0.0f, 2.0f, 0}, &Params::trem},            // shake -> bow
	{{"body_rot", "mod", -2.0f, 2.0f, 0}, &Params::body_rot},      // rotation -> body (octaves)
	{{"ensemble", "section", 0.0f, 1.5f, 0}, &Params::ens},        // two more players
	{{"ens_detune", "section", 0.0f, 25.0f, 0}, &Params::ens_detune},
	{{"ens_spread", "section", 2.0f, 25.0f, 0}, &Params::ens_spread}, // timing spread (ms)
	{{"pizz", "pizz", 0.0f, 1.0f, 2}, &Params::pizz},              // plucked instead of bowed
	{{"pizz_decay", "pizz", 0.1f, 5.0f, 0}, &Params::pizz_decay},  // ring (s) at note 60
	{{"pizz_bright", "pizz", 0.2f, 5.0f, 0}, &Params::pizz_bright},
};
static const int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

static inline void update_derived(Engine&) {}

} // namespace bowed

namespace strisy {
template <> struct EngineParams<bowed::Engine> {
	static const int count = bowed::NUM_LIVE_PARAMS;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &bowed::param_table[i].d : 0; }
	static float* ptr(bowed::Engine& e, int i) { return (i >= 0 && i < count) ? &(e.p.*(bowed::param_table[i].m)) : 0; }
	static void changed(bowed::Engine& e, int) { bowed::update_derived(e); }
};
} // namespace strisy

#endif // VOICE_BOWED_H_
