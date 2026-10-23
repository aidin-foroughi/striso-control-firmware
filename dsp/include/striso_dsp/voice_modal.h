/**
 * Modal voice: tuned percussion (vibraphone, marimba, glockenspiel, church bell, kalimba, wine glass).
 * Build with `make VOICE=modal`; presets with set_preset() (host renders: -DMODAL_PRESET=n).
 *
 * Every voice is a bank of up to NMODES damped resonators, one per partial of the instrument:
 *   z[n+1] = r*e^(jw) * z[n] (+ b*noise[n] when rubbed),  output Im(z)
 * in coupled form re' = c*re - s*im, im' = s*re + c*im (c = r cos w, s = r sin w). The rotation w is
 * set by the note alone and the decay only changes r, so partial frequencies never move except with
 * the note itself (vibrato / bend). The pole radius is exactly sqrt(c^2 + s^2): stable for any r < 1,
 * and the frequency is accurate in float down to the lowest notes.
 *
 * Partial ratios, levels and decay times follow published measurements of the real instruments
 * (Rossing, "Science of Percussion Instruments"; Fletcher & Rossing; church bell partials after
 * Hibberts / Perrin): each preset lists per partial its frequency ratio, strike level, how much
 * that level grows with mallet hardness, and its T60 at middle C. Higher notes decay faster
 * (T60 scales with f^-alpha).
 *
 * Playing:
 * - strike (fast pressure rise, vpres): loudness; a harder strike is also a little brighter (upper
 *   partials grow modestly with mallet hardness). Tilt at the moment of the strike makes the mallet
 *   a bit softer (back) or harder (forward). Nothing changes pitch.
 * - holding: the note rings with its natural decay. Lifting the key engages the damper gradually
 *   (fully at release); the pedal is the sustain pedal (lifts the damper).
 * - vibraphone: pressing harder than a normal hold, or rotating the instrument (gyro), adds the
 *   motor tremolo. Wine glass: held pressure rubs the rim (sustained singing tone, level follows
 *   pressure); the other presets ignore pressure after the strike.
 *
 * Safety: per-voice NaN/energy guard and reset, decay changes smoothed (no envelope kinks), master
 * one-block lookahead limiter at 4 and a final clamp at 7.5. No libm (polynomial exp2/sin/cos).
 */
#ifndef VOICE_MODAL_H_
#define VOICE_MODAL_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace modal {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 10;
static const int NMODES = 12;
static const int NSLOT = NMODES + 4;   // + 4 dummy (always zero) slots to pad the mode list

using strisy::rmax;
using strisy::rmin;

// ---------------------------------------------------------------------------------------------
// Presets

struct Partial {
	float ratio;   // frequency / note frequency
	float amp;     // strike level at full hardness
	float hexp;    // level scales with hardness^hexp (0: independent of the mallet)
	float t60;     // decay time (s) at note 60
	float bow;     // rubbed level (wine glass only)
};
struct Preset {
	const char* name;
	int n;              // number of partials
	float alpha;        // T60 ~ (f/f_C4)^-alpha over the keyboard
	float gain;         // level matching between presets
	float damper;       // damper strength on release (1 = vibraphone damper bar)
	float tremolo;      // 1: motor tremolo from pressure and gyro (vibraphone)
	float rub;          // > 0: held pressure rubs (wine glass), rubbed level per unit pressure
	Partial p[NMODES];
};

static const int NUM_PRESETS = 6;

