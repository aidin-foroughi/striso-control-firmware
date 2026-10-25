/**
 * trumpet: a physical model of the trumpet. Stage 1: one instrument, fitted to sound and respond like
 * the brass engine's "trumpet" (the hybrid); real bore profiles, valves and partials, mutes and the
 * other instruments come later, on top of the same parts. Build with `VOICE=trumpet`.
 *
 * Every voice is a player blowing a horn; nothing sets the pitch but the lips and the bore together:
 *
 *   breath   key pressure -> breath level (as the hybrid: pres (a + b pres) + the strike's accent,
 *            smoothed) -> mouth pressure: from the oscillation threshold of this note (the lightest
 *            touch) to p_loud x threshold (ff). The threshold is computed from the lips' and the
 *            horn's small-signal impedances (below), so every note speaks at the lightest touch
 *   tongue   the strike is the tongue: it releases the air (faster for a harder strike, never faster
 *            than 1 ms), the pressure built up behind it (an accent that decays) and a turbulent "t";
 *            while the strike rises the tongue holds the lips closed and lets them go at its peak (a
 *            harder strike: from further), so they start swinging at once, at their own frequency.
 *            A press without a strike is a breath attack
 *   LIPS     one mechanical mode, outward striking (the pressure difference pushes them open; Elliott
 *            & Bowsher 1982), tuned below the note: mass, Q, rest opening per register, width, contact
 *            stiffness and damping when closed, tissue losses growing with the lip speed, and an
 *            embouchure that firms with the breath (the rest position moves in by part of the static
 *            deflection above the threshold: the aperture holds, so loud playing closes the lips for
 *            part of each cycle). Flow: Bernoulli jet u = w h sqrt(2 (p_m - p_c) / rho), regularised at
 *            small dp, with turbulence riding on its pulsating part. Soft playing: the lips swing
 *            around their opening (round, nearly sinusoidal); loud: narrow buzzy pulses
 *   MOUTHPIECE  cup compliance V / (rho c^2), backbore inertance with a loss in parallel (none for
 *            the mean flow): the Helmholtz "popping" resonance between lips and bore, solved
 *            implicitly (trapezoidal) together with the jet's conductance
 *   BORE     a waveguide (two delay lines, AXI SRAM) whose resonances sit on the harmonics of the note
 *            (the horn a real flare and mouthpiece approximate; its length follows the note, as a
 *            slide). At the bell the low frequencies reflect (one-pole lowpass x the round-trip loss
 *            from a decay time), -1 at DC (the open end: the mean flow neither builds up nor
 *            radiates). The loop delay is solved per block (Newton) so that the small-signal loop gain
 *            of the lips' admittance and the load they see (cup || backbore + bore) is real at the
 *            note: the note sounds in tune at the threshold; the same loop gain gives the threshold.
 *            The player's ear trims the bore slowly for the pull of louder lips (zero crossings of
 *            the lip motion)
 *   BRASSINESS  the wave steepens on its way to the bell (finite-amplitude propagation; Hirschberg et
 *            al. 1996): the radiated wave is read at a delay that moves with the pressure (Pa), in two
 *            rate-limited stages (no fold-back, no aliasing grit); the loop itself stays linear
 *   RADIATION  bell transmission (1 - reflection), radiation high-pass, bell/directivity low-pass
 *
 * Every element is passive or bounded: the jet only draws on the mouth pressure, the bore loop gain is
 * < 0.95, the mouthpiece is solved implicitly, the steepening only re-times samples; lip excursion,
 * flows and cup pressure are clamped; a per-voice guard resets a voice whose state is non-finite or
 * absurd; the master has a DC blocker and a soft ceiling at 4.2.
 *
 * Playing (as the hybrid):
 *   pressure   breath: dynamics and brightness together (pp round -> ff blaring)
 *   strike     tonguing: hard = a fast release, an accent, the lips released from closed (a blat that
 *              scoops up onto the note) and a "t"; soft = "d"; none = a breath attack
 *   release    the breath stops, the bore rings out (pedal lengthens the release)
 *   tilt fwd/back  embouchure: forward firmer (brighter), back looser (darker)
 *   tilt sideways  pitch bend (the firmware moves the note; the horn follows)
 *   shake      flutter tongue
 *
 * Cost (QEMU Cortex-M7 instruction counts, 10 voices at ff, notes jumping every 0.25 s): the engine
 * about 114k instructions per block on average; engine + master effects (every stage on) 140k in the
 * worst block (all ten notes starting in the same block). The brass engine: 92k on the same test.
 * Per voice-sample: three divisions, one square root, ~110 FP operations; per block a few complex
 * impedance evaluations for the loop delay and threshold. Memory: bore delay lines 80 KB (64 KB FastMemory, 16 KB LargeMemory),
 * Engine ~7 KB.
 */
#ifndef VOICE_TRUMPET_H_
#define VOICE_TRUMPET_H_

#include <math.h>
#include <stdint.h>

#include "strisy.h"
#ifdef TRUMPET_TEST_HOOK
#include <stdio.h>
#endif

