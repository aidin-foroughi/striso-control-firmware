/**
 * ep: physically inspired electric piano (Rhodes first, Wurlitzer and friends as presets).
 * Build with `make VOICE=ep`; presets with set_preset() (host renders: -DEP_PRESET=n).
 *
 * Per voice:
 *   tine     four damped modal resonators (coupled form, exact frequency, stable for any decay):
 *              0 tine fundamental, with a two-stage decay (fast "prompt" part after the strike,
 *                then the long tonebar-fed sustain)
 *              1 tonebar partner: the coupled tine + tonebar pair splits into two normal modes a
 *                cent or two apart, heard as a slow bloom / waver of the sustain
 *              2 the tine's second bending mode (inharmonic, ~7x): the "bell", kept modest
 *              3 the third mode (~19x): a short attack tick
 *   hammer   a velocity impulse into the modes at the strike (displacement starts at 0, no click).
 *            The neoprene tip's contact time shortens with velocity and hardness; the upper modes
 *            are weighted by the contact pulse spectrum, so harder strikes ring brighter.
 *   pickup   the Rhodes magnetic pickup: flux through the coil as a function of the tine tip
 *            position, a Lorentzian bump  phi(u) = 1/(1+u^2),  u = drive * x - align. Small tine
 *            motion stays almost linear (round, pure); a hard strike swings the tip across the
 *            pole piece and the curve's asymmetry turns it into harmonics and bark. align is the
 *            tine's offset from the pole (0.58 = inflection, sweetest; > 0.8 barky; < 0.5 hollow,
 *            octave-ish). The Wurlitzer's electrostatic pickup is a biased soft clip instead
 *            (square, nasal); "wurli" morphs between the two. The output is normalised by the
 *            pickup's small-signal slope, so align/drive change the timbre, not the soft level.
 *   damper   on release a felt damper engages (smoothed, stronger on the upper modes); the
 *            pedal lifts it (sustain pedal).
 * Master: body EQ (band-pass boost/cut), amp grit (asymmetric soft clip), tone low-pass, DC
 * blocker, soft safety ceiling at 4.0 (full scale on the board at the usual volume). Optional tape wobble (wow + flutter) on all voices.
 *
 * Playing:
 *   strike (vpres at key-down)  velocity: soft = warm and round, hard = bright, barking
 *   pressure while holding      digging in adds a little pickup drive (bark), nothing else
 *   tilt (but_y)                pickup alignment: back = sweeter/rounder, forward = barkier
 *   release / pedal             damper; pedal 0..1 is a sustain pedal (fully lifted at 0.8)
 *   repeated strikes            lifting a held key a little and striking again re-strikes
 *
 * Cost: the per-sample loop is 43 instructions per voice-sample (30 FP, 1 division, all in
 * registers; 49 for the Wurlitzer curve, 67 with 2 divisions while morphing between the two),
 * coefficients once per block (~1.5k cycles per voice). Estimate ~3.5k cycles per voice per block,
 * ~37k cycles per block for 10 voices + master (~14 % CPU). Silent voices are skipped.
 * Engine 2.2 KB (DTCM), no other buffers. No libm, no doubles. Per-voice NaN/energy guard,
 * energy cap, slewed frequency-ratio parameters, master ceiling.
 */
#ifndef VOICE_EP_H_
#define VOICE_EP_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace ep {

using real = float;
static constexpr int BLOCK = strisy::BLOCK;
static constexpr int MAX_VOICES = 10;
static constexpr int NM = 4; // modes per voice

using strisy::rmax;
using strisy::rmin;

// ---------------------------------------------------------------------------------------------
// Sound design parameters (all floats: they are the live parameters, see EngineParams below)

struct Params {
	// hammer
	real hard;  // hammer hardness (shorter contact -> brighter, more bell/tick)
	real vsens; // velocity sensitivity: vel = (vpres - 0.02) * vsens
	real dyn;   // dynamic range: amplitude = vel * ((1 - dyn) + dyn * vel)
	// tine
	real decay;  // T60 of the fundamental at C4 (s)
	real dkey;   // decay keytracking: T60 ~ 2^(-(note-60)/12 * dkey)
	real prompt; // amount of the fast initial decay (0..1, about 0..20 dB in the first 0.3 s)
	real bell;   // level of the 2nd tine mode (relative displacement at a medium strike)
	real bratio; // its frequency ratio
	real bdecay; // its T60 at C4 (s)
	real tick;   // level of the 3rd tine mode (attack tick)
	real tratio; // its frequency ratio
	// tonebar
	real beat;   // level of the tonebar partner mode
	real bcents; // its detune (cents): bloom / waver speed
	// pickup
	real align; // tine offset from the pole piece (0.58 sweet, > 0.8 bark, < 0.5 hollow)
	real tilt;  // forward/back tilt -> align depth
	real drive; // tine displacement into the pickup (more = more bark at the same velocity)
	real dig;   // pressure while holding -> extra drive
	real wurli; // 0 magnetic (Rhodes) .. 1 electrostatic (Wurlitzer) pickup curve
	// damper
	real damper; // damper strength (0 = no dampers)
	// amp
	real gain;   // output level
	real grit;   // amp drive 0..1
	real tone;   // amp/speaker low-pass (Hz)
	real bodyhz; // body EQ centre (Hz)
	real body;   // body EQ gain (-0.9 cut .. 2 boost)
	// mod
	real wobble; // tape wow + flutter depth (cents)
};
static constexpr int NUM_PARAMS = 25;
static_assert(sizeof(Params) == NUM_PARAMS * sizeof(real), "Params must be plain floats");