static const Preset presets[NUM_PRESETS] = {
	{	// aluminium bar tuned 1 : 4 : 10, resonator tube; weak torsional and higher bending modes
		"vibraphone", 7, 0.8f, 0.87f, 1.0f, 1.0f, 0.0f, {
		{1.000f, 1.00f, 0.0f, 9.0f, 0}, {4.000f, 0.32f, 1.2f, 2.2f, 0}, {10.00f, 0.11f, 2.0f, 0.7f, 0},
		{2.760f, 0.025f, 1.5f, 0.5f, 0}, {6.250f, 0.025f, 2.0f, 0.30f, 0}, {13.50f, 0.02f, 2.5f, 0.15f, 0},
		{17.80f, 0.03f, 2.5f, 0.15f, 0}}},
	{	// rosewood bar tuned 1 : 3.96 : 9.86 (Rossing, E3 bar), resonator; short woody torsional modes
		"marimba", 8, 0.9f, 1.83f, 0.6f, 0.0f, 0.0f, {
		{1.000f, 1.00f, 0.0f, 1.6f, 0}, {3.960f, 0.38f, 1.3f, 0.35f, 0}, {9.860f, 0.13f, 2.0f, 0.10f, 0},
		{2.800f, 0.06f, 1.5f, 0.06f, 0}, {5.600f, 0.05f, 1.8f, 0.04f, 0}, {7.300f, 0.04f, 2.0f, 0.03f, 0},
		{12.30f, 0.03f, 2.5f, 0.025f, 0}, {15.90f, 0.04f, 2.5f, 0.03f, 0}}},
	{	// steel bar, free-free bending modes 1 : 2.71 : 5.15 : 8.43 (Rossing, glockenspiel)
		"glockenspiel", 7, 0.7f, 0.96f, 0.7f, 0.0f, 0.0f, {
		{1.000f, 1.00f, 0.0f, 6.0f, 0}, {2.710f, 0.42f, 1.2f, 1.2f, 0}, {5.150f, 0.20f, 1.8f, 0.5f, 0},
		{8.430f, 0.09f, 2.2f, 0.25f, 0}, {12.60f, 0.05f, 2.5f, 0.12f, 0}, {3.600f, 0.04f, 1.5f, 0.20f, 0},
		{7.100f, 0.03f, 2.0f, 0.10f, 0}}},
	{	// tuned church bell, prime = note: hum 0.5, tierce 1.183 (minor third), quint 1.506,
		// nominal 2.0, deciem 2.514, undeciem 2.662, duodeciem 3.011, upper octave 4.166 ...
		"bell", 11, 0.6f, 0.67f, 0.3f, 0.0f, 0.0f, {
		{1.000f, 0.55f, 0.3f, 9.0f, 0}, {0.500f, 0.45f, 0.0f, 14.0f, 0}, {1.183f, 0.60f, 0.4f, 8.0f, 0},
		{1.506f, 0.25f, 0.6f, 5.0f, 0}, {2.000f, 1.00f, 0.8f, 6.0f, 0}, {2.514f, 0.35f, 1.2f, 3.5f, 0},
		{2.662f, 0.25f, 1.3f, 3.0f, 0}, {3.011f, 0.30f, 1.5f, 2.5f, 0}, {4.166f, 0.20f, 1.8f, 1.6f, 0},
		{5.433f, 0.12f, 2.2f, 1.0f, 0}, {6.796f, 0.08f, 2.5f, 0.7f, 0}}},
	{	// steel tine clamped at one end: 1 : 6.27 : 17.55, plus short box/knock modes
		"kalimba", 5, 0.7f, 1.25f, 0.5f, 0.0f, 0.0f, {
		{1.000f, 1.00f, 0.0f, 3.5f, 0}, {6.270f, 0.20f, 1.5f, 0.25f, 0}, {17.55f, 0.06f, 2.5f, 0.06f, 0},
		{3.100f, 0.04f, 1.5f, 0.05f, 0}, {10.30f, 0.03f, 2.0f, 0.04f, 0}}},
	{	// wine glass, (m,0) modes 1 : 2.32 : 4.25 : 6.63 : 9.4; rubbing the rim drives the (2,0) mode
		"wine glass", 5, 0.7f, 0.90f, 0.15f, 0.0f, 0.32f, {
		{1.000f, 1.00f, 0.0f, 12.0f, 1.0f}, {2.320f, 0.35f, 1.0f, 5.0f, 0.12f}, {4.250f, 0.15f, 1.8f, 2.5f, 0.03f},
		{6.630f, 0.07f, 2.2f, 1.2f, 0}, {9.400f, 0.04f, 2.5f, 0.7f, 0}}},
};