namespace trumpet {

using real = float;
using strisy::rmax;
using strisy::rmin;

static constexpr int BLOCK = strisy::BLOCK;
static constexpr int MAX_VOICES = 10;
static constexpr int CTRL = 8; // control rate divider (6 kHz)
static constexpr int NCTRL = BLOCK / CTRL;
static_assert(BLOCK % CTRL == 0, "CTRL must divide BLOCK");
static constexpr int BLEN = 1024; // each half of the bore loop (period <= ~2040 samples, MIDI >= 19.5)
static constexpr uint32_t BMASK = BLEN - 1;
static constexpr real DMAX = (real)(BLEN - 3);
static constexpr int SLEN = 128; // second steepening stage
static constexpr uint32_t SMASK = SLEN - 1;
static constexpr int SD = 64; // its nominal delay
static constexpr real RL = 3.0f, RU = 0.95f, RLDIV = 20.0f;
static constexpr real RHO = 1.2f;
static constexpr real C0 = 343.0f;
static constexpr real PI = 3.14159265f;
static constexpr real Z0 = RHO * C0 / (0.25f * PI * 0.0117f * 0.0117f); // bore (11.7 mm) characteristic impedance
static constexpr real NOTE_HI = 100.0f;                                  // above: an octave down (the lips can't)
#ifdef TRUMPET_DC_HZ
static constexpr real DC_HZ = TRUMPET_DC_HZ;
#else
static constexpr real DC_HZ = 2.0f;
#endif

// the bore delay lines, 8 KB per voice, placed by the host (engine SDK): the first NFAST voices' in
// FastMemory (AHB SRAM1/2 on the board), the others' in LargeMemory (AXI SRAM); with VOICE=all
// neither region has room for all 80 KB
static constexpr int NFAST = 8;
struct FastMemory {
	alignas(32) float bore[NFAST][2][BLEN];
};
struct LargeMemory {
	alignas(32) float bore[MAX_VOICES - NFAST][2][BLEN];
};

// ---------------------------------------------------------------------------------------------
// Parameters (all floats: the live parameters)

struct Params {
	// level / breath
	real gain;         // output level (x 1e-4 per Pa)
	real pc_a, pc_b;   // breath from key pressure: pres * (pc_a + pc_b * pres)
	real lv_hz;        // breath smoothing (critically damped 2-pole)
	real rel, rel_ped; // release time constant (s): rel + rel_ped * pedal^2
	real p_soft;       // mouth pressure at the lightest touch, re the oscillation threshold
	real p_loud;       // mouth pressure at full breath, re the threshold (at Bb4)
	real p_reg;        // octaves of that ratio per octave of pitch
	real p_curve;      // breath -> mouth pressure exponent
	// lips
	real lip_tune;  // lip resonance re the note (cents; below: outward striking)
	real lip_treg;  // its change per octave up (cents)
	real lip_q;     // lip resonance Q
	real lip_mass;  // mass per area (kg/m^2) at Bb4
	real mass_reg;  // octaves lighter per octave up
	real lip_open;  // rest opening (mm) at Bb4
	real open_reg;  // octaves smaller per octave up
	real lip_width; // mm
	real contact;   // contact stiffness when closed, re the lip stiffness
	real lip_nl;    // tissue losses growing with the lip speed: x (1 + lip_nl (v / (wl x0))^2)
	real embouch;   // the embouchure firms with the breath: the lips' rest position moves in by this part
	                // of the static deflection of the pressure above the threshold (1: the aperture holds)
	real lip_tilt;  // forward tilt firms the lips: octaves of opening per unit tilt
	real jitter;    // lip tension jitter (cents)
	// mouthpiece
	real cup;    // cup volume (cm^3)
	real pop_hz; // mouthpiece (Helmholtz) resonance
	real pop_q;  // its Q into the bore
	// bore
	real bore_t;  // resonance decay time (s) at Bb4
	real bore_reg; // octaves shorter per octave up
	real bell_hz; // bell reflection cutoff
	real brass;   // steepening: shift (samples) per 10 kPa of wave
	// radiation
	real rad_hz;   // radiation high-pass
	real tone_hz;  // bell / directivity low-pass
	real tone_q;   //
	real low_comp; // level keytracking below rad_hz (exponent)
	// air
	real breath;  // jet turbulence (riding on the flow pulses)
	real t_noise; // tongue "t"
	// attack
	real tongue;   // strike sensitivity
	real acc_amp;  // the pressure built up behind the tongue: overshoot
	real acc_t;    // its decay (s)
	real tongue_t; // tongue release time of a soft "d" (s); a hard strike is ~16x faster
	real tongue_lip; // a tongued start: the tongue held the lips closed by this much (mm at a full strike)
	real slur;     // articulation on a legato note change
	real pitch_t;  // the player's ear: pitch correction time constant (s)
	// modulation
	real flutter, flut_hz; // shake -> flutter tongue
};
static constexpr int NPARAMS = sizeof(Params) / sizeof(real);
static_assert(sizeof(Params) == NPARAMS * sizeof(real), "Params must hold only floats");

[[nodiscard]] static inline Params trumpet_params() {
	Params p;
	p.gain = 19.3075f;
	p.pc_a = 0.351743f;
	p.pc_b = 1.19262f;
	p.lv_hz = 16.0102f;
	p.rel = 0.050774f;
	p.rel_ped = 0.4f;
	p.p_soft = 1.05585f;
	p.p_loud = 14.9845f;
	p.p_reg = -0.130238f;
	p.p_curve = 2.41112f;
	p.lip_tune = -265.508f;
	p.lip_treg = 84.7883f;
	p.lip_q = 9.98719f;
	p.lip_mass = 5.04216f;
	p.mass_reg = 1.24726f;
	p.lip_open = 0.020385f;
	p.open_reg = 1.02704f;
	p.lip_width = 12.906f;
	p.contact = 1.01252f;
	p.lip_nl = 4.18258f;
	p.embouch = 0.93927f;
	p.lip_tilt = 0.061837f;
	p.jitter = 1.37483f;
	p.cup = 1.48285f;
	p.pop_hz = 1427.48f;
	p.pop_q = 0.976347f;
	p.bore_t = 0.044498f;
	p.bore_reg = 0.784376f;
	p.bell_hz = 1821.65f;
	p.brass = 448.664f;
	p.rad_hz = 109.229f;
	p.tone_hz = 8298.62f;
	p.tone_q = 0.542062f;
	p.low_comp = 1.05614f;
	p.breath = 0.108717f;
	p.t_noise = 0.211963f;
	p.tongue = 1.39721f;
	p.acc_amp = 1.25615f;
	p.acc_t = 0.031235f;
	p.tongue_t = 0.001128f;
	p.tongue_lip = 0.053056f;
	p.slur = 0.148917f;
	p.pitch_t = 0.00626f;
	p.flutter = 0.6f;
	p.flut_hz = 24.0f;
	return p;
}

static constexpr int NUM_PRESETS = 1;

[[nodiscard]] static inline const char* preset_name(int) { return "trumpet"; }

[[nodiscard]] static inline Params preset_params(int) { return trumpet_params(); }

#ifdef TRUMPET_PRESET
static constexpr int DEFAULT_PRESET = TRUMPET_PRESET;
#else
static constexpr int DEFAULT_PRESET = 0;
#endif

struct VoiceState {
	// lips (opening x, velocity v), cup pressure, backbore flow, the previous jet flow and incoming wave
	real x, v, pc, ub, uM, uprev, pinprev;
	real um; // the jet's mean flow (turbulence rides on the flow pulses)
	real tlev; // the strike the tongue has held the lips for
	int age;   // blocks since the note started from silence
	// bore
	real lps, hps; // bell reflection lowpass, DC high-pass states
	real ltd;      // the transmitted wave's slow part (the bell reflects the mean flow)
	real d1;       // stage 1 steepened read delay
	real D;        // loop delay (samples)
	real ds;       // stage 2 read delay
	real lpt;      // bell lowpass of the transmitted wave
	uint32_t w, ws;
	float sb[SLEN];
	// body
	real rs, t1, t2;
	// player
	real lv1, lv2;    // breath smoother
	real amp;         // breath level after the release logic
	real gate;        // tongue / breath gate
	real gate_rate;   // its opening rate (per control step)
	real acc, tn;     // accent and tongue noise envelopes
	real accf;        // the accent's pressure, rising with the tongue's release (after the breath smoother)
	real ty1, ty2;    // tilt smoother
	real pm_prev;     // mouth pressure at the end of the last block
	real pthr;        // threshold estimate (Pa)
	real lre, lim;    // small-signal loop gain at the note (analysis)
	real jn, dr;      // lip jitter, slow wander
	real flut_ph;     //
	real note_prev;   //
	real vneg;        // release speed latch
	uint32_t rng, nrng;
	real level; // smoothed peak (activity)
	// the player's ear: the lips' period from their zero crossings, the bore trimmed onto the note
	real xlp, xhp, xprev, tlast, pprev, trim, xamp;
	int active;
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	Params p;  // as set (preset, live edits)
	Params ps; // smoothed copy used by the DSP
	FastMemory* fast;   // the host's (init): bore delay lines of voices 0 .. NFAST - 1
	LargeMemory* large; // the host's (init): the others'
	real sr, inv_sr, pi_sr, dt, ctrl_dt, block_dt;
	strisy::SvfCoef lv_c, ty_c;
	real shake_pk, shake;
	real dc, dc_c;
	VoiceState vs[MAX_VOICES];
	real vout[BLOCK];
	real mix[BLOCK];
	real pmc[NCTRL]; // mouth pressure at the control points
	real tnc[NCTRL]; // tongue noise at the control points
	// what every voice's block uses from the parameters alone (block_coefs, once per block)
	struct Coefs {
		real q, lw, Cc, Mb, iRp, alpha, beta, a1, one_m, Gd, gd, Gr, fr, br, fac_acc, fac_tn, pcv, shake, flut_inc;
		strisy::SvfCoef tcf;
	} k;
};

// ---------------------------------------------------------------------------------------------
// helpers (no libm)

[[nodiscard]] static inline real exp2s(real p) {
	p = rmin(rmax(p, -60.0f), 60.0f);
	const int wi = (int)(p + 64.0f) - 64; // floor
	const real z = p - (real)wi;
	union {
		uint32_t i;
		float f;
	} v = {static_cast<uint32_t>((1 << 23) * (p + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z))};
	return v.f;
}

// log2 for x > 0 (error ~1e-4)
[[nodiscard]] static inline real log2s(real x) {
	union {
		float f;
		uint32_t i;
	} u = {x};
	const real ex = (real)(int)((u.i >> 23) & 255) - 127.0f;
	u.i = (u.i & 0x007FFFFFu) | 0x3F800000u; // mantissa in [1, 2)
	const real m = u.f;
	return ex + 1.44269504f * (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

[[nodiscard]] static inline real note_freq(real note) { return 8.1757989f * exp2s(note * (1.0f / 12)); }

[[nodiscard]] static inline real clampr(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }

// tan(x) for 0 <= x < ~1.4 ([7/6] Pade)
[[nodiscard]] static inline real tanp(real x) {
	const real x2 = x * x;
	return x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2))) / (135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2)));
}