[[nodiscard]] static inline Params base_params() {
	Params p;
	p.hard = 1.0f;
	p.vsens = 1.4f;
	p.dyn = 0.8f;
	p.decay = 7.0f;
	p.dkey = 0.65f;
	p.prompt = 0.35f;
	p.bell = 0.09f;
	p.bratio = 6.9f;
	p.bdecay = 0.9f;
	p.tick = 0.08f;
	p.tratio = 19.0f;
	p.beat = 0.12f;
	p.bcents = 1.2f;
	p.align = 0.95f;
	p.tilt = 0.3f;
	p.drive = 1.1f;
	p.dig = 0.4f;
	p.wurli = 0.0f;
	p.damper = 1.0f;
	p.gain = 2.27f;
	p.grit = 0.15f;
	p.tone = 7000.0f;
	p.bodyhz = 250.0f;
	p.body = 0.25f;
	p.wobble = 0.0f;
	return p;
}

static constexpr int NUM_PRESETS = 14;

[[nodiscard]] static inline const char* preset_name(int i) {
	static constexpr const char* names[NUM_PRESETS] = {
	    "stage", "suitcase", "mk2", "dyno", "ballad", "bark", "neosoul",
	    "fm80s", "lofi", "wurli", "wurlibark", "pianet", "felt", "celesta"};
	return (i >= 0 && i < NUM_PRESETS) ? names[i] : names[0];
}

[[nodiscard]] static inline Params preset_params(int i) {
	Params p = base_params();
	switch (i) {
	case 1: // suitcase: rounder, more low-mid body, darker speaker, a little amp grit
		p.hard = 0.85f;
		p.decay = 8.0f;
		p.prompt = 0.35f;
		p.bell = 0.1f;
		p.tick = 0.07f;
		p.align = 0.72f;
		p.drive = 1.15f;
		p.grit = 0.3f;
		p.tone = 5000.0f;
		p.bodyhz = 180.0f;
		p.body = 0.6f;
		p.gain = 1.93f;
		break;
	case 2: // mk2: brighter and tighter, shorter sustain, harder hammers
		p.hard = 1.3f;
		p.decay = 5.0f;
		p.dkey = 0.75f;
		p.prompt = 0.5f;
		p.bell = 0.2f;
		p.bratio = 7.4f;
		p.bdecay = 0.7f;
		p.tick = 0.13f;
		p.align = 0.72f;
		p.drive = 1.1f;
		p.grit = 0.08f;
		p.tone = 9500.0f;
		p.bodyhz = 2500.0f;
		p.body = 0.25f;
		p.gain = 3.46f;
		break;
	case 3: // dyno: bright, sparkling, clean; more (but still moderate) bell and presence
		p.hard = 1.6f;
		p.bell = 0.25f;
		p.bratio = 7.0f;
		p.bdecay = 1.1f;
		p.tick = 0.15f;
		p.beat = 0.1f;
		p.bcents = 2.5f;
		p.align = 0.62f;
		p.drive = 0.9f;
		p.grit = 0.05f;
		p.tone = 12000.0f;
		p.bodyhz = 3200.0f;
		p.body = 0.5f;
		p.gain = 2.62f;
		break;
	case 4: // ballad: soft hammers, sweet alignment, long sustain, warm and clean
		p.hard = 0.7f;
		p.vsens = 1.2f;
		p.dyn = 0.7f;
		p.decay = 9.0f;
		p.prompt = 0.35f;
		p.bell = 0.07f;
		p.tick = 0.04f;
		p.align = 0.62f;
		p.drive = 0.8f;
		p.dig = 0.25f;
		p.grit = 0.0f;
		p.tone = 5500.0f;
		p.bodyhz = 300.0f;
		p.body = 0.2f;
		p.gain = 2.30f;
		break;
	case 5: // bark: tine close to the pole, high drive, amp pushed; digging in growls
		p.hard = 1.2f;
		p.decay = 6.0f;
		p.prompt = 0.5f;
		p.bell = 0.12f;
		p.align = 1.05f;
		p.tilt = 0.35f;
		p.drive = 2.0f;
		p.dig = 0.8f;
		p.grit = 0.55f;
		p.tone = 6000.0f;
		p.bodyhz = 700.0f;
		p.body = 0.4f;
		p.gain = 2.92f;
		break;
	case 6: // neosoul: dark Mk I, long sustain, heavy low body, mild bark and grit
		p.hard = 0.8f;
		p.decay = 10.0f;
		p.prompt = 0.4f;
		p.bell = 0.08f;
		p.tick = 0.05f;
		p.align = 0.85f;
		p.drive = 1.4f;
		p.dig = 0.5f;
		p.grit = 0.3f;
		p.tone = 3500.0f;
		p.bodyhz = 140.0f;
		p.body = 0.8f;
		p.gain = 1.75f;
		break;
	case 7: // fm80s: DX7-style tine: harmonic 14x bell, detuned 1:1 partner, clean pickup
		p.hard = 1.4f;
		p.decay = 4.5f;
		p.dkey = 0.8f;
		p.prompt = 0.4f;
		p.bell = 0.45f;
		p.bratio = 14.0f;
		p.bdecay = 0.6f;
		p.tick = 0.0f;
		p.beat = 0.35f;
		p.bcents = 7.0f;
		p.align = 0.6f;
		p.drive = 0.6f;
		p.dig = 0.2f;
		p.damper = 0.8f;
		p.grit = 0.0f;
		p.tone = 14000.0f;
		p.body = 0.0f;
		p.gain = 3.27f;
		break;
	case 8: // lofi: worn suitcase on tape: wow and flutter, saturation, dull top
		p.hard = 0.9f;
		p.decay = 7.0f;
		p.bell = 0.1f;
		p.align = 0.8f;
		p.drive = 1.4f;
		p.grit = 0.5f;
		p.tone = 3800.0f;
		p.bodyhz = 400.0f;
		p.body = 0.5f;
		p.wobble = 9.0f;
		p.gain = 2.20f;
		break;
	case 9: // wurli: 200A reed + electrostatic pickup: nasal, square-ish bark when hard
		p.wurli = 1.0f;
		p.align = 0.72f;
		p.drive = 2.0f;
		p.hard = 1.1f;
		p.decay = 3.2f;
		p.dkey = 0.7f;
		p.prompt = 0.4f;
		p.bell = 0.08f;
		p.bratio = 6.4f;
		p.bdecay = 0.4f;
		p.tick = 0.05f;
		p.beat = 0.0f;
		p.damper = 1.4f;
		p.grit = 0.2f;
		p.tone = 6500.0f;
		p.bodyhz = 1100.0f;
		p.body = 0.7f;
		p.gain = 2.80f;
		break;
	case 10: // wurlibark: driven Wurlitzer, more asymmetry, digging in growls
		p.wurli = 1.0f;
		p.align = 0.85f;
		p.drive = 3.2f;
		p.dig = 0.8f;
		p.hard = 1.2f;
		p.decay = 3.2f;
		p.dkey = 0.7f;
		p.prompt = 0.45f;
		p.bell = 0.08f;
		p.bratio = 6.4f;
		p.bdecay = 0.4f;
		p.tick = 0.05f;
		p.beat = 0.0f;
		p.damper = 1.4f;
		p.grit = 0.5f;
		p.tone = 5500.0f;
		p.bodyhz = 1000.0f;
		p.body = 0.6f;
		p.gain = 3.21f;
		break;
	case 11: // pianet: plucked reed, short and woody-bright, no bell, half electrostatic
		p.wurli = 0.6f;
		p.hard = 1.5f;
		p.decay = 1.4f;
		p.dkey = 0.5f;
		p.prompt = 0.7f;
		p.bell = 0.06f;
		p.bratio = 6.3f;
		p.tick = 0.1f;
		p.beat = 0.0f;
		p.align = 0.8f;
		p.drive = 1.3f;
		p.damper = 2.0f;
		p.tone = 7000.0f;
		p.bodyhz = 1500.0f;
		p.body = 0.5f;
		p.gain = 2.85f;
		break;
	case 12: // felt: muted EP, soft felt hammer, short sustain, dark
		p.hard = 0.4f;
		p.decay = 1.2f;
		p.dkey = 0.5f;
		p.prompt = 0.3f;
		p.bell = 0.05f;
		p.tick = 0.0f;
		p.align = 0.65f;
		p.drive = 0.9f;
		p.damper = 1.6f;
		p.tone = 3200.0f;
		p.bodyhz = 220.0f;
		p.body = 0.3f;
		p.grit = 0.1f;
		p.gain = 2.49f;
		break;
	case 13: // celesta: toy / celesta-like: hard hammer, consonant 4x partial, short and clean
		p.hard = 2.2f;
		p.decay = 2.5f;
		p.dkey = 0.6f;
		p.prompt = 0.2f;
		p.bell = 0.3f;
		p.bratio = 4.0f;
		p.bdecay = 0.5f;
		p.tick = 0.3f;
		p.tratio = 10.7f;
		p.beat = 0.0f;
		p.align = 0.6f;
		p.drive = 0.5f;
		p.damper = 0.8f;
		p.grit = 0.0f;
		p.tone = 14000.0f;
		p.bodyhz = 2000.0f;
		p.body = 0.3f;
		p.gain = 2.57f;
		break;
	default:
		break;
	}
	return p;
}