static inline const char* preset_name(int i) {
	return (i >= 0 && i < NUM_PRESETS) ? presets[i].name : presets[0].name;
}

#ifndef MODAL_PRESET
#define MODAL_PRESET 0
#endif

// ---------------------------------------------------------------------------------------------
// Tuning constants

static const real F_LO0 = 16.0f, F_LO1 = 24.0f;        // partials fade in above 16..24 Hz
static const real F_HI0 = 14000.0f, F_HI1 = 20000.0f;  // and out between 14 and 20 kHz
static const real STRIKE_REARM = 0.02f; // strike strength below this ends a strike
static const real STRIKE_LEVEL = 1.6f;  // amplitude of a full strike (times preset and partial levels)
static const real DAMP_SIGMA = 46.0f;   // damper decay rate (1/s, T60 0.15 s), times the damper parameter
static const real SIGMA_MIN = 0.1f;     // slowest natural decay (T60 69 s): poles stay inside the unit circle
static const real BOW_GROW = 40.0f;     // max negative damping of the rub (1/s)
static const real BOW_DAMP = 6.0f;      // max extra damping by the rub above target, per unit pressure
static const real ENERGY_CAP = 9.0f;    // soft cap on a voice's mode energy (sum |z|^2)
static const real ENERGY_BAD = 1e4f;    // above this (or NaN) the voice is reset
static const real GATE_ENERGY = 1e-10f; // released voice below this goes idle
#ifndef MODAL_LIMIT
#define MODAL_LIMIT 4.0f
#endif
#ifndef MODAL_CLIP
#define MODAL_CLIP 7.5f
#endif
static const real LIMIT = MODAL_LIMIT;  // master limiter threshold
static const real CLIP = MODAL_CLIP;    // final safety clamp

// ---------------------------------------------------------------------------------------------
// Live parameters (tools/app, strisy::EngineParams below). A preset sets all of them; writes can
// arrive between any two blocks. None of them moves a partial frequency: mallet and tone values act
// at the next strike, decay/damper values change decay rates (smoothed), tremolo and rub change
// amplitudes only.

struct Params {
	float level;      // level: strike level (preset level matching)
	float thresh;     // mallet: vpres where a strike starts
	float span;       // mallet: vpres above thresh for a full strike (sensitivity)
	float hard;       // mallet: hardness of the softest strike
	float hardvel;    // mallet: hardness added by strike strength
	float tilt;       // mallet: hardness change per unit tilt (at the strike)
	float mute;       // mallet: fraction of the ringing a full strike stops
	float upper;      // tone: level of all partials but the first, relative to the preset
	float decay;      // decay: T60 scale
	float keytrack;   // decay: T60 ~ f^-keytrack over the keyboard
	float hfdamp;     // decay: extra damping of upper partials, per unit of frequency ratio
	float damper;     // damper: strength on release (1 = vibraphone damper bar)
	float lift;       // damper: key pressure above which the damper is fully lifted
	float pedal;      // damper: how far the pedal lifts the damper (1 = fully)
	float trem;       // trem: motor tremolo on/off (vibraphone)
	float tremrate;   // trem: motor speed (Hz) without gyro
	float tremdepth;  // trem: maximum amplitude modulation depth
	float tremthr;    // trem: key pressure where the pressure tremolo starts
	float rub;        // rub: rubbed level per unit pressure (wine glass; 0 = off)
	float rubnoise;   // rub: noise share of the rubbed excitation
};
static const int NUM_PARAMS = (int)(sizeof(Params) / sizeof(float));