// atan(x), any sign (max error ~1e-5)
[[nodiscard]] static inline real atans(real x) {
	const real ax = fabsf(x);
	const bool inv = ax > 1.0f;
	const real t = inv ? 1.0f / ax : ax;
	const real t2 = t * t;
	const real a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	const real r = inv ? 1.5707963f - a : a;
	return x < 0.0f ? -r : r;
}

[[nodiscard]] static inline float fsqrt(float x) {
#if defined(__arm__) && defined(__ARM_FP)
	float r;
	__asm__("vsqrt.f32 %0, %1"
	        : "=t"(r)
	        : "t"(x));
	return r;
#else
	return __builtin_sqrtf(x);
#endif
}

[[nodiscard]] static inline real psin(real ph) { // sine of a phase in [0, 1)
	const real q = ph - 0.5f;
	return -8.0f * q * (1.0f - 2.0f * fabsf(q));
}

[[nodiscard]] static inline real sat(real x) {
	x = clampr(x, -1.5f, 1.5f);
	return x * (1.0f - 0.148148148f * x * x);
}

[[nodiscard]] static inline real block_noise(uint32_t& r) {
	r = r * 1664525u + 1013904223u;
	return (real)(int32_t)r * 4.656613e-10f;
}

struct Cx {
	real re, im;
};
[[nodiscard]] static inline Cx cmul(Cx a, Cx b) { return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
[[nodiscard]] static inline Cx cinv(Cx a) {
	const real d = 1.0f / rmax(a.re * a.re + a.im * a.im, 1e-30f);
	return {a.re * d, -a.im * d};
}

// a voice's bore delay line h (0: towards the bell, 1: back)
[[nodiscard]] static inline float* bore_line(Engine& e, int v, int h) {
	return v < NFAST ? e.fast->bore[v][h] : e.large->bore[v - NFAST][h];
}

// ---------------------------------------------------------------------------------------------
// setup

static inline void reset_voice(Engine& e, VoiceState& s, int v) {
	s.x = s.v = s.pc = s.ub = s.uM = s.uprev = s.pinprev = s.um = 0.0f;
	s.lps = s.hps = s.lpt = s.ltd = 0.0f;
	s.d1 = 0.0f;
	s.D = 100.0f;
	s.ds = (real)SD;
	s.w = s.ws = 0;
	for (int i = 0; i < SLEN; i++) s.sb[i] = 0.0f;
	s.rs = s.t1 = s.t2 = 0.0f;
	s.lv1 = s.lv2 = s.amp = s.gate = 0.0f;
	s.gate_rate = 0.0f;
	s.acc = s.tn = s.accf = 0.0f;
	s.ty1 = s.ty2 = 0.0f;
	s.pm_prev = 0.0f;
	s.pthr = 3000.0f;
	s.jn = s.dr = 0.0f;
	s.vneg = 0.0f;
	s.level = 0.0f;
	s.xlp = s.xhp = s.xprev = 0.0f;
	s.tlast = -1e4f;
	s.pprev = 0.0f;
	s.trim = s.xamp = 0.0f;
	if (v >= 0 && v < MAX_VOICES)
		for (int h = 0; h < 2; h++)
			for (int i = 0; i < BLEN; i++) bore_line(e, v, h)[i] = 0.0f;
}

static inline void update_derived(Engine& e) {
	e.lv_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * CTRL * rmin(e.p.lv_hz, 400.0f)), 0.5f);
}

static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	e.preset = i;
	e.p = preset_params(i);
	e.ps = e.p;
	update_derived(e);
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
	const real sr = (real)sample_rate;
	e.sr = sr;
	e.inv_sr = 1.0f / sr;
	e.pi_sr = PI / sr;
	e.dt = 1.0f / sr;
	e.ctrl_dt = (real)CTRL / sr;
	e.block_dt = (real)BLOCK / sr;
	e.ty_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * BLOCK * 12.0f), 0.5f);
	e.shake_pk = e.shake = 0.0f;
	e.dc = 0.0f;
	e.dc_c = 2.0f * PI * 12.0f / sr;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int v = 0; v < MAX_VOICES; v++) {
		VoiceState& s = e.vs[v];
		s = VoiceState();
		reset_voice(e, s, v);
		s.rng = 0x9E3779B9u * (uint32_t)(v + 1) + 12345u;
		s.nrng = 0x85EBCA6Bu * (uint32_t)(v + 3) + 999u;
		s.flut_ph = 0.31f * (real)v;
		s.flut_ph -= (real)(int)s.flut_ph;
		s.note_prev = 69.0f;
		s.active = 0;
	}
	for (int v = 0; v < e.nvoices; v++) e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	set_preset(e, DEFAULT_PRESET);
}

[[nodiscard]] static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

#ifdef TRUMPET_TEST_HOOK
static int trumpet_guard_resets = 0;
// host analysis: a flow impulse into the cup with the lips shut (the input impedance), the cup
// pressure and the lip opening of voice 0 recorded per sample
static int trumpet_dbg_imp = 0;
static long trumpet_dbg_n = 0, trumpet_dbg_len = 0;
static float* trumpet_dbg_pc = nullptr;
static float* trumpet_dbg_x = nullptr;
static float trumpet_dbg_fdc = 0.0f, trumpet_dbg_famp = 0.0f, trumpet_dbg_ff = 0.0f; // forced cup pressure (admittance test)
static float* trumpet_dbg_u = nullptr;
static int trumpet_dbg_newton = 0;
#endif

// ---------------------------------------------------------------------------------------------
// one voice, one block