// preset after init() (host renders: -DEP_PRESET=n)
#ifdef EP_PRESET
static constexpr int DEFAULT_PRESET = EP_PRESET;
#else
static constexpr int DEFAULT_PRESET = 0;
#endif

// ---------------------------------------------------------------------------------------------
// Constants

static constexpr real F_HI0 = 14000.0f, F_HI1 = 19000.0f; // upper modes fade out between these
static constexpr real STRIKE_THR = 0.15f;                 // vpres re-striking a held (re-armed) key
static constexpr real DAMP_PRES = 0.06f;                  // below this key pressure the damper starts to touch
static constexpr int ARM_BLOCKS = 2;                      // strike fires at the vpres peak, at most 2 blocks later
static constexpr int FADE_BLOCKS = 3;                     // fade-out of a voice taken over by a new key (2 ms)
static constexpr real VEL_MIN = 0.06f;                    // softest note (a very slow press still sounds)
static constexpr real PROMPT_SIG = 7.0f;                  // extra decay rate (1/s) of the prompt stage at prompt = 1
static constexpr real PROMPT_TAU = 0.3f;                  // prompt stage time constant at C4 (s)
static constexpr real DAMP_SIG = 40.0f;                   // damper decay rate (1/s, T60 0.17 s) at C4, damper = 1
static constexpr real DIG_MAX = 0.6f;                     // pressure -> drive: up to +60 % at dig = 1
static constexpr real HAMMER_C = 0.25f;                   // f0 * contact time at vel 0 is HAMMER_C * 1.5 / hard
static constexpr real U_REF = 0.225f;                     // f0 * contact time of the reference strike (vel 0.6)
static constexpr real ENERGY_CAP = 12.0f;                 // soft cap on the mode energy (sum |z|^2) of a voice
static constexpr real ENERGY_BAD = 1e4f;                  // above this (or NaN) the voice is reset
static constexpr real GATE_ENERGY = 1e-11f;               // a voice below this goes idle (about -110 dB)

struct VoiceState {
	real re[NM], im[NM];           // mode states; im is the displacement
	real cc[NM], ss[NM];           // rotation coefficients of the last block (r cos w, r sin w)
	real a[NM];                    // smoothed per-sample decay of each mode
	real pk;                       // prompt-stage envelope (1 at the strike)
	real damp;                     // smoothed damper engagement 0..1
	real ty, ps;                   // smoothed tilt and pressure
	real pD, pA, pB, pP0, pNG, pW; // pickup values at the end of the last block
	real vpk;                      // vpres peak while arming
	real ppk;                      // pressure peak since the strike
	real note_prev;
	int arm;   // blocks until the armed strike fires (0: none)
	int rearm; // a held key may strike again
	int held_prev;
	int active;
	int fresh; // pickup interpolation starts from this block's values
	int fade;  // blocks left of the fade-out of a stolen voice
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	Params p;
	real sr, inv_sr, w_hz, block_dt;
	// master
	strisy::SvfCoef body_c, tone_c;
	real b1, b2, t1, t2, dc, dc_c;
	real grit_L, grit_iL, grit_b, grit_sb;
	// tape wobble
	real wph1, wph2, wob;
	// frequency-ratio parameters, slewed so live writes glide instead of jumping partials
	real sm_beat, sm_boct, sm_bratio, sm_tratio;
	real out[BLOCK];
	real mix[BLOCK];
	VoiceState vs[MAX_VOICES];
};