static const strisy::ParamDesc param_desc[NUM_PARAMS] = {
	{"level", "level", 0.1f, 3.0f, 0},
	{"thresh", "mallet", 0.005f, 0.2f, 0},
	{"span", "mallet", 0.2f, 1.5f, 0},
	{"hard", "mallet", 0.2f, 1.2f, 0},
	{"hardvel", "mallet", 0.0f, 1.0f, 0},
	{"tilt", "mallet", 0.0f, 0.5f, 0},
	{"mute", "mallet", 0.0f, 1.0f, 0},
	{"upper", "tone", 0.0f, 2.0f, 0},
	{"decay", "decay", 0.1f, 4.0f, 0},
	{"keytrack", "decay", 0.0f, 1.5f, 0},
	{"hfdamp", "decay", 0.0f, 1.0f, 0},
	{"damper", "damper", 0.0f, 2.0f, 0},
	{"lift", "damper", 0.02f, 0.5f, 0},
	{"pedal", "damper", 0.0f, 1.0f, 0},
	{"trem", "trem", 0.0f, 1.0f, 2},
	{"tremrate", "trem", 1.0f, 10.0f, 0},
	{"tremdepth", "trem", 0.0f, 1.0f, 0},
	{"tremthr", "trem", 0.2f, 1.2f, 0},
	{"rub", "rub", 0.0f, 1.0f, 0},
	{"rubnoise", "rub", 0.0f, 0.3f, 0},
};

struct VoiceState {
	real re[NSLOT], im[NSLOT];       // mode states (slots NMODES.. are always 0)
	real a[NMODES];                  // smoothed per-sample decay of each mode
	// mode list of the current block: active slots (padded to a multiple of 4) and coefficients
	real lc[NSLOT], ls[NSLOT], lb[NSLOT];
	uint8_t lidx[NSLOT];
	int nact;
	uint32_t on_mask;               // slots active in the last block
	real note_prev;
	real sdone;                     // strike strength already delivered in the current strike
	real pbase;                     // pressure before the current rise
	real trem;                      // smoothed tremolo depth (vibraphone)
	real tgain;                     // tremolo gain at the end of the last block
	uint32_t rng;
	int active;
	int fade;                       // fade out over the next block and reset (preset change)
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	const Preset* pr;
	real sr, inv_sr, w_hz;          // w_hz = 2 pi / sr
	Params prm;                     // live parameters
	real inv_span, inv_lift;        // derived from prm
	real sig_ref[NMODES];           // decay rates (1/s) of the preset's partials at note 60
	real gyro;                      // smoothed gyro tremolo depth
	real trem_phase;
	real lim_gain, peak_prev;
	real mix[BLOCK];
	real delay[BLOCK];              // limiter lookahead
	real x[BLOCK];                  // rub noise
	real out[BLOCK];                // voice output
	VoiceState vs[MAX_VOICES];
};

// ---------------------------------------------------------------------------------------------
// Helpers (no libm)