static inline void block_coefs(Engine& e) {
	const Params& P = e.ps;
	Engine::Coefs& k = e.k;
	const real dt = e.dt;
	k.q = clampr(P.lip_q, 0.5f, 100.0f);
	k.lw = clampr(P.lip_width * 0.001f, 0.002f, 0.02f);
	// mouthpiece: cup compliance C, backbore inertance M from the popping frequency, and its loss as a
	// resistance in parallel with M (no loss for the mean flow) from the resonance's Q into the bore:
	// series equivalent at the resonance zc^2 / Rp, so Q = zc / (Z0 + zc^2 / Rp)
	const real Cc = clampr(P.cup, 0.1f, 3.0f) * 1e-6f / (RHO * C0 * C0);
	const real wp = 2.0f * PI * clampr(P.pop_hz, 200.0f, 4000.0f);
	const real Mb = 1.0f / (wp * wp * Cc);
	const real zc = fsqrt(Mb / Cc);
	const real rs_need = zc / clampr(P.pop_q, 0.3f, 50.0f) - Z0;
	const real iRp = rs_need > 0.0f ? rs_need / (zc * zc) : 0.0f; // 1 / Rp
	const real alpha = 2.0f * Cc / dt, beta = 2.0f * Mb / dt;
	// backbore (trapezoidal): ub' = a1 pc' + b1, see the audio loop
	const real gam = 1.0f / beta + iRp;
	const real kap = 1.0f / (1.0f + Z0 * gam);
	k.Cc = Cc;
	k.Mb = Mb;
	k.iRp = iRp;
	k.alpha = alpha;
	k.beta = beta;
	k.a1 = gam * kap;
	k.one_m = 1.0f - gam * kap * Z0;
	// the bell's DC high-pass: the open end, no reflection of the mean flow (far below the note: the
	// harmonics stay harmonic)
	const real fdc = DC_HZ;
	const real gd = tanp(e.pi_sr * fdc);
	k.gd = gd;
	k.Gd = gd / (1.0f + gd);
	// radiation filters
	const real fr = clampr(P.rad_hz, 10.0f, 4000.0f);
	const real grr = tanp(e.pi_sr * fr);
	k.fr = fr;
	k.Gr = grr / (1.0f + grr);
	k.tcf = strisy::svf_pade(e.pi_sr * clampr(P.tone_hz, 200.0f, 16000.0f), clampr(P.tone_q, 0.5f, 4.0f));
	k.br = rmax(P.breath, 0.0f);
	k.fac_acc = 1.0f - e.ctrl_dt / rmax(P.acc_t, 0.005f);
	k.fac_tn = 1.0f - e.ctrl_dt / 0.006f;
	k.pcv = clampr(P.p_curve, 0.3f, 3.0f);
	k.shake = e.shake * rmax(P.flutter, 0.0f);
	k.flut_inc = P.flut_hz * e.ctrl_dt;
}