// ---------------------------------------------------------------------------------------------
// Helpers (no libm)

[[nodiscard]] static inline real clampr(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }

// cos and sin for |x| <= pi/2 (Taylor to x^8 / x^9, error below 4e-6)
[[nodiscard]] static inline real cos_poly(real x) {
	real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
[[nodiscard]] static inline real sin_poly(real x) {
	real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
// sin(2 pi p) for p in [0, 1)
[[nodiscard]] static inline real sin_turn(real p) {
	real x = 6.2831853f * p - 3.1415927f;
	if (x > 1.5707963f) x = 3.1415927f - x;
	else if (x < -1.5707963f) x = -3.1415927f - x;
	return -sin_poly(x);
}
// 2^p for |p| < 100, relative error ~2e-7
[[nodiscard]] static inline real exp2_poly(real p) {
	p = clampr(p, -100.0f, 100.0f);
	int i = (int)p;
	if ((real)i > p) i--;
	const real z = p - (real)i - 0.5f;
	const real y = 1.4142136f * (1.0f + z * (0.69314718f + z * (0.24022651f + z * (0.055504109f + z * (0.0096181291f + z * (0.0013333558f + z * 0.00015403530f))))));
	union {
		uint32_t u;
		float f;
	} s = {(uint32_t)(i + 127) << 23};
	return y * s.f;
}
// 1/sqrt(x) for x > 0, relative error ~5e-6
[[nodiscard]] static inline real rsqrt_fast(real x) {
	union {
		float f;
		uint32_t i;
	} u = {x};
	u.i = 0x5f375a86u - (u.i >> 1);
	real y = u.f;
	y = y * (1.5f - 0.5f * x * y * y);
	y = y * (1.5f - 0.5f * x * y * y);
	return y;
}
// tan(x) for 0 <= x < 1.5 ([7/6] Pade, as strisy::svf_pade)
[[nodiscard]] static inline real tan_pade(real x) {
	const real x2 = x * x;
	return x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2))) /
	    (135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2)));
}
// soft clipper: x - 4/27 x^3 on |x| <= 1.5, +-1 beyond (C1, slope 1 at 0)
[[nodiscard]] static inline real sat(real x) {
	x = clampr(x, -1.5f, 1.5f);
	return x * (1.0f - 0.148148148f * x * x);
}
// electrostatic pickup curve: tanh Pade x(27+x^2)/(27+9x^2), +-1 at |x| >= 3 (C1)
[[nodiscard]] static inline real wcurve(real x) {
	x = clampr(x, -3.0f, 3.0f);
	const real x2 = x * x;
	return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
[[nodiscard]] static inline real wslope(real x) {
	x = clampr(x, -3.0f, 3.0f);
	const real x2 = x * x;
	const real d = 27.0f + 9.0f * x2;
	return ((27.0f + 3.0f * x2) * d - (27.0f + x2) * 18.0f * x2) / (d * d);
}
// hammer contact spectrum weight of a mode at f * contact time u (smooth low-pass)
[[nodiscard]] static inline real hammer_w(real u) {
	const real u2 = u * u;
	return 1.0f / (1.0f + u2 * (4.0f + u2));
}
// level of a mode with frequency ratio relative to the reference strike, soft-limited to 3x
// (a very hard hammer cannot make the upper modes louder without bound)
[[nodiscard]] static inline real hammer_rel(real ratio, real u1) {
	const real h = hammer_w(ratio * u1) / hammer_w(ratio * U_REF);
	return 3.0f * h / (h + 2.0f);
}

static inline void reset_voice(VoiceState& s) {
	for (int k = 0; k < NM; k++) {
		s.re[k] = s.im[k] = 0.0f;
		s.cc[k] = s.ss[k] = 0.0f;
		s.a[k] = 0.0f;
	}
	s.pk = 0.0f;
	s.damp = 0.0f;
	s.fresh = 1;
	s.fade = 0;
	s.active = 0;
}

// derived master values (filters, grit); called after any parameter change
static inline void derive(Engine& e) {
	const Params& P = e.p;
	const real ft = clampr(P.tone, 200.0f, 0.4f * e.sr);
	e.tone_c = strisy::svf_from_g(tan_pade(3.14159265f * ft * e.inv_sr), 0.707f);
	const real fb = clampr(P.bodyhz, 30.0f, 0.4f * e.sr);
	e.body_c = strisy::svf_from_g(tan_pade(3.14159265f * fb * e.inv_sr), 0.9f);
	const real gr = clampr(P.grit, 0.0f, 1.0f);
	e.grit_L = 6.0f - 5.1f * gr; // soft clip ceiling: 6 (clean) .. 0.9 (gritty)
	e.grit_iL = 1.0f / e.grit_L;
	e.grit_b = 0.25f * gr; // asymmetry (even harmonics) grows with grit
	e.grit_sb = sat(e.grit_b);
}

// Select a preset; callable between blocks at any time. Sounding voices continue with the new
// parameters (every state stays bounded for any parameter set).
// targets of the slewed frequency-ratio values
static inline void ratio_targets(const Params& P, real& beat, real& boct, real& br, real& tr) {
	beat = clampr(P.beat, 0.0f, 1.0f);
	boct = clampr(P.bcents, 0.0f, 50.0f) * (1.0f / 1200.0f) / (1.0f + beat);
	br = clampr(P.bratio, 1.5f, 30.0f);
	tr = clampr(P.tratio, 2.0f, 40.0f);
}

static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	e.preset = i;
	e.p = preset_params(i);
	derive(e);
	ratio_targets(e.p, e.sm_beat, e.sm_boct, e.sm_bratio, e.sm_tratio);
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.w_hz = 6.2831853f / e.sr;
	e.block_dt = (real)BLOCK / e.sr;
	e.b1 = e.b2 = e.t1 = e.t2 = e.dc = 0.0f;
	e.dc_c = 6.2831853f * 8.0f / e.sr; // 8 Hz DC blocker
	e.wph1 = 0.0f;
	e.wph2 = 0.37f;
	e.wob = 0.0f;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int n = 0; n < BLOCK; n++) {
		e.out[n] = 0.0f;
		e.mix[n] = 0.0f;
	}
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		VoiceState& s = e.vs[v];
		s = VoiceState();
		reset_voice(s);
		s.note_prev = 69.0f;
		s.arm = 0;
		s.rearm = 1;
		s.held_prev = 0;
		s.vpk = 0.0f;
		s.ppk = 0.0f;
		s.ty = 0.0f;
		s.ps = 0.0f;
	}
	set_preset(e, DEFAULT_PRESET);
}