// cos and sin for |x| <= pi/2 (Taylor to x^8 / x^9, error below 4e-6)
static inline real cos_poly(real x) {
	real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
static inline real sin_poly(real x) {
	real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
// sin(2 pi p) for p in [0, 1)
static inline real sin_turn(real p) {
	real x = 6.2831853f * p - 3.1415927f;   // [-pi, pi), sin(x + pi) = -sin(x)
	if (x > 1.5707963f) x = 3.1415927f - x;
	else if (x < -1.5707963f) x = -3.1415927f - x;
	return -sin_poly(x);
}
// 2^p for |p| < 100, relative error ~2e-7 (0.0003 cents as a pitch)
static inline real exp2_poly(real p) {
	p = rmin(rmax(p, -100.0f), 100.0f);
	int i = (int)p;
	if ((real)i > p) i--;                       // floor
	const real z = p - (real)i - 0.5f;          // [-0.5, 0.5)
	const real y = 1.4142136f * (1.0f + z * (0.69314718f + z * (0.24022651f + z * (0.055504109f
		+ z * (0.0096181291f + z * (0.0013333558f + z * 0.00015403530f))))));
	union { uint32_t u; float f; } s = { (uint32_t)(i + 127) << 23 };
	return y * s.f;
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
	for (int k = 0; k < NMODES; k++) s.a[k] = 0.0f;
	s.nact = 0;
	s.on_mask = 0;
	s.sdone = 0.0f;
	s.trem = 0.0f;
	s.tgain = 1.0f;
	s.fade = 0;
}

// Select a preset; callable between blocks at any time. Sounding voices fade out over one block
// (their partials would otherwise jump to the new instrument's ratios).
// Recompute values derived from the parameters, after clamping every parameter into its range
// (NaN becomes the minimum). Cheap; called after each live write and by set_preset.
static inline void params_changed(Engine& e) {
	float* q = &e.prm.level;
	for (int i = 0; i < NUM_PARAMS; i++) {
		const float lo = param_desc[i].min, hi = param_desc[i].max;
		q[i] = q[i] >= lo ? (q[i] <= hi ? q[i] : hi) : lo;
	}
	e.inv_span = 1.0f / e.prm.span;
	e.inv_lift = 1.0f / e.prm.lift;
	const Preset& P = *e.pr;
	for (int k = 0; k < NMODES; k++)
		e.sig_ref[k] = k < P.n ? 6.9077553f * (1.0f + e.prm.hfdamp * (P.p[k].ratio - 1.0f)) / (P.p[k].t60 * e.prm.decay) : 0.0f;
}

// Select a preset (sets all parameters); callable between blocks at any time. Sounding voices fade
// out over one block (their partials would otherwise jump to the new instrument's ratios).
static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	e.preset = i;
	e.pr = &presets[i];
	const Preset& P = *e.pr;
	e.prm.level = P.gain;
	e.prm.thresh = 0.03f;
	e.prm.span = 0.7f;
	e.prm.hard = 0.6f;
	e.prm.hardvel = 0.4f;
	e.prm.tilt = 0.2f;
	e.prm.mute = 0.3f;
	e.prm.upper = 1.0f;
	e.prm.decay = 1.0f;
	e.prm.keytrack = P.alpha;
	e.prm.hfdamp = 0.0f;
	e.prm.damper = P.damper;
	e.prm.lift = 0.15f;
	e.prm.pedal = 1.0f;
	e.prm.trem = P.tremolo > 0.0f ? 1.0f : 0.0f;
	e.prm.tremrate = 5.0f;
	e.prm.tremdepth = 0.6f;
	e.prm.tremthr = 0.7f;
	e.prm.rub = P.rub;
	e.prm.rubnoise = 0.05f;
	params_changed(e);
	for (int v = 0; v < MAX_VOICES; v++)
		if (e.vs[v].active) e.vs[v].fade = 1;
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.w_hz = 6.2831853f / e.sr;
	e.gyro = 0.0f; e.trem_phase = 0.0f;
	e.lim_gain = 1.0f;
	e.peak_prev = 0.0f;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int n = 0; n < BLOCK; n++) { e.mix[n] = 0.0f; e.delay[n] = 0.0f; e.x[n] = 0.0f; e.out[n] = 0.0f; }
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		VoiceState& s = e.vs[v];
		reset_voice(s);
		s.note_prev = 69.0f;
		s.pbase = 0.0f;
		s.rng = 0x9e3779b9u * (uint32_t)(v + 1);
		s.active = 0;
	}
	set_preset(e, MODAL_PRESET);
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

// strike amplitude for strength 0..1 (about 26 dB of dynamics)
static inline real strike_level(real st) { return STRIKE_LEVEL * st * (1.5f - 0.5f * st); }