// (out of line: inlined into process, the board build carried two copies of it, twice the code for
// the I-cache)
__attribute__((noinline)) static void process_voice(Engine& e, int vi) {
	VoiceState& s = e.vs[vi];
	const Params& P = e.ps;
	const strisy::VoiceControls cc = e.vc[vi];
	const real pres = clampr(cc.pres, 0.0f, 1.5f);
	// after a release the firmware leaves the last vpres in place: a positive leftover must not
	// strike or hold a released voice; negative values still set the release speed
	const real vpres = clampr(cc.vpres, -1.0f, pres > 0.0f ? 1.0f : 0.0f);
	const real vp1 = rmax(vpres - 0.02f, 0.0f);
	const real vplev = rmin(rmax(P.tongue, 0.0f) * vp1 / (0.4f + vp1), 1.0f);
	real note = clampr(cc.note, 0.0f, 132.0f);
	while (note > NOTE_HI) note -= 12.0f;
	const bool held = pres > 0.0f;
	bool start = false;
	if (!s.active) {
		if (!held && vplev <= 0.0f) {
			s.note_prev = note;
			return;
		}
		s.active = 1;
		start = true;
	}

	// ---- block rate: the player and the horn for this note -------------------------------------
	const bool jump = start || fabsf(note - s.note_prev) >= 1.0f;
	if (jump && !start && held) s.acc = rmax(s.acc, P.slur); // legato change: a small articulation
	if (jump) s.vneg = 0.0f;
	s.note_prev = note;
	if (start) {
		// from silence: the lips at rest, the tongue against them
		s.x = s.v = s.pc = s.ub = s.uM = s.uprev = s.pinprev = s.um = 0.0f;
		s.gate = 0.0f;
		s.lv1 = 0.0f;
		s.lv2 = 0.0f;
		s.amp = 0.0f;
		s.pm_prev = 0.0f;
	}
	// a strike opens the tongue: fast for a hard one
	if (vplev > 0.05f && (start || vplev > s.acc + 0.05f)) s.gate_rate = rmin(e.ctrl_dt / rmax(P.tongue_t * exp2s(-4.0f * vplev), 0.001f), 1.0f);
	if (start && vplev <= 0.05f) s.gate_rate = rmin(e.ctrl_dt / rmax(P.tongue_t, 0.0005f), 1.0f);

	const real by = clampr(cc.but_y, -1.0f, 1.0f);
	if (start) {
		s.ty1 = 0.0f;
		s.ty2 = by;
	}
	real tilt;
	{
		real ty1 = s.ty1, ty2 = s.ty2, t1;
		SVF_TICK(by, ty1, ty2, e.ty_c, t1, tilt);
		s.ty1 = ty1;
		s.ty2 = ty2;
	}
	tilt = clampr(tilt, -1.0f, 1.0f);
	// lip life: tension jitter (~35 Hz noise) and a slow wander
	s.jn += 0.15f * (6.0f * block_noise(s.rng) - s.jn);
	s.jn = clampr(s.jn, -4.0f, 4.0f);
	s.dr += 0.003f * (45.0f * block_noise(s.rng) - s.dr);
	s.dr = clampr(s.dr, -4.0f, 4.0f);

	const real f0 = rmin(note_freq(note), 0.45f * e.sr);
	const real P0 = e.sr / f0;
	const real oct = (note - 70.0f) * (1.0f / 12); // octaves re Bb4
	const real w0 = 2.0f * PI * f0;
	// lips
	const real lip_c = P.lip_tune + P.lip_treg * oct + rmax(P.jitter, 0.0f) * (s.jn + 0.5f * s.dr);
	const real wl = w0 * exp2s(lip_c * (1.0f / 1200));
	const real w2 = wl * wl;
	const real q = e.k.q;
	const real mu = clampr(P.lip_mass * exp2s(-P.mass_reg * oct), 0.05f, 50.0f);
	const real imu = 1.0f / mu;
	const real x0 = clampr(P.lip_open * 0.001f * exp2s(-P.open_reg * oct - P.lip_tilt * tilt), 0.0f, 0.003f);
	const real lw = e.k.lw;
	const real dt = e.dt;
	// lip damping per sample: linear (Q) and growing with the speed (tissue losses), contact when closed
	const real c1 = dt * wl / q;
	const real c1nl = c1 * clampr(P.lip_nl, 0.0f, 200.0f);
	const real ivref = 1.0f / (wl * rmax(x0, 5e-5f));
	const real ke = clampr(P.embouch, 0.0f, 1.5f) * imu / w2; // rest position per Pa above the threshold
	const real kc = rmax(P.contact, 0.0f) * w2;
	const real ccon = dt * 2.0f * fsqrt(kc);
	// mouthpiece (block_coefs)
	const real Cc = e.k.Cc, Mb = e.k.Mb, iRp = e.k.iRp, alpha = e.k.alpha, beta = e.k.beta;
	const real a1 = e.k.a1, one_m = e.k.one_m; // backbore (trapezoidal): ub' = a1 pc' + b1, see the audio loop
	// bore: bell reflection lowpass, DC high-pass, round-trip loss
	const real fb = clampr(P.bell_hz * exp2s(0.3f * P.lip_tilt * tilt), 50.0f, 8000.0f);
	const real gb = tanp(e.pi_sr * fb);
	const real Gb = gb / (1.0f + gb);
	const real gd = e.k.gd, Gd = e.k.Gd; // the open end (DC_HZ)
	const real t0 = tanp(rmin(PI / P0, 1.3f));
	// the mouthpiece's reflection phase at f0 (lips closed): R = (Zm - Z0) / (Zm + Z0), Zm = (Rp || iwM)
	// + 1 / (iwC) = Rs + iX
	const real ww = 2.0f * e.sr * t0; // warped (trapezoidal) frequency
	const real wm = ww * Mb;
	const real den_p = 1.0f + wm * wm * iRp * iRp; // (Rp^2 + w^2 M^2) / Rp^2
	const real Rs = wm * wm * iRp / den_p;
	const real Xm = wm / den_p;
	const real X = Xm - 1.0f / (ww * Cc);
	real mp_ph;
	if (Rs < Z0) mp_ph = PI + atans(X / (Rs - Z0)) - atans(X / (Rs + Z0)) - 2.0f * PI;
	else mp_ph = atans(X / (Rs - Z0 + 1e-3f)) - atans(X / (Rs + Z0));
	real g = exp2s(-1.442695f * P0 / (clampr(P.bore_t * exp2s(-P.bore_reg * oct), 0.002f, 0.5f) * e.sr));
	g = rmin(g, 0.95f) * clampr((2040.0f - P0) * 0.01f, 0.0f, 1.0f);
	// the bell's reflection at the note, R = LP (g - (1 + g) LPdc) (prewarped one-poles, exact)
	const Cx rlp = cinv({1.0f, t0 / gb});
	const Cx rdc = cinv({1.0f, t0 / gd});
	const Cx rbell = cmul(rlp, {g - (g + 1.0f) * rdc.re, -(g + 1.0f) * rdc.im});
	// The loop delay. First guess: the bore loop's resonance on the note (the bell's and the
	// mouthpiece's phases). Then what the lips play on: the small-signal loop gain L = Y Z of the lips'
	// admittance Y and the load Z they see (the cup in parallel with backbore + bore, the bore's input
	// impedance from its round-trip reflection R e^{-i w D}) must be real at the note: a Newton step on
	// D per block (three at a new note). The same L gives the oscillation threshold.
	const real phase0 = atans(rbell.im / rmax(rbell.re, 1e-6f)) + mp_ph;
	const real Dpole = clampr(P0 * (1.0f + phase0 * (0.5f / PI)), 2.0f, 2.0f * DMAX);
	const real wdig = 2.0f * PI / P0;
	const Cx hl = cinv({w2 - w0 * w0, w0 * wl / q}); // lip displacement per force
	const real dpe = rmax(s.pthr, 100.0f); // at the threshold (the ear trims the pull above it)
	const real sv = fsqrt(2.0f * dpe / RHO);
	const real hbar = rmax(x0 + dpe * imu / w2, 2e-6f);
	const Cx ylip = {-lw * sv * imu * hl.re - lw * hbar / (RHO * sv), -lw * sv * imu * hl.im};
	auto load = [&](real D) -> Cx {
		const real th = -wdig * D;
		real tw = th * (0.5f / PI);
		tw -= (real)(int)tw;
		if (tw > 0.5f) tw -= 1.0f;
		if (tw < -0.5f) tw += 1.0f;
		// cos(2 pi tw), sin(2 pi tw): parabolic sine refined (error ~1e-3)
		const real sn0 = psin(tw < 0.0f ? tw + 1.0f : tw);
		const real sn = sn0 * (0.775f + 0.225f * fabsf(sn0));
		real tc = tw + 0.25f;
		if (tc >= 1.0f) tc -= 1.0f;
		if (tc < 0.0f) tc += 1.0f;
		const real cn0 = psin(tc);
		const real cn = cn0 * (0.775f + 0.225f * fabsf(cn0));
		const Cx gam = cmul(rbell, {cn, sn});
		const Cx zb = cmul({Z0 * (1.0f + gam.re), Z0 * gam.im}, cinv({1.0f - gam.re, -gam.im}));
		const Cx yb = cinv({Rs + zb.re, Xm + zb.im});
		return cinv({yb.re, yb.im + ww * Cc});
	};
	auto argL = [&](real D) -> real {
		const Cx L = cmul(ylip, load(D));
		return L.re > 0.0f ? atans(L.im / L.re) : (L.im >= 0.0f ? 1.5f : -1.5f);
	};
	if (jump) {
		s.D = Dpole;
		s.trim = 0.0f;
		s.tlast = -1e4f;
	}
	{
		const int iters = jump ? 3 : 1; // (then one per block: converged within a few ms)
		const real lo = Dpole - 0.15f * P0, hi = Dpole + 0.15f * P0;
		real D = clampr(s.D, lo, hi);
		for (int it = 0; it < iters; it++) {
			const real ph = argL(D), ph2 = argL(D + 0.25f);
			const real der = (ph2 - ph) * 4.0f;
#ifdef TRUMPET_TEST_HOOK
			if (trumpet_dbg_newton && vi == 0) fprintf(stderr, "newton it %d D %.3f ph %.4f ph2 %.4f der %.4f%c", it, (double)D, (double)ph, (double)ph2, (double)der, 10);
#endif
			if (der < -1e-4f) D = clampr(D - clampr(ph / der, -0.02f * P0, 0.02f * P0), lo, hi);
		}
		s.D = D;
	}
	const real Dtot = clampr(s.D * exp2s(s.trim * (1.0f / 1200)), 2.0f, 2.0f * DMAX);
	const real d1t = clampr(0.5f * Dtot, 1.0f, DMAX);
	const real d2t = clampr(Dtot - d1t, 1.0f, DMAX);
	if (start) s.d1 = d1t;
	// the threshold: Re(Y Z) = 1 solved for the mouth pressure (s = sqrt(2 dp / rho): A s^2 - s - B = 0;
	// the lip opening at the previous estimate)
	{
		const Cx zl = load(s.D); // at the feedforward delay (the trim follows the louder lips' pull)
		{
			const Cx L = cmul(ylip, zl);
			s.lre = L.re;
			s.lim = L.im;
		}
		const Cx mh = {-hl.re, -hl.im};
		const real A = lw * imu * cmul(mh, zl).re; // the jet modulated by the lip motion
		const real hb = rmax(x0 + s.pthr * imu / w2, 2e-6f);
		const real B = lw * hb / RHO * zl.re; // the jet's own (positive) resistance
		real pt = 1e5f;
		if (A > 1e-12f) {
			const real sq = (1.0f + fsqrt(1.0f + 4.0f * A * B)) / (2.0f * A);
			pt = 0.5f * RHO * sq * sq;
		}
		s.pthr = clampr(pt, 100.0f, 60000.0f);
	}
	// steepening: shift per Pa over both stages (low notes a little more: larger swings, longer bore)
	const real kkey = clampr(fsqrt(P0 * (1.0f / 120)), 0.6f, 3.0f);
	const real kpa = rmax(P.brass, 0.0f) * kkey * 1e-4f;
	const real ks = 0.5f * kpa, ks2 = 0.5f * kpa;
	const real smax1 = 0.8f * d1t, smax2 = (real)(SD - 8);
	const real rl = clampr(fsqrt(P0 * (1.0f / RLDIV)) - 1.0f, 0.15f, RL);

	// radiation filters
	const real fr = e.k.fr, Gr = e.k.Gr;
	const strisy::SvfCoef tcf = e.k.tcf;
	// (1 + (rad_hz / f0)^2)^(low_comp / 2): low notes keep their weight through the radiation high-pass
	const real rq = fr / f0;
	const real lcomp = rmin(exp2s(0.5f * rmax(P.low_comp, 0.0f) * log2s(1.0f + rq * rq)), 4.0f);
	const real gain = clampr(P.gain, 0.0f, 20.0f) * 1e-4f * lcomp;

	// ---- control rate: breath, tongue, accent ----------------------------------------------------
	if (vpres > 0.1f) s.vneg = 0.0f;
	if (held && vpres < 0.0f) s.vneg = rmax(s.vneg, -vpres);
	const real ped = clampr(e.g.pedal, 0.0f, 1.0f);
	const real rel_tc = rmax(P.rel + P.rel_ped * ped * ped, 0.004f) / (1.0f + 3.0f * s.vneg);
	const real fac = 1.0f - e.ctrl_dt / (held ? 0.03f : rel_tc);
	const real fac_acc = e.k.fac_acc;
	const real fac_tn = e.k.fac_tn;
	const real fac_gate = rmin(e.ctrl_dt / rel_tc, 1.0f);
	const real pcl = pres * (P.pc_a + P.pc_b * pres);
	const real p_lo = s.pthr * clampr(P.p_soft, 0.3f, 4.0f);
	const real p_hi = s.pthr * rmax(clampr(P.p_loud, 1.0f, 20.0f) * exp2s(P.p_reg * oct), P.p_soft + 0.05f);
	const real pcv = e.k.pcv;
	const real shake = e.k.shake, flut_inc = e.k.flut_inc;
	{
		real lv1 = s.lv1, lv2 = s.lv2, amp = s.amp, acc = s.acc, accf = s.accf, tn = s.tn, gate = s.gate, fph = s.flut_ph;
		// the lungs and the mouth follow the key slowly (the breath smoother); the tongue's accent is fast
		const real fac_af = rmin(e.ctrl_dt / rmax(rmax(P.tongue_t, 0.0005f) * exp2s(-4.0f * acc), 0.001f), 1.0f);
		const strisy::SvfCoef lc = e.lv_c;
		for (int k = 0; k < NCTRL; k++) {
			acc = rmax(acc * fac_acc, vplev);
			tn = rmax(tn * fac_tn, vplev);
			real t1, ls;
			SVF_TICK(rmin(pcl, 2.0f), lv1, lv2, lc, t1, ls);
			amp = rmax(rmax(ls, 0.0f), amp * fac);
			accf += fac_af * (P.acc_amp * acc - accf);
			if (held) gate += s.gate_rate * (1.0f - gate);
			else gate -= fac_gate * gate;
			fph += flut_inc;
			if (fph >= 1.0f) fph -= 1.0f;
			const real am = 1.0f - shake * (0.5f + 0.5f * psin(fph));
			// breath -> mouth pressure (amp^p_curve: exp2s(p_curve log2 amp) without libm)
			const real a = rmin(amp + accf, 1.5f);
			const real ap = (pcv != 1.0f && a > 1e-6f) ? exp2s(pcv * log2s(a)) : a;
			e.pmc[k] = gate * am * (p_lo + (p_hi - p_lo) * ap);
			e.tnc[k] = P.t_noise * tn * tn;
		}
		s.lv1 = lv1;
		s.lv2 = lv2;
		s.amp = amp;
		s.acc = acc;
		s.accf = accf;
		s.tn = tn;
		s.gate = gate;
		s.flut_ph = fph;
	}

	// ---- audio rate: lips, mouthpiece, bore, steepening, bell ------------------------------------
	float* __restrict__ B1 = bore_line(e, vi, 0);
	float* __restrict__ B2 = bore_line(e, vi, 1);
	// a tongued start: while the strike rises (the first ~16 ms of a note from silence) the tongue
	// holds the lips closed, more for a harder strike; at its peak it lets them go
	if (start) {
		s.age = 0;
		s.tlev = 0.0f;
	} else if (s.age < 1000) {
		s.age++;
	}
	if (s.age < 24 && vplev > s.tlev + 0.02f) {
		s.tlev = vplev;
		s.x = rmin(s.x, x0 - clampr(P.tongue_lip, 0.0f, 3.0f) * 0.001f * vplev);
		s.v = rmin(s.v, 0.0f);
	}
	real x = s.x, v = s.v, pc = s.pc, ub = s.ub, uM = s.uM, uprev = s.uprev, pinp = s.pinprev, um = s.um;
	real lps = s.lps, hps = s.hps, lpt = s.lpt, ltd = s.ltd, d1 = s.d1, ds = s.ds;
	uint32_t w = s.w, ws = s.ws, nr = s.nrng;
	float* __restrict__ SB = s.sb;
	real* __restrict__ out = e.vout;
	const uint32_t i2 = (uint32_t)(int)d2t, in = (uint32_t)(int)d1t;
	const real f2 = d2t - (real)(int)i2, fn = d1t - (real)(int)in;
	const real br = e.k.br;
	const real two_rho = 2.0f / RHO;
	real pm = s.pm_prev;
	const real pthr_b = s.pthr;
	real peak = 0.0f;
	// pitch tracker: lip opening low-passed (~1.3 f0), DC removed, upward zero crossings
	const real a_lp = clampr(2.0f * PI * 1.3f * f0 * dt, 0.001f, 0.9f);
	real xlp = s.xlp, xhp = s.xhp, xprev = s.xprev, tlast = s.tlast - (real)BLOCK, pprev = s.pprev, xamp = 0.0f;
	int ncross = 0;
	real per_sum = 0.0f;
	// Three passes: the lips and the mouthpiece (they write the wave into the bore), the bell (its
	// reflection back into the bore, and what radiates: the steepening and the transmission), and the
	// pitch tracker on the lips' opening. The mouthpiece hears the bell after d2t samples, so the first
	// two run in chunks of up to that many samples; sample by sample (the order of a single loop) when a
	// read of the bell's could reach back a whole line to where the chunk writes (the lowest notes)
	real dpm = 0.0f, tnl = 0.0f;
	real xs[BLOCK];
	const bool reach = (real)in + 2.0f < (real)(BLEN - BLOCK) && d1 + (real)BLOCK * RU + 2.0f < (real)(BLEN - BLOCK);
	const int chunk = reach ? (i2 < (uint32_t)BLOCK ? (int)i2 : BLOCK) : 1;
	const uint32_t wst = w;
	for (int n0 = 0; n0 < BLOCK; n0 += chunk) {
		const int n1 = n0 + chunk < BLOCK ? n0 + chunk : BLOCK;
		for (int n = n0; n < n1; n++) {
			if ((n & (CTRL - 1)) == 0) {
				dpm = (e.pmc[(unsigned)n / CTRL] - pm) * (1.0f / CTRL);
				tnl = e.tnc[(unsigned)n / CTRL] * 2e-4f;
			}
			pm += dpm;
			// the wave returning from the bell, at the mouthpiece
			const real r0 = B2[(w - i2) & BMASK], r1 = B2[(w - i2 - 1u) & BMASK];
			const real pin = r0 + f2 * (r1 - r0);
			// the jet through the lips (Bernoulli, regularised at small dp) and its turbulence
			const real dp = pm - pc;
			const real sj = fsqrt(two_rho * fabsf(dp) + 4.0f);
			const real isj = 1.0f / sj;
#ifdef TRUMPET_TEST_HOOK
			const real wh = (trumpet_dbg_imp && vi == 0) ? 0.0f : lw * rmax(x, 0.0f);
#else
			const real wh = lw * rmax(x, 0.0f);
#endif
			const real uj = wh * two_rho * dp * isj;
			nr = nr * 1664525u + 1013904223u;
			const real nz = (real)(int32_t)nr * 4.656613e-10f;
#ifdef TRUMPET_TEST_HOOK
			um += 0.0005f * (uj - um);
			real u0 = uj + br * (uj - um) * nz + tnl * nz;
			if (trumpet_dbg_imp && vi == 0) u0 = (trumpet_dbg_n == 0) ? 1e-3f : 0.0f;
#else
			um += 0.0005f * (uj - um);
			const real u0 = uj + br * (uj - um) * nz + tnl * nz;
#endif
			const real G = wh * isj * (1.0f / RHO); // the jet's conductance (implicit in p_c)
			// cup + backbore (trapezoidal). Cup: alpha (pc' - pc) = u' + u - ub' - ub. Backbore: the
			// drop D = pc - pb (pb = 2 pin + Z0 ub) drives the inertance, beta (uM' - uM) = D' + D, and
			// the loss Rp in parallel: ub = uM + D / Rp. Solved: ub' = a1 pc' + bb1
			const real Dn = pc - 2.0f * pinp - Z0 * ub;
			const real bb1 = (uM + Dn / beta) * one_m - 2.0f * a1 * pin;
#ifdef TRUMPET_TEST_HOOK
			real pcn = ((alpha + G) * pc + u0 + uprev - ub - bb1) / (alpha + G + a1);
			if (trumpet_dbg_famp > 0.0f && vi == 0) pcn = trumpet_dbg_fdc + trumpet_dbg_famp * __builtin_sinf(6.2831853f * trumpet_dbg_ff * (real)trumpet_dbg_n / e.sr);
			if (trumpet_dbg_u && vi == 0 && trumpet_dbg_n < trumpet_dbg_len) trumpet_dbg_u[trumpet_dbg_n] = u0;
#else
			const real pcn = ((alpha + G) * pc + u0 + uprev - ub - bb1) / (alpha + G + a1);
#endif
			const real ubn = a1 * pcn + bb1;
			const real Dp = pcn - 2.0f * pin - Z0 * ubn;
			uM = clampr(ubn - Dp * iRp, -0.05f, 0.05f);
			uprev = u0 - G * (pcn - pc);
			pc = clampr(pcn, -60000.0f, 60000.0f);
			ub = clampr(ubn, -0.05f, 0.05f);
			pinp = pin;
#ifdef TRUMPET_TEST_HOOK
			if (vi == 0 && trumpet_dbg_pc && trumpet_dbg_n < trumpet_dbg_len) {
				trumpet_dbg_pc[trumpet_dbg_n] = pc;
				if (trumpet_dbg_x) trumpet_dbg_x[trumpet_dbg_n] = x;
			}
			if (vi == 0) trumpet_dbg_n++;
#endif
			// the wave leaving into the bore
			const real pout = pin + Z0 * ub;
			B1[w] = pout;
			// lips: outward striking, contact when closed
			const real F = (pm - pc) * imu;
			{
				const real nu = v * ivref;
				const real cd = 1.0f + c1 + c1nl * rmin(nu * nu, 100.0f);
				const real xr = x0 - ke * rmax(pm - pthr_b, 0.0f);
				if (x >= 0.0f) v = (v + dt * (-w2 * (x - xr) + F)) / cd;
				else v = (v + dt * (-w2 * (x - xr) - kc * x + F)) / (cd + ccon);
			}
			x = clampr(x + dt * v, -0.003f, 0.006f);
			xs[n] = x;
			w = (w + 1u) & BMASK;
		}
		for (int n = n0; n < n1; n++) {
			const uint32_t wr = (wst + (uint32_t)n) & BMASK;
			// the outgoing wave at the bell: the linear wave reflects (lowpass, DC-free, x g) ...
			const real a0 = B1[(wr - in) & BMASK], a1 = B1[(wr - in - 1u) & BMASK];
			const real bl = a0 + fn * (a1 - a0);
			const real vb = (bl - lps) * Gb;
			const real lp = vb + lps;
			lps = lp + vb;
			const real vd = (lp - hps) * Gd;
			const real ld = vd + hps;
			hps = ld + vd;
			B2[wr] = g * lp - (g + 1.0f) * ld; // -1 at DC (the open end), g lp at the note
			// ... and the steepened wave radiates (stage 1 on the bore line, stage 2 on its output)
			real dn = d1t - clampr(ks * bl, -smax1, smax1);
			dn = clampr(dn, d1 - rl, d1 + RU);
			d1 = clampr(dn, 0.0f, DMAX);
			const int i1 = (int)d1;
			const real f1 = d1 - (real)i1;
			const real b0 = B1[(wr - (uint32_t)i1) & BMASK], b1 = B1[(wr - (uint32_t)i1 - 1u) & BMASK];
			const real b = b0 + f1 * (b1 - b0);
			SB[ws] = b;
			const real ynom = SB[(ws - (uint32_t)SD) & SMASK];
			real dsn = (real)SD - clampr(ks2 * ynom, -smax2, smax2);
			dsn = clampr(dsn, ds - rl, ds + RU);
			ds = clampr(dsn, 1.0f, (real)(SLEN - 3));
			const int is = (int)ds;
			const real fs = ds - (real)is;
			const real y0 = SB[(ws - (uint32_t)is) & SMASK], y1 = SB[(ws - (uint32_t)is - 1u) & SMASK];
			const real yst = y0 + fs * (y1 - y0);
			ws = (ws + 1u) & SMASK;
			// the bell transmits what it doesn't reflect
			const real vt = (yst - lpt) * Gb;
			const real lt = vt + lpt;
			lpt = lt + vt;
			// ... and its slow part reflects too (R = -1 at DC: the mean flow does not radiate)
			const real vdc = (yst - ltd) * Gd;
			const real ldc = vdc + ltd;
			ltd = ldc + vdc;
			out[n] = yst - g * lt - (1.0f - g) * ldc;
		}
	}
	for (int n = 0; n < BLOCK; n++) {
		const real x = xs[n];
		xlp += a_lp * (x - xlp);
		xhp += 0.004f * (xlp - xhp);
		{
			const real xc = xlp - xhp;
			xamp = rmax(xamp, fabsf(xc));
			if (xprev < 0.0f && xc >= 0.0f) {
				const real tc = (real)n - xc / (xc - xprev);
				if (tlast > -4000.0f) {
					const real per = tc - tlast;
					if (fabsf(per - pprev) < 0.05f * per) { // a clean, periodic regime
						per_sum += per;
						ncross++;
					}
					pprev = per;
				}
				tlast = tc;
			}
			xprev = xc;
		}
	}
	s.x = x;
	s.v = v;
	s.pc = pc;
	s.ub = ub;
	s.uM = uM;
	s.um = um;
	s.uprev = uprev;
	s.pinprev = pinp;
	s.lps = lps;
	s.hps = hps;
	s.lpt = lpt;
	s.ltd = ltd;
	s.d1 = d1;
	s.ds = ds;
	s.w = w;
	s.ws = ws;
	s.nrng = nr;
	s.pm_prev = pm;
	s.xlp = xlp;
	s.xhp = xhp;
	s.xprev = xprev;
	s.tlast = tlast;
	s.pprev = pprev;
	s.xamp = rmax(xamp, s.xamp + 0.1f * (xamp - s.xamp));
	// the player's ear: a slow trim of the bore onto the note (the feedforward places it at the
	// threshold; louder lips pull a little)
	if (ncross > 0 && held && s.xamp > 5e-6f) {
		const real err = 1200.0f * log2s(e.sr * (real)ncross / (rmax(per_sum, 1.0f) * f0));
		if (fabsf(err) < 200.0f) s.trim = clampr(s.trim + clampr(err * rmin(per_sum * dt / clampr(P.pitch_t, 0.002f, 1.0f), 0.5f), -6.0f, 6.0f), -150.0f, 150.0f);
	}

	// radiation high-pass, bell low-pass, gain
	{
		real rs = s.rs, t1 = s.t1, t2 = s.t2;
		for (int n = 0; n < BLOCK; n++) {
			const real o = out[n];
			const real vr = (o - rs) * Gr;
			const real lr = vr + rs;
			rs = lr + vr;
			const real hp = o - lr;
			real tb, ty;
			SVF_TICK(hp, t1, t2, tcf, tb, ty);
			const real y = ty * gain;
			out[n] = y;
			peak = rmax(peak, fabsf(y));
		}
		s.rs = rs;
		s.t1 = t1;
		s.t2 = t2;
	}

	// guard: non-finite or absurd state -> reset this voice and drop its block
	const real st = fabsf(s.pc) + fabsf(s.lps) + fabsf(s.hps) + fabsf(s.lpt) + fabsf(s.rs) + fabsf(s.t1) + fabsf(s.t2) + fabsf(s.x) * 1e6f + fabsf(s.v) + fabsf(s.amp) + fabsf(s.d1) + fabsf(s.ds);
	if (!(peak < 1e3f) || !(st < 1e7f)) {
#ifdef TRUMPET_TEST_HOOK
		trumpet_guard_resets++;
		fprintf(stderr, "guard: voice %d peak %g st %g note %g pres %g\n", vi, (double)peak, (double)st, (double)note, (double)pres);
#endif
		reset_voice(e, s, vi);
		s.active = 0;
		return;
	}
	real* __restrict__ mix = e.mix;
	for (int n = 0; n < BLOCK; n++) mix[n] += out[n];
	s.level += 0.05f * (peak - s.level);
	// idle once released and faded out; the state is kept
	if (!held && vplev <= 0.0f && s.gate < 1e-4f && s.amp < 1e-5f && s.acc < 1e-4f && peak < 1e-5f) s.active = 0;
}