// pickup values for alignment a and drive scale
struct Pick {
	real D, A, B, P0, NG, W;
};
[[nodiscard]] static inline Pick pickup_vals(const Params& P, real a, real dig_gain, real reg, real rs_reg) {
	Pick k;
	k.W = clampr(P.wurli, 0.0f, 1.0f);
	k.A = a;
	k.B = 0.9f * (a - 0.45f); // Wurlitzer bias from the same alignment
	const real D0 = clampr(P.drive, 0.05f, 8.0f) * reg;
	k.D = D0 * dig_gain;
	const real rh = 1.0f / (1.0f + a * a);
	const real slr = 2.0f * a * rh * rh; // phi'(-a), > 0.25 for a in [0.3, 1.6]
	k.P0 = rh + k.W * (wcurve(k.B) - rh);
	const real sl = slr + k.W * (wslope(k.B) - slr);
	// small-signal gain: gain * x * reg^(1/4) (lower notes a little louder, as on the real thing)
	k.NG = clampr(P.gain, 0.0f, 10.0f) * rs_reg / (D0 * rmax(sl, 0.1f));
	return k;
}

// the modes and the pickup over one block; returns the sum of the output
template <int MODE> // 0: magnetic only, 1: electrostatic only, 2: both (morph)
[[nodiscard]] static inline real render(VoiceState& s, const Pick& p0, const Pick& p1, real* __restrict__ out) {
	real r0 = s.re[0], q0 = s.im[0], r1 = s.re[1], q1 = s.im[1];
	real r2 = s.re[2], q2 = s.im[2], r3 = s.re[3], q3 = s.im[3];
	const real c0 = s.cc[0], c1 = s.cc[1], c2 = s.cc[2], c3 = s.cc[3];
	const real s0 = s.ss[0], s1 = s.ss[1], s2 = s.ss[2], s3 = s.ss[3];
	const real ib = 1.0f / BLOCK;
	real D = p0.D, A = p0.A, B = p0.B, P0 = p0.P0, NG = p0.NG, W = p0.W;
	const real dD = (p1.D - D) * ib, dA = (p1.A - A) * ib, dB = (p1.B - B) * ib;
	const real dP0 = (p1.P0 - P0) * ib, dNG = (p1.NG - NG) * ib, dW = (p1.W - W) * ib;
	real sum = 0.0f;
	for (int n = 0; n < BLOCK; n++) {
		const real t0 = c0 * r0 - s0 * q0, t1 = c1 * r1 - s1 * q1;
		const real t2 = c2 * r2 - s2 * q2, t3 = c3 * r3 - s3 * q3;
		q0 = s0 * r0 + c0 * q0;
		q1 = s1 * r1 + c1 * q1;
		q2 = s2 * r2 + c2 * q2;
		q3 = s3 * r3 + c3 * q3;
		r0 = t0;
		r1 = t1;
		r2 = t2;
		r3 = t3;
		D += dD;
		P0 += dP0;
		NG += dNG;
		const real x = (q0 + q1) + (q2 + q3);
		const real xd = D * x;
		real phi;
		if (MODE == 0) {
			A += dA;
			const real u = xd - A;
			phi = 1.0f / (1.0f + u * u);
		} else if (MODE == 1) {
			B += dB;
			phi = wcurve(xd + B);
		} else {
			A += dA;
			B += dB;
			W += dW;
			const real u = xd - A;
			const real rh = 1.0f / (1.0f + u * u);
			phi = rh + W * (wcurve(xd + B) - rh);
		}
		const real y = (phi - P0) * NG;
		out[n] = y;
		sum += y;
	}
	s.re[0] = r0;
	s.im[0] = q0;
	s.re[1] = r1;
	s.im[1] = q1;
	s.re[2] = r2;
	s.im[2] = q2;
	s.re[3] = r3;
	s.im[3] = q3;
	return sum;
}

static inline real render_any(VoiceState& s, const Pick& p0, const Pick& p1, real* out) {
	if (p0.W <= 0.0f && p1.W <= 0.0f) return render<0>(s, p0, p1, out);
	if (p0.W >= 1.0f && p1.W >= 1.0f) return render<1>(s, p0, p1, out);
	return render<2>(s, p0, p1, out);
}