static inline void process_voice(Engine& e, int v, real trem_lfo) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	const Preset& P = *e.pr;
	real* __restrict__ out = e.out;
	const real pres = clampr(c.pres, 0.0f, 1.5f);
	const bool held = pres > 0.0f;
	// strikes only while a key is held: after release the firmware leaves the last vpres in place
	const Params& Q = e.prm;
	const real starget = held ? clampr((c.vpres - Q.thresh) * e.inv_span, 0.0f, 1.0f) : 0.0f;

	if (!s.active) {
		if (!held && starget <= STRIKE_REARM) {
			s.note_prev = c.note;
			s.sdone = 0.0f;
			s.pbase = pres;
			return;
		}
		reset_voice(s);
		s.active = 1;
	} else if (s.fade || fabsf(c.note - s.note_prev) >= 1.0f) {
		// new note (or preset) on a sounding voice: fade the old one out over this block with its
		// old partials (about -45 dB), then start fresh next block (a pending strike comes then)
		for (int m = 0; m < s.nact; m++) { s.lc[m] *= 0.85f; s.ls[m] *= 0.85f; }
		run_modes<false>(s, e.x, out);
		for (int n = 0; n < BLOCK; n++) e.mix[n] += out[n];
		reset_voice(s);
		s.note_prev = c.note;
		s.pbase = 0.0f;   // a new note may strike even when the voice was taken from a held key
		return;
	}
	s.note_prev = c.note;

	const real f0 = 440.0f * exp2_poly((c.note - 69.0f) * (1.0f / 12.0f));
	// decay over the keyboard: T60 ~ (f0 / f_C4)^-alpha
	const real pitch_decay = exp2_poly(Q.keytrack * (c.note - 60.0f) * (1.0f / 12.0f));
	// damper: engages as the key is lifted (fully at release), lifted by the sustain pedal
	const real ped = 1.0f - clampr(e.g.pedal, 0.0f, 1.0f) * Q.pedal;
	const real damp = DAMP_SIGMA * Q.damper * ped * ped * clampr(1.0f - pres * e.inv_lift, 0.0f, 1.0f);

	// --- strike ---------------------------------------------------------------------------------
	// A strike is a fast pressure rise; its strength is delivered as it grows (a rise over several
	// control updates adds up). Only rises that start from a light touch strike: pressure changes
	// while holding must not retrigger the mallet.
	real dlev = 0.0f;
	if (starget <= STRIKE_REARM) {
		s.sdone = 0.0f;
		s.pbase = pres;
	} else if (starget > s.sdone + 0.005f) {
		const real from_light = clampr((0.3f - s.pbase) * 4.0f, 0.0f, 1.0f);
		dlev = from_light * (strike_level(starget) - strike_level(s.sdone)) * Q.level;
		s.sdone = starget;
	}
	const real keep = 1.0f - Q.mute * clampr(dlev * (1.0f / STRIKE_LEVEL), 0.0f, 1.0f);
	// mallet hardness: strike strength, a little tilt (at the moment of the strike only)
	const real hard = (Q.hard + Q.hardvel * starget) * (1.0f + Q.tilt * clampr(c.but_y, -1.0f, 1.0f));
	const real lhard = hard > 1e-3f ? hard : 1e-3f;

	// --- rub (wine glass) -------------------------------------------------------------------------
	const real rublev = Q.rub * rmax(pres - 0.05f, 0.0f);
	const bool rubbing = held && rublev > 1e-4f;
	const real bdamp = -BOW_DAMP * pres;

	// --- partials -------------------------------------------------------------------------------
	uint32_t mask = 0;
	int m = 0;
	const uint32_t was_on = s.on_mask;
	for (int k = 0; k < P.n; k++) {
		const Partial& pt = P.p[k];
		const real f = f0 * pt.ratio;
		const real fade = rmin(clampr((f - F_LO0) * (1.0f / (F_LO1 - F_LO0)), 0.0f, 1.0f),
		                       clampr((F_HI1 - f) * (1.0f / (F_HI1 - F_HI0)), 0.0f, 1.0f));
		if (!(fade > 0.0f)) {
			if (was_on & (1u << k)) { s.re[k] = 0.0f; s.im[k] = 0.0f; s.a[k] = 0.0f; }
			continue;
		}
		mask |= 1u << k;
		real re = s.re[k], im = s.im[k];
		if (dlev > 0.0f) {
			// hardness^hexp as 2^(hexp*log2(hard)) would need a log; hexp is small, use repeated
			// products of the (<= 1.3) hardness with a fractional remainder blended linearly
			real hp = 1.0f, ex = pt.hexp;
			while (ex >= 1.0f) { hp *= lhard; ex -= 1.0f; }
			hp *= 1.0f + ex * (lhard - 1.0f);
			const real amp = dlev * (k > 0 ? pt.amp * Q.upper : pt.amp) * hp * fade;
			re = re * keep + amp;
			im = im * keep;
			s.re[k] = re; s.im[k] = im;
		}
		// rotation: set by the note only
		const real wk = f * e.w_hz;
		real cw, sw;
		if (wk > 1.5707963f) { const real x = 3.1415927f - wk; cw = -cos_poly(x); sw = sin_poly(x); }
		else { cw = cos_poly(wk); sw = sin_poly(wk); }
		const real g = 1.5f - 0.5f * (cw * cw + sw * sw);   // unit magnitude
		cw *= g; sw *= g;
		// decay: natural (per partial, over the keyboard), damper (stronger for high partials), rub
		real sigma = rmax(e.sig_ref[k] * pitch_decay, SIGMA_MIN) + damp * (1.0f + f * (1.0f / 3000.0f));
		real b = 0.0f;
		if (rubbing && pt.bow > 0.0f) {
			const real T = rublev * pt.bow * fade;
			const real mag2 = re * re + im * im;
			const real beta = clampr(BOW_GROW * (1.0f - mag2 / (T * T)), bdamp, BOW_GROW);
			// noise for an rms of rubnoise*T from the noise alone (uniform noise, var 1/3)
			const real an = rmax(sigma * e.inv_sr, 1e-6f);
			b = Q.rubnoise * 1.7320508f * T * 2.0f * (an * rsqrt_fast(an));
			sigma -= beta;
		}
		// decay changes (damper, rub) are smoothed over a few blocks: a step in the decay rate is a
		// kink in the envelope, audible as a tick on loud notes. Frequencies are not affected.
		const real at = clampr(sigma * e.inv_sr, -0.001f, 0.03f);
		const real a = (was_on & (1u << k)) ? s.a[k] + 0.25f * (at - s.a[k]) : at;
		s.a[k] = a;
		const real r = 1.0f + a * (-1.0f + a * (0.5f - a * (1.0f / 6.0f)));   // exp(-a)
		s.lidx[m] = (uint8_t)k;
		s.lc[m] = r * cw;
		s.ls[m] = r * sw;
		s.lb[m] = b;
		m++;
	}
	s.on_mask = mask;
	for (int p = 0; m < NSLOT && (m & 3) != 0; p++, m++) {   // pad with dummy modes
		s.lidx[m] = (uint8_t)(NMODES + p);
		s.lc[m] = 0.0f; s.ls[m] = 0.0f; s.lb[m] = 0.0f;
	}
	s.nact = m;

	// --- audio ----------------------------------------------------------------------------------
	if (rubbing) {
		uint32_t rng = s.rng;
		for (int n = 0; n < BLOCK; n++) {
			rng = rng * 1664525u + 1013904223u;
			e.x[n] = (real)(int32_t)rng * 4.6566129e-10f;
		}
		s.rng = rng;
		run_modes<true>(s, e.x, out);
	} else {
		run_modes<false>(s, e.x, out);
	}

	// vibraphone motor tremolo: pressing harder than a normal hold, or the gyro (shared phase)
	if (Q.trem > 0.5f) {
		const real dt = rmax(clampr((pres - Q.tremthr) * (1.0f / 0.3f), 0.0f, 1.0f), e.gyro);
		s.trem += 0.1f * (dt - s.trem);
		if (!(s.trem > 1e-4f)) s.trem = 0.0f;
	} else {
		s.trem = 0.0f;
	}

	// --- guard, energy cap, gating ----------------------------------------------------------------
	real energy = 0.0f;
	for (int k = 0; k < NMODES; k++) {
		const real m2 = s.re[k] * s.re[k] + s.im[k] * s.im[k];
		// flush partials that decayed below -200 dB before they reach denormals
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
	// tremolo gain (depth 0.6 at most; amplitude only, never pitch), interpolated over the block
	const real g1 = 1.0f - Q.tremdepth * s.trem * (0.5f + 0.5f * trem_lfo);
	if (g1 != 1.0f || s.tgain != 1.0f) {
		const real dg = (g1 - s.tgain) * (1.0f / BLOCK);
		real g = s.tgain;
		for (int n = 0; n < BLOCK; n++) { g += dg; e.mix[n] += out[n] * g; }
	} else {
		for (int n = 0; n < BLOCK; n++) e.mix[n] += out[n];
	}
	s.tgain = g1;
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
	// gyro -> vibraphone motor (depth follows rotation, ~0.5 s fade after it stops)
	const real rot = fabsf(e.g.rot_x) + fabsf(e.g.rot_y) + fabsf(e.g.rot_z);
	const real gt = rmin(1.5f * rot, 1.0f);
	e.gyro += (gt > e.gyro ? 0.05f : 0.0015f) * (gt - e.gyro);
	if (!(e.gyro > 1e-4f)) e.gyro = 0.0f;
	// motor speed ~5 Hz (a little faster with more rotation); one shared phase like a real motor
	e.trem_phase += (e.prm.tremrate + 2.0f * e.gyro) * BLOCK * e.inv_sr;
	if (e.trem_phase >= 1.0f) e.trem_phase -= 1.0f;
	const real lfo = sin_turn(e.trem_phase);

	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v, lfo);

	real peak = 0.0f, sabs = 0.0f;
	for (int n = 0; n < BLOCK; n++) {
		const real a = fabsf(e.mix[n]);
		sabs += a;
		peak = rmax(peak, a);
	}
	if (!(sabs < 1e9f)) {   // non-finite: silence the block and reset everything
		for (int n = 0; n < BLOCK; n++) { output[n] = 0.0f; e.mix[n] = 0.0f; e.delay[n] = 0.0f; }
		for (int v = 0; v < e.nvoices; v++) { reset_voice(e.vs[v]); e.vs[v].active = 0; }
		e.lim_gain = 1.0f;
		e.peak_prev = 0.0f;
		return;
	}
	// limiter with one block lookahead: the output is the previous block, its gain ramps from the
	// gain chosen for it (g0) to one that also suits the next block (g1); both are below what the
	// delayed block needs, so the ramp never overshoots. Instant attack, ~0.5 s recovery.
	const real need = rmax(peak, e.peak_prev);
	const real gl = need > LIMIT ? LIMIT / need : 1.0f;
	const real g0 = e.lim_gain;
	const real g1 = rmin(gl, g0 + 0.0015f);
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

// live parameters for the app (see strisy.h)
namespace strisy {
template <> struct EngineParams<modal::Engine> {
	static const int count = modal::NUM_PARAMS;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &modal::param_desc[i] : 0; }
	static float* ptr(modal::Engine& e, int i) { return (i >= 0 && i < count) ? &e.prm.level + i : 0; }
	static void changed(modal::Engine& e, int) { modal::params_changed(e); }
};
} // namespace strisy

#endif // VOICE_MODAL_H_