static inline void process(Engine& e, real* __restrict__ output) {
	// smooth the parameters towards the set values (live edits glide in ~13 ms instead of stepping)
	{
		const real* __restrict__ pt = reinterpret_cast<const real*>(&e.p);
		real* __restrict__ ps = reinterpret_cast<real*>(&e.ps);
		for (int i = 0; i < NPARAMS; i++) {
			const real d = pt[i] - ps[i];
			// (the snap is relative: a large value's last steps would be below its float resolution)
			ps[i] = fabsf(d) <= 1e-5f * rmax(fabsf(pt[i]), 1.0f) ? pt[i] : ps[i] + 0.05f * d;
		}
	}
	{
		const real sh = clampr(e.g.acc_abs - 1.1f, 0.0f, 1.0f);
		e.shake_pk = rmax(sh, e.shake_pk * (1.0f - e.block_dt * 2.5f));
		e.shake += 0.03f * (e.shake_pk - e.shake);
	}
	block_coefs(e);
	for (int n = 0; n < BLOCK; n++) e.mix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);
	// master: DC blocker, then a gentle soft clip with a ceiling of 4.2
	real dc = e.dc;
	const real dcc = e.dc_c;
	for (int n = 0; n < BLOCK; n++) {
		const real x = e.mix[n];
		dc += dcc * (x - dc);
		output[n] = 4.2f * sat((x - dc) * (1.0f / 4.2f));
	}
	e.dc = (fabsf(dc) < 1e3f) ? dc : 0.0f;
}