[[nodiscard]] static inline Pick last_pick(const VoiceState& s) {
	Pick k = {s.pD, s.pA, s.pB, s.pP0, s.pNG, s.pW};
	return k;
}

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const Params& P = e.p;
	const strisy::VoiceControls c = e.vc[v];
	real* __restrict__ out = e.out;
	real* __restrict__ mix = e.mix;

	const real pres = clampr(c.pres, 0.0f, 1.5f);
	const bool held = pres > 0.0f;
	// after a release the firmware leaves the last vpres in place: ignore positive values then
	const real vp = clampr(c.vpres, -1.0f, held ? 1.5f : 0.0f);
	const real note = clampr(c.note, 0.0f, 135.0f);
	const bool jump = fabsf(note - s.note_prev) >= 1.0f; // reassigned to a new key
	s.note_prev = note;

	// --- key events and strike arming ------------------------------------------------------------
	const bool keydown = held && (!s.held_prev || jump);
	s.held_prev = held;
	bool fire = false;
	if (keydown) {
		s.arm = ARM_BLOCKS;
		s.vpk = vp;
		s.rearm = 0;
		s.ppk = pres;
	} else if (held) {
		// a clear lift (below 60 % of the held pressure, or a fast lift) re-arms the hammer, so
		// digging in never re-strikes but a quick lift-and-press repeats the note
		s.ppk = rmax(s.ppk, pres);
		if (vp < -0.12f || pres < 0.6f * s.ppk) s.rearm = 1;
		if (s.arm > 0) {
			const bool peak_passed = s.vpk > 0.05f && vp < 0.75f * s.vpk;
			s.vpk = rmax(s.vpk, vp);
			if (--s.arm <= 0 || peak_passed) {
				fire = true;
				s.arm = 0;
			}
		} else if (s.rearm && vp > STRIKE_THR) {
			s.arm = ARM_BLOCKS;
			s.vpk = vp;
			s.rearm = 0;
			s.ppk = pres;
		}
	} else {
		if (s.arm > 0) {
			fire = true;
			s.arm = 0;
		} // a tap shorter than the arming window
		s.rearm = 1;
	}

	// --- new key on a sounding voice: fade the old note out over FADE_BLOCKS (2 ms) with its old
	// pitch, then start fresh; a strike arriving meanwhile waits for the end of the fade ----------
	if (jump && s.active && s.fade == 0) s.fade = FADE_BLOCKS;
	if (s.fade > 0) {
		const Pick pk = last_pick(s);
		render_any(s, pk, pk, out);
		const int j = FADE_BLOCKS - s.fade;
		const real r = 1.0f / (FADE_BLOCKS * BLOCK);
		for (int n = 0; n < BLOCK; n++) {
			const real g = 1.0f - (real)(j * BLOCK + n + 1) * r;
			mix[n] += out[n] * (g * g * (3.0f - 2.0f * g));
		}
		if (--s.fade == 0) reset_voice(s);
		if (fire) s.arm = 1; // fire after the fade
		return;
	}
	if (jump) reset_voice(s);

	// --- block-rate control values ---------------------------------------------------------------
	const real by = clampr(c.but_y, -1.0f, 1.0f);
	if (fire && !s.active) {
		s.ty = by;
		s.ps = pres;
	}
	s.ty += 0.08f * (by - s.ty);
	s.ps += 0.05f * (pres - s.ps);
	const real dn = note - 60.0f;
	const real f0 = 440.0f * exp2_poly((note + e.wob - 69.0f) * (1.0f / 12.0f));
	// tonebar pair: the fundamental and its partner are split around the note, weighted by their
	// levels, so the perceived pitch (the level-weighted mean) is the note itself
	const real beat = e.sm_beat, boct = e.sm_boct;
	const real ratio[NM] = {exp2_poly(-beat * boct), exp2_poly(boct), e.sm_bratio, e.sm_tratio};
	real fade[NM];
	for (int k = 0; k < NM; k++)
		fade[k] = clampr((F_HI1 - f0 * ratio[k]) * (1.0f / (F_HI1 - F_HI0)), 0.0f, 1.0f);

	// --- strike: velocity impulse into the modes ---------------------------------------------------
	if (fire) {
		if (!s.active) reset_voice(s);
		const real vel = clampr((s.vpk - 0.02f) * P.vsens, VEL_MIN, 1.0f);
		const real dyn = clampr(P.dyn, 0.0f, 1.0f);
		const real va = vel * ((1.0f - dyn) + dyn * vel);
		const real hard = clampr(P.hard, 0.1f, 5.0f);
		const real u1 = HAMMER_C * (1.5f - vel) / hard;                       // f0 * contact time
		const real regb = clampr(exp2_poly(dn * (1.0f / 24.0f)), 0.5f, 1.8f); // more bell up high
		real amp[NM];
		amp[0] = va;
		amp[1] = va * beat;
		amp[2] = va * clampr(P.bell, 0.0f, 3.0f) * regb * hammer_rel(ratio[2], u1);
		amp[3] = va * clampr(P.tick, 0.0f, 3.0f) * hammer_rel(ratio[3], u1);
		// a hammer hitting a ringing tine stops part of its motion first
		const real keep = 1.0f - 0.55f * vel;
		for (int k = 0; k < NM; k++) {
			s.re[k] = s.re[k] * keep + amp[k] * fade[k];
			s.im[k] *= keep;
		}
		s.pk = 1.0f;
		s.active = 1;
	}
	if (!s.active) return;

	// --- decays and rotations -------------------------------------------------------------------
	const real lift = clampr((e.g.pedal - 0.2f) * (1.0f / 0.6f), 0.0f, 1.0f);
	// damper: fully on when released, starts touching below DAMP_PRES (a slow lift damps gradually)
	const real dtarget = (1.0f - lift) * (1.0f - lift) * clampr(1.0f - pres * (1.0f / DAMP_PRES), 0.0f, 1.0f);
	s.damp += 0.1f * (dtarget - s.damp);
	if (!(s.damp > 1e-6f)) s.damp = 0.0f;
	const real kdec = exp2_poly(-dn * (1.0f / 12.0f) * clampr(P.dkey, 0.0f, 2.0f));
	const real t0 = clampr(P.decay, 0.05f, 60.0f) * kdec;
	const real tb = clampr(P.bdecay, 0.02f, 10.0f) * exp2_poly(-dn * (0.8f / 12.0f));
	// prompt stage: decays with PROMPT_TAU (shorter for higher notes)
	const real ptau = PROMPT_TAU * clampr(exp2_poly(-dn * (1.0f / 24.0f)), 0.3f, 3.0f);
	s.pk *= 1.0f - e.block_dt / ptau;
	if (!(s.pk > 1e-6f)) s.pk = 0.0f;
	const real sd = DAMP_SIG * clampr(P.damper, 0.0f, 5.0f) * clampr(exp2_poly(dn * (1.0f / 30.0f)), 0.35f, 2.5f) * s.damp;
	real sig[NM];
	sig[0] = 6.9077553f / t0 + clampr(P.prompt, 0.0f, 1.0f) * PROMPT_SIG * s.pk + sd;
	sig[1] = sig[0]; // same decay: the pair's level ratio (and so the perceived pitch) stays put
	sig[2] = 6.9077553f / tb + 1.5f * sd;
	sig[3] = 6.9077553f / (0.15f * tb) + 2.0f * sd;
	for (int k = 0; k < NM; k++) {
		if (!(fade[k] > 0.0f)) { // above the band: mode off
			s.re[k] = s.im[k] = 0.0f;
			s.cc[k] = s.ss[k] = 0.0f;
			s.a[k] = 0.0f;
			continue;
		}
		const real wk = f0 * ratio[k] * e.w_hz;
		real cw, sw;
		if (wk > 1.5707963f) {
			const real x = 3.1415927f - wk;
			cw = -cos_poly(x);
			sw = sin_poly(x);
		} else {
			cw = cos_poly(wk);
			sw = sin_poly(wk);
		}
		const real gn = 1.5f - 0.5f * (cw * cw + sw * sw);
		cw *= gn;
		sw *= gn;
		const real at = clampr(sig[k] * e.inv_sr, 0.0f, 0.05f);
		const real a = fire || s.fresh ? at : s.a[k] + 0.3f * (at - s.a[k]);
		s.a[k] = a;
		const real r = 1.0f + a * (-1.0f + a * (0.5f - a * (1.0f / 6.0f))); // exp(-a)
		s.cc[k] = r * cw;
		s.ss[k] = r * sw;
	}

	// --- pickup ---------------------------------------------------------------------------------
	const real a = clampr(P.align + clampr(P.tilt, 0.0f, 1.0f) * s.ty, 0.3f, 1.6f);
	const real dg = 1.0f + DIG_MAX * clampr(P.dig, 0.0f, 1.0f) * clampr((s.ps - 0.45f) * (1.0f / 0.55f), 0.0f, 1.0f);
	const real reg = clampr(exp2_poly(-(note - 64.0f) * (1.0f / 30.0f)), 0.4f, 1.5f);
	const real rs_reg = rsqrt_fast(rsqrt_fast(reg)); // reg^(1/4)
	const Pick p1 = pickup_vals(P, a, dg, reg, rs_reg);
	const Pick p0 = s.fresh ? p1 : last_pick(s);
	s.fresh = 0;
	const real sum = render_any(s, p0, p1, out);
	s.pD = p1.D;
	s.pA = p1.A;
	s.pB = p1.B;
	s.pP0 = p1.P0;
	s.pNG = p1.NG;
	s.pW = p1.W;

	// --- guard, energy cap, gating ----------------------------------------------------------------
	real energy = 0.0f;
	for (int k = 0; k < NM; k++) {
		const real m2 = s.re[k] * s.re[k] + s.im[k] * s.im[k];
		if (m2 < 1e-24f) {
			s.re[k] = 0.0f;
			s.im[k] = 0.0f;
		} // before denormals
		energy += m2;
	}
	if (!(energy < ENERGY_BAD) || !(fabsf(sum) < 1e5f)) { // NaN, Inf or runaway: drop and reset
		reset_voice(s);
		return;
	}
	if (energy > ENERGY_CAP) {
		const real gsc = 3.4641016f * rsqrt_fast(energy); // sqrt(ENERGY_CAP / energy)
		for (int k = 0; k < NM; k++) {
			s.re[k] *= gsc;
			s.im[k] *= gsc;
		}
	}
	for (int n = 0; n < BLOCK; n++) mix[n] += out[n];
	if (energy < GATE_ENERGY && s.arm == 0) reset_voice(s);
}

[[nodiscard]] static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

static inline void process(Engine& e, real* __restrict__ output) {
#ifdef EP_TEST_SWITCH
	// host test: step to the next preset every EP_TEST_SWITCH blocks, while voices are sounding
	{
		static int nb = 0;
		if (++nb % EP_TEST_SWITCH == 0) set_preset(e, (e.preset + 1) % NUM_PRESETS);
	}
#endif
	// tape wobble: wow ~0.55 Hz + flutter ~6.3 Hz (semitones), shared by all voices like a tape
	const real wd = clampr(e.p.wobble, 0.0f, 100.0f) * 0.01f;
	e.wph1 += 0.55f * e.block_dt;
	if (e.wph1 >= 1.0f) e.wph1 -= 1.0f;
	e.wph2 += 6.3f * e.block_dt;
	if (e.wph2 >= 1.0f) e.wph2 -= 1.0f;
	const real wt = wd * (0.8f * sin_turn(e.wph1) + 0.2f * sin_turn(e.wph2));
	e.wob += 0.2f * (wt - e.wob); // smooth, so depth changes never jump the pitch
	if (!(fabsf(e.wob) < 2.0f)) e.wob = 0.0f;
	{ // ~30 ms glide of the frequency-ratio parameters
		real tb, to, tbr, ttr;
		ratio_targets(e.p, tb, to, tbr, ttr);
		e.sm_beat += 0.02f * (tb - e.sm_beat);
		e.sm_boct += 0.02f * (to - e.sm_boct);
		e.sm_bratio += 0.02f * (tbr - e.sm_bratio);
		e.sm_tratio += 0.02f * (ttr - e.sm_tratio);
	}

	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);

	// master: body EQ -> amp grit -> tone low-pass -> DC blocker -> safety ceiling
	const real body = clampr(e.p.body, -0.95f, 3.0f);
	const strisy::SvfCoef bc = e.body_c, tc = e.tone_c;
	const real kb = body * bc.k; // k * v1 is the unity-peak band-pass
	const real L = e.grit_L, iL = e.grit_iL, gb = e.grit_b, gsb = e.grit_sb;
	real b1 = e.b1, b2 = e.b2, t1 = e.t1, t2 = e.t2, dc = e.dc;
	const real dcc = e.dc_c;
	for (int n = 0; n < BLOCK; n++) {
		real x = e.mix[n], v1, v2, w1, w2;
		SVF_TICK(x, b1, b2, bc, v1, v2);
		x += kb * v1;
		x = L * (sat(x * iL + gb) - gsb);
		SVF_TICK(x, t1, t2, tc, w1, w2);
		dc += dcc * (w2 - dc);
		output[n] = 4.0f * sat((w2 - dc) * (1.0f / 4.0f));
	}
	const real st = fabsf(b1) + fabsf(b2) + fabsf(t1) + fabsf(t2) + fabsf(dc);
	if (!(st < 1e4f)) { // never expected: reset the master filters and mute this block
		b1 = b2 = t1 = t2 = dc = 0.0f;
		for (int n = 0; n < BLOCK; n++) output[n] = 0.0f;
	}
	e.b1 = b1;
	e.b2 = b2;
	e.t1 = t1;
	e.t2 = t2;
	e.dc = dc;
}

} // namespace ep