// ---------------------------------------------------------------------------------------------
// live parameters

struct ParamEntry {
	strisy::ParamDesc d;
	real Params::*m;
};

static constexpr strisy::ParamKind C_ = strisy::ParamKind::continuous;

static constexpr ParamEntry param_table[] = {
    {{"gain", "level", 0.05f, 20.0f, C_}, &Params::gain},
    {{"pres_lin", "level", 0.0f, 1.0f, C_}, &Params::pc_a},
    {{"pres_sq", "level", 0.0f, 1.5f, C_}, &Params::pc_b},
    {{"pres_hz", "level", 2.0f, 200.0f, C_}, &Params::lv_hz},
    {{"release", "level", 0.005f, 1.0f, C_}, &Params::rel},
    {{"rel_pedal", "level", 0.0f, 3.0f, C_}, &Params::rel_ped},
    {{"p_soft", "breath", 0.5f, 3.0f, C_}, &Params::p_soft},  // lightest touch re the threshold
    {{"p_loud", "breath", 1.1f, 20.0f, C_}, &Params::p_loud}, // ff mouth pressure re the threshold
    {{"p_reg", "breath", -0.5f, 1.5f, C_}, &Params::p_reg},
    {{"p_curve", "breath", 0.3f, 3.0f, C_}, &Params::p_curve},
    {{"lip_tune", "lips", -600.0f, 300.0f, C_}, &Params::lip_tune}, // cents re the note
    {{"lip_treg", "lips", -300.0f, 300.0f, C_}, &Params::lip_treg},
    {{"lip_q", "lips", 0.5f, 60.0f, C_}, &Params::lip_q},
    {{"lip_mass", "lips", 0.2f, 30.0f, C_}, &Params::lip_mass}, // kg/m^2
    {{"mass_reg", "lips", -0.5f, 2.0f, C_}, &Params::mass_reg},
    {{"lip_open", "lips", 0.0f, 1.5f, C_}, &Params::lip_open}, // mm
    {{"open_reg", "lips", -1.0f, 2.0f, C_}, &Params::open_reg},
    {{"lip_width", "lips", 3.0f, 15.0f, C_}, &Params::lip_width},
    {{"contact", "lips", 0.0f, 20.0f, C_}, &Params::contact},
    {{"lip_nl", "lips", 0.0f, 100.0f, C_}, &Params::lip_nl},
    {{"embouch", "lips", 0.0f, 1.5f, C_}, &Params::embouch},
    {{"lip_tilt", "lips", -1.5f, 1.5f, C_}, &Params::lip_tilt},
    {{"jitter", "lips", 0.0f, 40.0f, C_}, &Params::jitter},
    {{"cup", "mouthpiece", 0.2f, 2.5f, C_}, &Params::cup},
    {{"pop_hz", "mouthpiece", 300.0f, 3000.0f, C_}, &Params::pop_hz},
    {{"pop_q", "mouthpiece", 0.5f, 10.0f, C_}, &Params::pop_q},
    {{"bore_decay", "bore", 0.005f, 0.2f, C_}, &Params::bore_t},
    {{"bore_reg", "bore", -1.0f, 2.0f, C_}, &Params::bore_reg},
    {{"bell_hz", "bore", 150.0f, 4000.0f, C_}, &Params::bell_hz},
    {{"brassiness", "bore", 0.0f, 1500.0f, C_}, &Params::brass}, // shift (samples) per 10 kPa of wave
    {{"rad_hz", "radiation", 20.0f, 2000.0f, C_}, &Params::rad_hz},
    {{"tone_hz", "radiation", 500.0f, 16000.0f, C_}, &Params::tone_hz},
    {{"tone_q", "radiation", 0.5f, 2.0f, C_}, &Params::tone_q},
    {{"low_comp", "radiation", 0.0f, 2.0f, C_}, &Params::low_comp},
    {{"breath", "air", 0.0f, 0.8f, C_}, &Params::breath},
    {{"t_noise", "air", 0.0f, 1.0f, C_}, &Params::t_noise},
    {{"tongue", "attack", 0.0f, 2.0f, C_}, &Params::tongue},
    {{"acc_level", "attack", 0.0f, 1.5f, C_}, &Params::acc_amp},
    {{"acc_decay", "attack", 0.01f, 0.3f, C_}, &Params::acc_t},
    {{"tongue_t", "attack", 0.001f, 0.05f, C_}, &Params::tongue_t},
    {{"tongue_lip", "attack", 0.0f, 2.0f, C_}, &Params::tongue_lip},
    {{"slur", "attack", 0.0f, 1.0f, C_}, &Params::slur},
    {{"pitch_t", "attack", 0.003f, 0.3f, C_}, &Params::pitch_t},
    {{"flutter", "mod", 0.0f, 1.0f, C_}, &Params::flutter},
    {{"flut_hz", "mod", 8.0f, 40.0f, C_}, &Params::flut_hz},
};
static constexpr int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

} // namespace trumpet

namespace strisy {
template <> struct EngineParams<trumpet::Engine> {
	static constexpr int count = trumpet::NUM_LIVE_PARAMS;
	[[nodiscard]] static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &trumpet::param_table[i].d : nullptr; }
	[[nodiscard]] static float* ptr(trumpet::Engine& e, int i) { return (i >= 0 && i < count) ? &(e.p.*(trumpet::param_table[i].m)) : nullptr; }
	static void changed(trumpet::Engine& e, int) { trumpet::update_derived(e); }
};
} // namespace strisy

#endif // VOICE_TRUMPET_H_