// ---------------------------------------------------------------------------------------------
// Live parameters

namespace strisy {
template <> struct EngineParams<ep::Engine> {
	static constexpr int count = ep::NUM_PARAMS;
	static const ParamDesc* desc(int i) {
		static constexpr ParamDesc d[ep::NUM_PARAMS] = {
		    {"hard", "hammer", 0.3f, 3.0f, strisy::ParamKind::continuous},
		    {"vsens", "hammer", 0.5f, 4.0f, strisy::ParamKind::continuous},
		    {"dyn", "hammer", 0.0f, 1.0f, strisy::ParamKind::continuous},
		    {"decay", "tine", 0.2f, 30.0f, strisy::ParamKind::continuous},
		    {"dkey", "tine", 0.0f, 1.5f, strisy::ParamKind::continuous},
		    {"prompt", "tine", 0.0f, 1.0f, strisy::ParamKind::continuous},
		    {"bell", "tine", 0.0f, 2.0f, strisy::ParamKind::continuous},
		    {"bratio", "tine", 2.0f, 16.0f, strisy::ParamKind::continuous},
		    {"bdecay", "tine", 0.05f, 5.0f, strisy::ParamKind::continuous},
		    {"tick", "tine", 0.0f, 1.5f, strisy::ParamKind::continuous},
		    {"tratio", "tine", 6.0f, 30.0f, strisy::ParamKind::continuous},
		    {"beat", "tonebar", 0.0f, 0.5f, strisy::ParamKind::continuous},
		    {"bcents", "tonebar", 0.0f, 25.0f, strisy::ParamKind::continuous},
		    {"align", "pickup", 0.3f, 1.6f, strisy::ParamKind::continuous},
		    {"tilt", "pickup", 0.0f, 0.6f, strisy::ParamKind::continuous},
		    {"drive", "pickup", 0.2f, 4.0f, strisy::ParamKind::continuous},
		    {"dig", "pickup", 0.0f, 1.0f, strisy::ParamKind::continuous},
		    {"wurli", "pickup", 0.0f, 1.0f, strisy::ParamKind::continuous},
		    {"damper", "damper", 0.0f, 3.0f, strisy::ParamKind::continuous},
		    {"gain", "amp", 0.0f, 8.0f, strisy::ParamKind::continuous},
		    {"grit", "amp", 0.0f, 1.0f, strisy::ParamKind::continuous},
		    {"tone", "amp", 800.0f, 16000.0f, strisy::ParamKind::continuous},
		    {"bodyhz", "amp", 60.0f, 4000.0f, strisy::ParamKind::continuous},
		    {"body", "amp", -0.9f, 2.0f, strisy::ParamKind::continuous},
		    {"wobble", "mod", 0.0f, 40.0f, strisy::ParamKind::continuous},
		};
		return (i >= 0 && i < ep::NUM_PARAMS) ? &d[i] : 0;
	}
	static float* ptr(ep::Engine& e, int i) {
		static float ep::Params::*const m[ep::NUM_PARAMS] = {
		    &ep::Params::hard,
		    &ep::Params::vsens,
		    &ep::Params::dyn,
		    &ep::Params::decay,
		    &ep::Params::dkey,
		    &ep::Params::prompt,
		    &ep::Params::bell,
		    &ep::Params::bratio,
		    &ep::Params::bdecay,
		    &ep::Params::tick,
		    &ep::Params::tratio,
		    &ep::Params::beat,
		    &ep::Params::bcents,
		    &ep::Params::align,
		    &ep::Params::tilt,
		    &ep::Params::drive,
		    &ep::Params::dig,
		    &ep::Params::wurli,
		    &ep::Params::damper,
		    &ep::Params::gain,
		    &ep::Params::grit,
		    &ep::Params::tone,
		    &ep::Params::bodyhz,
		    &ep::Params::body,
		    &ep::Params::wobble,
		};
		return (i >= 0 && i < ep::NUM_PARAMS) ? &(e.p.*m[i]) : 0;
	}
	static void changed(ep::Engine& e, int) { ep::derive(e); }
};
} // namespace strisy

#endif // VOICE_EP_H_
