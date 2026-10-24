/**
 * brass: lip-reed brass voice (trumpet, flugelhorn, trombone, horn, euphonium, tuba, muted brass).
 * Build with `make VOICE=brass`; presets with set_preset() (host renders: -DBRASS_PRESET=n).
 *
 * A hybrid of a lip source that sets the pitch and a physical bore that shapes it:
 *
 *   lips     an outward-striking valve that opens once per period. The flow pulse is
 *            max(cos(2 pi phi) - c, 0)^2 (smooth closure, no aliasing edges); the closure c follows
 *            the breath: soft playing keeps the lips almost sinusoidal (c < -1: fundamental and a
 *            weak 2nd harmonic), loud playing closes them for most of the cycle (c -> 0.7: a narrow,
 *            harmonic-rich pulse). The oscillator is the pitch master, so the voice is always in tune
 *            with `note` and can never jump registers or squeal. Lip life: a little pitch jitter
 *            (~3 cents), breath turbulence that is modulated by the lip opening (pulse-synchronous
 *            noise), and at a tongued attack a short "blat": a pitch scoop from below, a burst of
 *            irregular buzz (random FM), a brighter lip pulse, an overshoot and a tongue noise "t".
 *   bore     a waveguide loop one period long (two half-period delay lines in AXI SRAM: mouthpiece
 *            -> bell -> mouthpiece). At the bell the wave splits: the low frequencies reflect
 *            (one-pole lowpass at the bell cutoff, times the round-trip loss g, set from a decay time
 *            in seconds), the rest radiates. The loop delay is the period minus the bell filter's
 *            phase delay, so its resonances sit on the harmonics; in steady state the radiated wave
 *            equals the source (the loop is transparent), but in time it behaves like a real bore:
 *            the attack starts as a bright buzz while the low resonances build up (the bloom), a
 *            release leaves a short ringing tail, legato note changes smear for a moment.
 *   brassiness  the wave steepens on its way to the bell like a finite-amplitude wave in a real
 *            bore (pressure crests travel faster, fronts sharpen towards shock waves): the wave that
 *            radiates is read at a delay that moves with the pressure, d = D - k * p, in two explicit
 *            stages in series (the bore line itself, then a short line on the radiated path); in
 *            series they sharpen a front far more than one stage, like the physical (implicit)
 *            solution. The read speed of each stage is limited (fronts never fold back, never get
 *            shorter than ~5 samples: no aliasing grit on high notes). This is the dominant
 *            brightness mechanism: high harmonics grow like powers of the amplitude, so soft is
 *            round and loud blares, from mf upwards. The loop itself reflects the linear wave, so it
 *            stays linear (no chaos, no subharmonics), and the bell transmits (1 - reflection) of
 *            the steepened wave. k is a time shift in samples (~ bore length), longer for low notes.
 *   body     radiation high-pass, mouthpiece cup formant (peaking band-pass), bell/tone low-pass
 *            (its cutoff also rises with the level: a gentler second brightness mechanism), mutes:
 *            one state-variable filter whose high/band/low-pass mix makes a straight mute (high-pass
 *            with a resonance) or a harmon mute (narrow nasal resonance); forward/back tilt sweeps
 *            the mute resonance (wah, level compensated) and/or the tone low-pass (plunger / hand).
 *   section  optional second player: a second lip oscillator a few cents apart, own jitter.
 *
 * Every element is passive or bounded: the source is bounded, the loop gain is < 0.95 at all
 * frequencies (the bell lowpass has a non-negative impulse response, the interpolated reads are convex
 * combinations), the steepening only re-times samples (it can't add amplitude), filter coefficients
 * and gains are clamped. A per-voice guard resets a voice whose state is non-finite or absurd; the
 * master has a DC blocker and a soft ceiling at 4.2.
 *
 * Playing:
 *   pressure   breath: dynamics and brightness together (pp mellow -> ff blaring)
 *   strike     tonguing (positive vpres while held): a hard strike gives a bright blat, an accent,
 *              a slight scoop from below and a tongue "t"; re-striking a held key re-tongues
 *   release    the lips stop, the bore rings out briefly (pedal lengthens the tail a little)
 *   tilt fwd/back  open presets: brighter / darker (lips and bell; the horn's hand). Mute presets:
 *              wah (harmon: forward = open "wow", back = closed "wu"; plunger: back closes it)
 *   shake      flutter tongue
 *
 * Cost (ARM disassembly, per voice-sample): lips 38 instructions (59 with the section's second
 * player), bore + steepening 93 (43 FP), body filters 47 (58 with a mute); no divisions. Control
 * rate (4 per block): pulse shape, level, tone coefficients (~4 divisions, 2 square roots); block
 * rate: pitch, loop and filter coefficients (~10 divisions). Worst case ~10.5k cycles per voice per
 * block. Memory: Engine 8.3 KB (DTCM), bore delay lines 80 KB (AXI SRAM). Silent voices are skipped.
 */
#ifndef VOICE_BRASS_H_
#define VOICE_BRASS_H_

#include <stdint.h>
#include <type_traits>
#include <math.h>
#include "strisy.h"
#ifdef BRASS_TEST_HOOK
#include <stdio.h>
#endif

namespace brass {

using real = float;
static constexpr int BLOCK = strisy::BLOCK;
static constexpr int MAX_VOICES = 10;
static constexpr int CTRL = 8; // control rate divider (6.1 kHz)
static constexpr int NCTRL = BLOCK / CTRL;
static constexpr int NCV = 10; // interpolated control values (see process_voice)
static_assert(BLOCK % CTRL == 0, "CTRL must divide BLOCK");
static constexpr int BLEN = 1024; // each half of the bore loop (period <= ~2040 samples, MIDI >= 19.5)
static constexpr uint32_t BMASK = BLEN - 1;
static constexpr real DMAX = (real)(BLEN - 3);
static constexpr int SLEN = 128; // second steepening stage buffer
static constexpr uint32_t SMASK = SLEN - 1;
static constexpr int SD = 64; // its nominal delay (latency of the radiated wave, 1.3 ms)
// steepening: each stage compresses fronts at most (1 + BRASS_RL) times (host experiments: -DBRASS_RL=...)
#ifdef BRASS_RL
static constexpr real RL = BRASS_RL;
#else
static constexpr real RL = 3.0f;
#endif
// ... and stretch to at most 1/(1 - BRASS_RU) (never fold back) (host experiments: -DBRASS_RU=...)
#ifdef BRASS_RU
static constexpr real RU = BRASS_RU;
#else
static constexpr real RU = 0.95f;
#endif
static_assert(RU < 1.0f, "the read position never moves back (audio_bore's fast path relies on it)");
// ... and both stages together at most P0 / BRASS_RLDIV times (host experiments: -DBRASS_RLDIV=...)
#ifdef BRASS_RLDIV
static constexpr real RLDIV = BRASS_RLDIV;
#else
static constexpr real RLDIV = 20.0f;
#endif
// total steepening shift cap (fraction of the period) (host experiments: -DBRASS_KCAP=...)
#ifdef BRASS_KCAP
static constexpr real KCAP = BRASS_KCAP;
#else
static constexpr real KCAP = 0.3f;
#endif

using strisy::rmax;
using strisy::rmin;

// the bore delay lines: normal static arrays (AXI SRAM), 8 KB per voice
static float bore_buf[MAX_VOICES][2][BLEN];

// ---------------------------------------------------------------------------------------------
// Sound design parameters (all floats: they are the live parameters, see EngineParams below).
// lb = brightness level: the breath level (0..~1.3) plus the strike accent.

struct Params {
	// level
	real gain;         // output level
	real pc_a, pc_b;   // breath level from pressure: pres * (pc_a + pc_b * pres)
	real lv_hz;        // pressure smoothing (critically damped 2-pole)
	real rel, rel_ped; // release time constant (s): rel + rel_ped * pedal^2
	// lips
	real lip_soft, lip_loud; // lip closure c at lb = 0 and lb = 1 (-2 = sinusoidal .. 0.8 = narrow pulse)
	real lip_tilt;           // closure added by forward tilt (subtle lip brightness)
	real jitter;             // pitch jitter (cents, ~35 Hz band)
	real breath;             // breath turbulence, modulated by the lip opening
	// bore
	real brass;   // wave steepening: time shift (samples) at full level
	real bore_t;  // bore resonance decay time (s): bloom and tail
	real bell_hz; // bell reflection cutoff (resonances above it fade)
	// body
	real rad_hz;               // radiation high-pass corner
	real cup_hz, cup_q, cup_g; // mouthpiece cup formant: frequency, Q, peak boost (linear, +1)
	real tone_hz, tone_q;      // bell/tone low-pass at full level
	real tone_dyn;             // the tone low-pass closes by this many octaves from lb = 1 to lb = 0
	real tilt_tone;            // tilt: tone low-pass octaves per unit tilt (back = darker / closed)
	// mute
	real mute;                      // mute depth 0..1
	real mute_hz, mute_q;           // mute resonance
	real mute_hp, mute_bp, mute_lp; // output mix: high-pass (straight) / band-pass (harmon) / low-pass
	real wah;                       // tilt: mute resonance octaves per unit tilt (forward = open)
	// attack / tonguing
	real tongue;     // strike sensitivity
	real acc_amp;    // accent: level overshoot
	real acc_t;      // accent decay (s)
	real acc_bright; // accent: added brightness (lip closure, tone)
	real scoop;      // pitch scoop from below at a strike (semitones at full strike)
	real blat;       // attack buzz: random pitch flutter during the accent (semitones)
	real t_noise;    // tongue noise burst
	real slur;       // accent on a legato note change
	// modulation
	real vib, vib_hz;      // automatic (delayed) lip vibrato, semitones
	real flutter, flut_hz; // shake -> flutter tongue
	// section
	real pair;   // second player (detuned copy of the lips), 0 = solo
	real detune; // its detune (cents)
};

static constexpr int NPARAMS = sizeof(Params) / sizeof(real);
static_assert(sizeof(Params) == NPARAMS * sizeof(real), "Params must hold only floats");

[[nodiscard]] static inline Params trumpet_params() {
	Params p;
	p.gain = 0.328f;
	p.pc_a = 0.3f;
	p.pc_b = 0.7f;
	p.lv_hz = 40.0f;
	p.rel = 0.03f;
	p.rel_ped = 0.4f;
	p.lip_soft = -1.5f;
	p.lip_loud = 0.65f;
	p.lip_tilt = 0.15f;
	p.jitter = 3.0f;
	p.breath = 0.08f;
	p.brass = 16.0f;
	p.bore_t = 0.025f;
	p.bell_hz = 1400.0f;
	p.rad_hz = 350.0f;
	p.cup_hz = 1200.0f;
	p.cup_q = 1.4f;
	p.cup_g = 1.5f;
	p.tone_hz = 5500.0f;
	p.tone_q = 0.8f;
	p.tone_dyn = 1.5f;
	p.tilt_tone = 0.6f;
	p.mute = 0.0f;
	p.mute_hz = 1700.0f;
	p.mute_q = 4.0f;
	p.mute_hp = 0.3f;
	p.mute_bp = 2.5f;
	p.mute_lp = 0.0f;
	p.wah = 0.0f;
	p.tongue = 1.0f;
	p.acc_amp = 0.35f;
	p.acc_t = 0.05f;
	p.acc_bright = 0.5f;
	p.scoop = 0.25f;
	p.blat = 0.25f;
	p.t_noise = 0.15f;
	p.slur = 0.25f;
	p.vib = 0.0f;
	p.vib_hz = 5.5f;
	p.flutter = 0.6f;
	p.flut_hz = 24.0f;
	p.pair = 0.0f;
	p.detune = 8.0f;
	return p;
}

static constexpr int NUM_PRESETS = 9;

[[nodiscard]] static inline const char* preset_name(int i) {
	static constexpr const char* names[NUM_PRESETS] = {
	    "trumpet", "flugelhorn", "trombone", "horn", "euphonium", "tuba", "harmon", "plunger", "section"};
	return (i >= 0 && i < NUM_PRESETS) ? names[i] : names[0];
}

[[nodiscard]] static inline Params preset_params(int i) {
	Params p = trumpet_params();
	switch (i) {
	case 1: // flugelhorn: conical, deep cup: dark and mellow, little steepening
		p.gain = 0.415f;
		p.lip_soft = -1.8f;
		p.lip_loud = 0.4f;
		p.breath = 0.12f;
		p.brass = 8.0f;
		p.bore_t = 0.03f;
		p.bell_hz = 900.0f;
		p.rad_hz = 250.0f;
		p.cup_hz = 750.0f;
		p.cup_q = 1.2f;
		p.cup_g = 1.0f;
		p.tone_hz = 2600.0f;
		p.tone_q = 0.7f;
		p.tone_dyn = 1.2f;
		p.acc_amp = 0.25f;
		p.acc_bright = 0.3f;
		p.scoop = 0.15f;
		p.blat = 0.1f;
		p.t_noise = 0.1f;
		break;
	case 2: // trombone: long cylindrical bore: round when soft, the brassiest when loud
		p.gain = 0.411f;
		p.lip_soft = -1.5f;
		p.lip_loud = 0.7f;
		p.brass = 30.0f;
		p.bore_t = 0.04f;
		p.bell_hz = 700.0f;
		p.rad_hz = 150.0f;
		p.cup_hz = 550.0f;
		p.cup_q = 1.3f;
		p.cup_g = 1.5f;
		p.tone_hz = 4500.0f;
		p.tone_dyn = 1.8f;
		p.acc_amp = 0.35f;
		p.blat = 0.35f;
		p.scoop = 0.3f;
		break;
	case 3: // French horn: hand in the bell, dark and round, cuivre only when pushed
		p.gain = 0.444f;
		p.lv_hz = 25.0f;
		p.lip_soft = -1.8f;
		p.lip_loud = 0.6f;
		p.breath = 0.06f;
		p.brass = 20.0f;
		p.bore_t = 0.06f;
		p.bell_hz = 500.0f;
		p.rad_hz = 120.0f;
		p.cup_hz = 450.0f;
		p.cup_q = 1.2f;
		p.cup_g = 0.8f;
		p.tone_hz = 1600.0f;
		p.tone_q = 1.1f;
		p.tone_dyn = 1.4f;
		p.tilt_tone = 0.8f;
		p.acc_amp = 0.2f;
		p.acc_t = 0.07f;
		p.scoop = 0.2f;
		p.blat = 0.15f;
		p.t_noise = 0.08f;
		break;
	case 4: // euphonium: conical tenor tuba, warm and singing
		p.gain = 0.478f;
		p.lip_soft = -1.8f;
		p.lip_loud = 0.45f;
		p.breath = 0.08f;
		p.brass = 10.0f;
		p.bore_t = 0.05f;
		p.bell_hz = 500.0f;
		p.rad_hz = 100.0f;
		p.cup_hz = 400.0f;
		p.cup_q = 1.2f;
		p.cup_g = 0.8f;
		p.tone_hz = 2000.0f;
		p.tone_q = 0.7f;
		p.tone_dyn = 1.3f;
		p.acc_amp = 0.25f;
		p.acc_t = 0.06f;
		p.scoop = 0.15f;
		p.blat = 0.12f;
		break;
	case 5: // tuba: big, warm low brass
		p.gain = 0.473f;
		p.lip_soft = -1.6f;
		p.lip_loud = 0.5f;
		p.breath = 0.06f;
		p.brass = 10.0f;
		p.bore_t = 0.06f;
		p.bell_hz = 350.0f;
		p.rad_hz = 60.0f;
		p.cup_hz = 280.0f;
		p.cup_q = 1.1f;
		p.cup_g = 1.0f;
		p.tone_hz = 1500.0f;
		p.tone_q = 0.7f;
		p.tone_dyn = 1.5f;
		p.acc_amp = 0.3f;
		p.acc_t = 0.07f;
		p.scoop = 0.2f;
		p.blat = 0.15f;
		p.t_noise = 0.08f;
		break;
	case 6: // harmon-muted trumpet: thin and nasal, tilt is the hand on the stem (wah)
		p.gain = 0.29f;
		p.mute = 1.0f;
		p.mute_hz = 1700.0f;
		p.mute_q = 5.0f;
		p.mute_hp = 0.2f;
		p.mute_bp = 3.0f;
		p.mute_lp = 0.25f;
		p.wah = 1.0f;
		p.tilt_tone = 0.0f;
		p.lip_tilt = 0.0f;
		p.lip_soft = -0.8f;
		p.breath = 0.12f; // a buzzier embouchure: soft notes still feed the mute
		break;
	case 7: // trombone with a straight mute and a plunger over it (tilt back closes the plunger)
		p.gain = 0.412f;
		p.lip_soft = -1.5f;
		p.lip_loud = 0.7f;
		p.brass = 30.0f;
		p.bore_t = 0.04f;
		p.bell_hz = 700.0f;
		p.rad_hz = 150.0f;
		p.cup_hz = 550.0f;
		p.cup_q = 1.3f;
		p.cup_g = 1.5f;
		p.tone_hz = 4500.0f;
		p.tone_q = 1.3f;
		p.tone_dyn = 1.8f;
		p.tilt_tone = 2.0f;
		p.lip_tilt = 0.0f;
		p.mute = 0.8f;
		p.mute_hz = 1000.0f;
		p.mute_q = 1.4f;
		p.mute_hp = 1.4f;
		p.mute_bp = 0.6f;
		p.mute_lp = 0.3f;
		p.wah = 0.5f;
		p.blat = 0.35f;
		p.scoop = 0.3f;
		break;
	case 8: // brass section: two players a few cents apart
		p.gain = 0.507f;
		p.lip_soft = -1.6f;
		p.lip_loud = 0.55f;
		p.brass = 18.0f;
		p.bore_t = 0.035f;
		p.bell_hz = 1000.0f;
		p.rad_hz = 220.0f;
		p.cup_hz = 900.0f;
		p.cup_q = 1.3f;
		p.cup_g = 1.2f;
		p.tone_hz = 4000.0f;
		p.tone_dyn = 1.5f;
		p.pair = 1.0f;
		p.detune = 9.0f;
		p.jitter = 4.0f;
		break;
	default:
		break;
	}
	return p;
}

// preset after init() (host renders: -DBRASS_PRESET=n)
#ifdef BRASS_PRESET
static constexpr int DEFAULT_PRESET = BRASS_PRESET;
#else
static constexpr int DEFAULT_PRESET = 0;
#endif

struct VoiceState {
	uint32_t ph, ph2;            // lip phases (32 bit fixed point, wrap = one period)
	uint32_t inc, inc2;          // phase increments at the end of the previous block
	real lv1, lv2;               // pressure smoother (control rate SVF)
	real amp;                    // breath level after the release logic
	real acc, tn, sc;            // accent, tongue noise and scoop envelopes
	real ty1, ty2;               // tilt smoother (block rate SVF)
	real d1;                     // stage 1 steepened read delay
	real lps;                    // bell reflection lowpass state
	real rs;                     // radiation high-pass state
	real c1, c2, t1, t2, m1, m2; // cup, tone and mute filter states
	real ctrl_prev[NCV];         // last control point of the previous block
	real jn, jn2, bn, dr;        // jitter (both players), blat noise, slow pitch wander (block rate)
	uint32_t rng, nrng;          // block-rate and audio-rate noise generators
	real vib_ph, vib_env, flut_ph;
	real note_prev;
	real vneg;      // release speed latch
	uint32_t w;     // delay line write index
	real ds;        // second steepening stage: read delay
	uint32_t ws;    // second stage write index
	real lpt;       // bell lowpass state of the transmitted (steepened) wave
	float sb[SLEN]; // second stage: the steepened wave
	int active;
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	Params p;  // parameters as set (presets, live edits)
	Params ps; // smoothed copy used by the DSP (de-zippers live edits)
	real sr, pi_sr, ph_scale, ctrl_dt, block_dt;
	strisy::SvfCoef lv_c, ty_c;
	real shake_pk, shake; // global shake envelope
	real dc, dc_c;        // master DC blocker
	VoiceState vs[MAX_VOICES];
	real src[BLOCK];
	real vout[BLOCK];
	real mix[BLOCK];
	real ctrl[NCTRL][NCV];
};

// ---------------------------------------------------------------------------------------------
// helpers (no libm)

// 2^p for any p (fast.lib's fastpow2 needs p >= 0: the fraction is taken after a floor here)
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

[[nodiscard]] static inline real note_freq(real note) { return 8.1757989f * exp2s(note * (1.0f / 12)); }

// tan(x) for 0 <= x < ~1.4 ([7/6] Pade, as strisy::svf_pade)
[[nodiscard]] static inline real tanp(real x) {
	const real x2 = x * x;
	return x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2))) / (135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2)));
}

// atan(x) for x >= 0 (max error ~1e-5)
[[nodiscard]] static inline real atanp(real x) {
	const bool inv = x > 1.0f;
	const real t = inv ? 1.0f / x : x;
	const real t2 = t * t;
	const real a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	return inv ? 1.5707963f - a : a;
}

// square root as the single FPU instruction (sqrtf may call libm for the errno case)
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

// parabolic sine of a phase in [0, 1) (LFOs)
[[nodiscard]] static inline real psin(real ph) {
	const real q = ph - 0.5f;
	return -8.0f * q * (1.0f - 2.0f * fabsf(q));
}

// soft clipper: x - 4/27 x^3 on |x| <= 1.5, +-1 beyond
[[nodiscard]] static inline real sat(real x) {
	x = rmin(rmax(x, -1.5f), 1.5f);
	return x * (1.0f - 0.148148148f * x * x);
}

// lip pulse u = max(cos - c, 0)^2 / (1 - c)^2: returns 1/(1-c)^2, the mean of u and the amplitude of
// its fundamental (all exact)
static inline void pulse_norm(real c, real& inv, real& mean, real& a1) {
	const real omc = 1.0f - c;
	inv = 1.0f / (omc * omc);
	real m, f;
	if (c <= -0.999f) {
		m = 0.5f + c * c; // never closes: mean of (cos - c)^2
		f = -2.0f * c;
	} else {
		// open for |theta| < alpha = acos(c), s = sin(alpha):
		//   mean = (alpha (1/2 + c^2) - 3/2 c s) / pi,  fundamental = 2/pi (s - s^3/3 - c alpha)
		const real sn = fsqrt(rmax(1.0f - c * c, 0.0f));
		const real alpha = 2.0f * atanp(fsqrt(omc / (1.0f + c)));
		m = (alpha * (0.5f + c * c) - 1.5f * c * sn) * 0.318309886f;
		f = (sn - sn * sn * sn * (1.0f / 3) - c * alpha) * 0.636619772f;
	}
	mean = m * inv;
	a1 = rmax(f * inv, 0.05f);
}

static inline void reset_voice(VoiceState& s, int v) {
	s.lv1 = s.lv2 = 0.0f;
	s.amp = s.acc = s.tn = s.sc = 0.0f;
	s.ty1 = s.ty2 = 0.0f;
	s.d1 = 0.0f;
	s.lps = s.rs = 0.0f;
	s.c1 = s.c2 = s.t1 = s.t2 = s.m1 = s.m2 = 0.0f;
	for (int k = 0; k < NCV; k++) s.ctrl_prev[k] = 0.0f;
	s.jn = s.jn2 = s.bn = s.dr = 0.0f;
	s.vneg = 0.0f;
	s.vib_env = 0.0f;
	s.inc = s.inc2 = 0;
	s.w = 0;
	s.ds = (real)SD;
	s.ws = 0;
	s.lpt = 0.0f;
	for (int i = 0; i < SLEN; i++) s.sb[i] = 0.0f;
	if (v >= 0 && v < MAX_VOICES)
		for (int h = 0; h < 2; h++)
			for (int i = 0; i < BLEN; i++) bore_buf[v][h][i] = 0.0f;
}

static inline void update_derived(Engine& e) {
	e.lv_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * CTRL * rmin(e.p.lv_hz, 400.0f)), 0.5f); // critically damped
}

// Select a preset; callable between blocks at any time (after init). Sounding voices continue with
// the new parameters (every state stays bounded for any parameter set).
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
	e.pi_sr = 3.14159265f / sr;
	e.ph_scale = 4294967296.0f / sr;
	e.ctrl_dt = (real)CTRL / sr;
	e.block_dt = (real)BLOCK / sr;
	e.ty_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * BLOCK * 12.0f), 0.5f); // 12 Hz
	e.shake_pk = e.shake = 0.0f;
	e.dc = 0.0f;
	e.dc_c = 2.0f * 3.14159265f * 12.0f / sr;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	for (int v = 0; v < MAX_VOICES; v++) {
		VoiceState& s = e.vs[v];
		s = VoiceState();
		reset_voice(s, v);
		s.rng = 0x9E3779B9u * (uint32_t)(v + 1) + 12345u;
		s.nrng = 0x85EBCA6Bu * (uint32_t)(v + 3) + 999u;
		s.ph = s.rng * 2654435761u;
		s.ph2 = s.ph + 0x30000000u;
		s.vib_ph = 0.17f * (real)v;
		s.vib_ph -= (real)(int)s.vib_ph;
		s.flut_ph = 0.31f * (real)v;
		s.flut_ph -= (real)(int)s.flut_ph;
		s.note_prev = 69.0f;
		s.active = 0;
	}
	for (int v = 0; v < e.nvoices; v++) e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
	set_preset(e, DEFAULT_PRESET);
}

static inline void process_global(Engine& e) {
	// shaking -> flutter depth: peak hold with ~0.4 s decay, then smoothed
	const real sh = rmin(rmax(e.g.acc_abs - 1.1f, 0.0f), 1.0f);
	e.shake_pk = rmax(sh, e.shake_pk * (1.0f - e.block_dt * 2.5f));
	e.shake += 0.03f * (e.shake_pk - e.shake);
}

#ifdef BRASS_TEST_HOOK
static int brass_guard_resets = 0; // host test: voices reset by the guard
static void brass_test_hook(Engine& e);
#endif

[[nodiscard]] static inline real block_noise(uint32_t& r) {
	r = r * 1664525u + 1013904223u;
	return (real)(int32_t)r * 4.656613e-10f; // uniform -1..1
}

// bore loop, both steepening stages and the bell: the radiated wave -> e.vout
// The two linear reads use the block's delays (they move by < 0.05 samples per block under vibrato).
static inline void audio_bore(Engine& e, VoiceState& s, float* __restrict__ B1, float* __restrict__ B2,
    real d1n, real d2, real ks, real ks2, real rl, real g, real Gb) {
	real d1 = s.d1, lps = s.lps, lpt = s.lpt, ds = s.ds;
	uint32_t w = s.w, ws = s.ws;
	const uint32_t i2 = (uint32_t)(int)d2, in = (uint32_t)(int)d1n;
	const real f2 = d2 - (real)(int)i2, fn = d1n - (real)(int)in;
	float* __restrict__ SB = s.sb;
	const real* __restrict__ src = e.src;
	real* __restrict__ out = e.vout;
	// The bore reads: (w - i2, w - i2 - 1), (w - in, w - in - 1) and the steepened (w - i1, w - i1 - 1)
	// & BMASK, w rising by one per sample. d1 moves by at most RU (up) or rl (down) per sample, so
	// over the block i1 stays within [d1 - 32 rl - 1, d1 + 32 RU + 1] of its start. When none of
	// the reads wraps around the buffer's end in this block (most blocks), the loop addresses the
	// same slots as plain indices from per-block offsets (identical results).
	const int c = (int)w;                       // (w is kept masked)
	const bool d1ok = d1 >= 0.0f && d1 <= DMAX; // (false for NaN)
	const int i1lo = c - (d1ok ? (int)d1 : 0) - 4, span1 = 4 + 33 + (int)(32.0f * rmin(rl, RL)) + 2;
	const int y2 = (int)((w - i2 - 1u) & BMASK), ya = (int)((w - in - 1u) & BMASK), y1 = (int)((uint32_t)i1lo & BMASK);
	const bool fast = d1ok && c + BLOCK <= BLEN && y2 + BLOCK < BLEN && ya + BLOCK < BLEN && y1 + span1 < BLEN;
	// fast: slot = w + offset (o2, oa: the fixed reads; o1 - i1: the steepened read)
	const int o2 = y2 + 1 - c, oa = ya + 1 - c, o1 = y1 - i1lo;
	auto run = [&](auto fast_tag) {
		constexpr bool FAST = decltype(fast_tag)::value;
#pragma GCC unroll 2
		for (int n = 0; n < BLOCK; n++) {
			// return wave (bell -> mouthpiece), read at d2 >= 1, plus the lips -> outgoing wave
			const real r0 = FAST ? B2[(int)w + o2] : B2[(w - i2) & BMASK], r1 = FAST ? B2[(int)w + o2 - 1] : B2[(w - i2 - 1u) & BMASK];
			B1[w] = src[n] + r0 + f2 * (r1 - r0);
			// the outgoing wave arriving at the bell: the linear wave closes the loop ...
			const real a0 = FAST ? B1[(int)w + oa] : B1[(w - in) & BMASK], a1 = FAST ? B1[(int)w + oa - 1] : B1[(w - in - 1u) & BMASK];
			const real bl = a0 + fn * (a1 - a0);
			const real v = (bl - lps) * Gb; // bell: low frequencies reflect (one-pole TPT lowpass x g)
			const real lp = v + lps;
			lps = lp + v;
			B2[w] = g * lp;
			// ... and the steepened wave radiates: higher pressure arrives earlier. Stage 1 reads the bore
			// line at d1 = d1n - ks * (pressure at the nominal position); the read speed stays within
			// 1 - BRASS_RU .. 1 + rl samples per sample (fronts never fold back, never shorter than ~5 samples)
			real dn = d1n - ks * bl;
			dn = rmin(rmax(dn, d1 - rl), d1 + RU);
			d1 = rmin(rmax(dn, 0.0f), DMAX);
			const int i1 = (int)d1;
			const real f1 = d1 - (real)i1;
			const real b0 = FAST ? B1[(int)w - i1 + o1] : B1[(w - (uint32_t)i1) & BMASK];
			const real b1 = FAST ? B1[(int)w - i1 + o1 - 1] : B1[(w - (uint32_t)i1 - 1u) & BMASK];
			const real b = b0 + f1 * (b1 - b0);
			w = (w + 1u) & BMASK;
			// stage 2: the same on stage 1's output (in series they sharpen a front far more than one stage)
			SB[ws] = b;
			const real ynom = SB[(ws - (uint32_t)SD) & SMASK];
			real dsn = (real)SD - ks2 * ynom;
			dsn = rmin(rmax(dsn, ds - rl), ds + RU);
			ds = rmin(rmax(dsn, 1.0f), (real)(SLEN - 3));
			const int is = (int)ds;
			const real fs = ds - (real)is;
			const real y0 = SB[(ws - (uint32_t)is) & SMASK], y1 = SB[(ws - (uint32_t)is - 1u) & SMASK];
			const real yst = y0 + fs * (y1 - y0);
			ws = (ws + 1u) & SMASK;
			// the bell transmits what it doesn't reflect: (1 - g LP) of the steepened wave. (The loop
			// reflects the linear wave, so it stays linear and can't turn chaotic.)
			const real vt = (yst - lpt) * Gb;
			const real lt = vt + lpt;
			lpt = lt + vt;
			out[n] = yst - g * lt;
		}
	};
	if (fast) run(std::true_type{});
	else run(std::false_type{});
	s.d1 = d1;
	s.lps = lps;
	s.lpt = lpt;
	s.ds = ds;
	s.w = w;
	s.ws = ws;
}

// radiation high-pass, cup formant, tone low-pass, mute, gain: e.vout in place
template <bool MUTE>
static inline void audio_body(Engine& e, VoiceState& s, real Gr, const strisy::SvfCoef& cupc, real cupG,
    const strisy::SvfCoef& mc, real mdep, real wh, real wb, real wl, real gain) {
	real rs = s.rs, c1 = s.c1, c2 = s.c2, t1 = s.t1, t2 = s.t2, m1 = s.m1, m2 = s.m2;
	real ta = s.ctrl_prev[7], tga = s.ctrl_prev[8], tg2a = s.ctrl_prev[9];
	real* __restrict__ out = e.vout;
	for (int k = 0; k < NCTRL; k++) {
		const real* cur = e.ctrl[k];
		const real r = 1.0f / CTRL;
		const real dta = (cur[7] - ta) * r, dtga = (cur[8] - tga) * r, dtg2a = (cur[9] - tg2a) * r;
#pragma GCC unroll 2
		for (int j = 0; j < CTRL; j++) {
			ta += dta;
			tga += dtga;
			tg2a += dtg2a;
			const int n = k * CTRL + j;
			const real o = out[n];
			// radiation high-pass (one-pole TPT)
			const real vr = (o - rs) * Gr;
			const real lr = vr + rs;
			rs = lr + vr;
			const real hp = o - lr;
			// mouthpiece cup formant: peaking band-pass
			real bp, lo;
			SVF_TICK(hp, c1, c2, cupc, bp, lo);
			const real y = hp + cupG * bp;
			// tone / bell low-pass (level dependent, interpolated coefficients)
			const strisy::SvfCoef tc = {ta, tga, tg2a, 0.0f};
			real tb, ty;
			SVF_TICK(y, t1, t2, tc, tb, ty);
			real o2 = ty;
			if (MUTE) {
				real mb, ml;
				SVF_TICK(ty, m1, m2, mc, mb, ml);
				const real mh = ty - mc.k * mb - ml;
				o2 = ty + mdep * (wh * mh + wb * mb + wl * ml - ty);
			}
			out[n] = o2 * gain;
		}
		ta = cur[7];
		tga = cur[8];
		tg2a = cur[9]; // exact control point (no drift)
	}
	s.rs = rs;
	s.c1 = c1;
	s.c2 = c2;
	s.t1 = t1;
	s.t2 = t2;
	s.m1 = m1;
	s.m2 = m2;
	s.ctrl_prev[7] = ta;
	s.ctrl_prev[8] = tga;
	s.ctrl_prev[9] = tg2a;
}

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const Params& P = e.ps;
	const strisy::VoiceControls cc = e.vc[v];
	const real pres = rmin(rmax(cc.pres, 0.0f), 1.5f);
	// after a release the firmware leaves the last vpres in place: a positive leftover must not
	// strike or hold a released voice; negative values still set the release speed
	const real vpres = rmin(rmax(cc.vpres, -1.0f), pres > 0.0f ? 1.0f : 0.0f);
	const real vp1 = rmax(vpres - 0.02f, 0.0f);
	const real vplev = rmin(P.tongue * vp1 / (0.4f + vp1), 1.0f);
	const real note = rmin(rmax(cc.note, 0.0f), 132.0f);
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

	// ---- block rate -------------------------------------------------------------------------
	const bool jump = start || fabsf(note - s.note_prev) >= 1.0f; // a new note
	if (jump && !start && held) s.acc = rmax(s.acc, P.slur);      // legato change: a small articulation
	if (jump) {
		s.vib_env = 0.0f;
		s.vneg = 0.0f;
	}
	s.note_prev = note;
	const real by = rmin(rmax(cc.but_y, -1.0f), 1.0f);
	if (start) {
		s.ty1 = 0.0f;
		s.ty2 = by;
	}
	real y;
	{
		real ty1 = s.ty1, ty2 = s.ty2, t1;
		SVF_TICK(by, ty1, ty2, e.ty_c, t1, y);
		s.ty1 = ty1;
		s.ty2 = ty2;
	}
	y = rmin(rmax(y, -1.0f), 1.0f);

	// lip life: jitter (~35 Hz noise) and blat buzz (wide band, only during the accent)
	s.jn += 0.15f * (6.0f * block_noise(s.rng) - s.jn);
	s.bn += 0.6f * (1.7f * block_noise(s.rng) - s.bn);
	s.jn = rmin(rmax(s.jn, -4.0f), 4.0f);
	s.dr += 0.003f * (45.0f * block_noise(s.rng) - s.dr); // ~0.7 Hz, std ~1
	s.dr = rmin(rmax(s.dr, -4.0f), 4.0f);
	s.bn = rmin(rmax(s.bn, -3.0f), 3.0f);
	// delayed automatic vibrato
	s.vib_ph += P.vib_hz * e.block_dt;
	if (s.vib_ph >= 1.0f) s.vib_ph -= (real)(int)s.vib_ph;
	if (held) s.vib_env = rmin(s.vib_env + e.block_dt * 1.6f, 1.0f);
	const real vib = rmin(rmax(P.vib, 0.0f), 2.0f) * rmax(s.vib_env - 0.3f, 0.0f) * 1.43f * psin(s.vib_ph);

	// pitches: the bore follows the note; the lips add scoop, jitter and buzz on top
	const real nloop = note + vib + 0.005f * P.jitter * s.dr; // slow wander: the bore follows
	const real acc0 = s.acc;
	const bool pair_on = P.pair > 0.001f;
	// section: the two players sit symmetrically around the note, each with its own jitter
	const real half_det = pair_on ? 0.005f * rmin(rmax(P.detune, 0.0f), 50.0f) * rmin(P.pair, 1.0f) : 0.0f;
	const real nlip = nloop - P.scoop * s.sc + 0.01f * P.jitter * s.jn + P.blat * acc0 * acc0 * s.bn - half_det;
	const real fmax = 0.45f * e.sr;
	const real f0 = rmin(note_freq(nlip), fmax);
	const uint32_t inc1 = (uint32_t)(f0 * e.ph_scale) | 1u;
	if (pair_on) {
		s.jn2 += 0.15f * (6.0f * block_noise(s.rng) - s.jn2);
		s.jn2 = rmin(rmax(s.jn2, -4.0f), 4.0f);
	}
	const real n2 = nloop - 0.8f * P.scoop * s.sc + 0.01f * P.jitter * s.jn2 + P.blat * acc0 * acc0 * s.bn + half_det;
	const real f2r = rmin(note_freq(n2), fmax);
	const uint32_t inc2 = (uint32_t)(f2r * e.ph_scale) | 1u;
	if (jump || s.inc == 0) {
		s.inc = inc1;
		s.inc2 = inc2;
	}
	const int32_t dA = (int32_t)(inc1 - s.inc) / BLOCK;
	const int32_t dB = (int32_t)(inc2 - s.inc2) / BLOCK;

	// bore loop: period P0, bell reflection lowpass, round trip loss from the decay time
	const real fl = rmin(note_freq(nloop), fmax);
	const real P0 = e.sr / fl;
	const real tone_y = P.tilt_tone * y;
	const real fb = rmin(rmax(P.bell_hz * exp2s(0.5f * tone_y), 50.0f), 8000.0f);
	const real gb = tanp(e.pi_sr * fb);
	const real Gb = gb / (1.0f + gb);
	const real t0 = tanp(rmin(3.14159265f / P0, 1.3f));
	const real ratio = t0 / gb;
	const real lag = atanp(ratio);                         // bell lowpass phase lag at f0
	const real lpmag = 1.0f / fsqrt(1.0f + ratio * ratio); // and its magnitude
	const real Dtot = rmax(P0 - lag * P0 * 0.159154943f, 2.0f);
	real g = exp2s(-1.442695f * P0 / (rmax(P.bore_t, 0.002f) * e.sr));
	g = rmin(g, 0.95f) * rmin(rmax((2040.0f - P0) * 0.01f, 0.0f), 1.0f); // no resonance below MIDI ~19.5
	const real geff = g * lpmag;
	const real d1t = rmin(rmax(0.5f * Dtot, 0.5f), 1000.0f);
	const real d2t = rmin(rmax(Dtot - d1t, 1.0f), 1000.0f);
	if (start) s.d1 = d1t;
	// steepening: total time shift ~ P.brass samples at full level (the internal wave is ~1/(1-geff)
	// larger than the source), split over two explicit stages (each one is safe on its own; in series
	// they sharpen a front much more than one stage, like the physical, implicit solution). Low notes
	// get a longer shift (~ sqrt(period)): their mouthpiece pressure swings are larger and their
	// spectra reach as high as the upper notes' when loud. A strike steepens more (the blat).
	const real kkey = rmin(rmax(fsqrt(P0 * (1.0f / 120)), 0.6f), 3.0f);
	const real ktot = rmin(rmax(P.brass, 0.0f) * kkey * (1.0f + 1.5f * rmax(P.acc_bright, 0.0f) * acc0), KCAP * P0);
	const real knorm = 2.5f * (1.0f - geff);
	const real ks = rmin(0.5f * ktot, 0.8f * d1t) * knorm;
	const real ks2 = rmin(0.5f * ktot, (real)(SD - 8)) * knorm;
	// each stage compresses a front at most (1 + rl) times: together at most ~P0/12 times, so a front
	// (~P0/4 long) never gets shorter than ~3 samples (no aliasing on high notes)
	const real rl = rmin(rmax(fsqrt(P0 * (1.0f / RLDIV)) - 1.0f, 0.15f), RL);

	// body filters (block rate)
	const real fr = rmin(rmax(P.rad_hz, 10.0f), 4000.0f);
	const real gr = tanp(e.pi_sr * fr);
	const real Gr = gr / (1.0f + gr);
	const real cq = rmin(rmax(P.cup_q, 0.3f), 8.0f);
	const real fcup = rmin(rmax(P.cup_hz, 50.0f), 8000.0f);
	const strisy::SvfCoef cupc0 = strisy::svf_pade(e.pi_sr * fcup, cq);
	const strisy::SvfCoef cupc = {cupc0.a, cupc0.ga, cupc0.g2a, 1.0f / cq};
	const real cupG = rmin(rmax(P.cup_g, 0.0f), 6.0f) / cq; // band-pass (peak gain Q) -> boost cup_g
	const real mdep = rmin(rmax(P.mute, 0.0f), 1.0f);
	const bool mute_on = mdep > 0.0f;
	strisy::SvfCoef mc = {0, 0, 0, 1};
	real wh = 0, wb = 0, wl = 0;
	if (mute_on) {
		const real mq = rmin(rmax(P.mute_q, 0.3f), 12.0f);
		const real fm = rmin(rmax(P.mute_hz * exp2s(P.wah * y), 100.0f), 8000.0f);
		const strisy::SvfCoef m0 = strisy::svf_pade(e.pi_sr * fm, mq);
		mc = {m0.a, m0.ga, m0.g2a, 1.0f / mq};
		// a resonance swept up sits on weaker harmonics: keep the open (forward) wah as loud as closed
		const real wcomp = exp2s(rmin(rmax(0.8f * P.wah * y, -2.0f), 2.0f));
		wh = P.mute_hp * wcomp;
		wb = P.mute_bp / mq * wcomp;
		wl = P.mute_lp * wcomp;
	}

	// envelope rates
	if (vpres > 0.1f) s.vneg = 0.0f;
	if (held && vpres < 0.0f) s.vneg = rmax(s.vneg, -vpres);
	const real ped = rmin(rmax(e.g.pedal, 0.0f), 1.0f);
	const real rel_tc = rmax(P.rel + P.rel_ped * ped * ped, 0.004f) / (1.0f + 3.0f * s.vneg);
	const real fac = 1.0f - e.ctrl_dt / (held ? 0.03f : rel_tc);
	const real fac_acc = 1.0f - e.ctrl_dt / rmax(P.acc_t, 0.005f);
	const real fac_sc = 1.0f - e.ctrl_dt / rmax(0.6f * P.acc_t, 0.005f);
	const real fac_tn = 1.0f - e.ctrl_dt / 0.006f;
	const real pc = pres * (P.pc_a + P.pc_b * pres);
	// lip closure: soft -> loud; the narrowest pulse is limited for high notes (bandwidth)
	const real cmax = rmin(rmax(0.85f - (note - 84.0f) * 0.035f, -0.6f), 0.85f);
	const real lip_lo = P.lip_soft, lip_d = P.lip_loud - P.lip_soft, lip_y = P.lip_tilt * y;
	const real c_knee = rmin(P.lip_loud, cmax - 0.1f);
	const real c_span = cmax - c_knee, c_span_inv = 1.0f / c_span;
	const real pairlv = pair_on ? rmin(P.pair, 1.0f) : 0.0f;
	const real pnorm = 1.0f / (1.0f + 0.75f * pairlv);
	const real tone_lo = rmin(rmax(P.tone_hz, 100.0f), 16000.0f);
	const real tq = rmin(rmax(P.tone_q, 0.5f), 4.0f);
	const real shake = e.shake * rmax(P.flutter, 0.0f);
	const real flut_inc = P.flut_hz * e.ctrl_dt;

	// ---- control rate: breath level, accent, lip closure, tone ----------------------------------
	{
		real lv1 = s.lv1, lv2 = s.lv2, amp = s.amp, acc = s.acc, tn = s.tn, sc = s.sc, fph = s.flut_ph;
		const strisy::SvfCoef lc = e.lv_c;
		for (int k = 0; k < NCTRL; k++) {
			acc = rmax(acc * fac_acc, vplev);
			sc = rmax(sc * fac_sc, vplev);
			tn = rmax(tn * fac_tn, vplev);
			real t1, ls;
			SVF_TICK(rmin(pc + P.acc_amp * acc, 2.0f), lv1, lv2, lc, t1, ls);
			amp = rmax(rmax(ls, 0.0f), amp * fac);
			// brightness level: ~ breath pressure (sqrt of the level), plus the strike accent
			const real lb = rmin(fsqrt(amp) + P.acc_bright * acc, 1.5f);
			// lip closure: linear in lb up to lip_loud, then saturating smoothly towards cmax
			real c = rmax(lip_lo + lip_d * lb + lip_y, -2.0f);
			if (c > c_knee) {
				const real t = (c - c_knee) * c_span_inv;
				c = c_knee + c_span * t / (1.0f + t);
			}
			real inv, mean, a1, inv2 = 0.0f, mean2 = 0.0f, a12 = 0.0f;
			pulse_norm(c, inv, mean, a1);
			if (pair_on) pulse_norm(rmin(c + 0.12f, cmax), inv2, mean2, a12);
			fph += flut_inc;
			if (fph >= 1.0f) fph -= 1.0f;
			const real am = 1.0f - shake * (0.5f + 0.5f * psin(fph));
			const real ft = rmin(tone_lo * exp2s(P.tone_dyn * (lb - 1.0f) + tone_y), 16000.0f);
			const strisy::SvfCoef tcf = strisy::svf_pade(e.pi_sr * rmax(ft, 40.0f), tq);
			real* cv = e.ctrl[k];
			cv[0] = amp * am * pnorm * (0.5f / a1); // the fundamental follows the breath level
			cv[1] = c;
			cv[2] = inv;
			cv[3] = mean + pairlv * mean2;
			cv[4] = P.t_noise * tn * tn;
			cv[5] = inv2 * pairlv;
			cv[6] = rmin(c + 0.12f, cmax);
			cv[7] = tcf.a;
			cv[8] = tcf.ga;
			cv[9] = tcf.g2a;
		}
		s.lv1 = lv1;
		s.lv2 = lv2;
		s.amp = amp;
		s.acc = acc;
		s.tn = tn;
		s.sc = sc;
		s.flut_ph = fph;
	}
	if (start) { // first block of a note from silence: no interpolation from stale control values
		for (int k = 0; k < 7; k++) s.ctrl_prev[k] = (k == 0 || k == 4) ? 0.0f : e.ctrl[0][k];
		for (int k = 7; k < NCV; k++) s.ctrl_prev[k] = e.ctrl[0][k];
	}

	// ---- audio rate: lips ------------------------------------------------------------------------
	{
		uint32_t pa = s.ph, pb = s.ph2, ia = s.inc, ib = s.inc2, nr = s.nrng;
		real sg = s.ctrl_prev[0], c = s.ctrl_prev[1], inv = s.ctrl_prev[2], mn = s.ctrl_prev[3], tnl = s.ctrl_prev[4];
		real inv2 = s.ctrl_prev[5], cb = s.ctrl_prev[6];
		const real br = P.breath;
		const real M32 = 2.3283064e-10f; // 2^-32
		real* __restrict__ src = e.src;
		for (int k = 0; k < NCTRL; k++) {
			const real* cur = e.ctrl[k];
			const real r = 1.0f / CTRL;
			const real dsg = (cur[0] - sg) * r, dc = (cur[1] - c) * r, dinv = (cur[2] - inv) * r;
			const real dmn = (cur[3] - mn) * r, dtn = (cur[4] - tnl) * r;
			const real dinv2 = (cur[5] - inv2) * r, dcb = (cur[6] - cb) * r;
			if (pair_on) {
#pragma GCC unroll 2
				for (int j = 0; j < CTRL; j++) {
					sg += dsg;
					c += dc;
					inv += dinv;
					mn += dmn;
					tnl += dtn;
					inv2 += dinv2;
					cb += dcb;
					ia += (uint32_t)dA;
					ib += (uint32_t)dB;
					pa += ia;
					pb += ib;
					real q = (real)(int32_t)(pa + 0x40000000u) * M32; // cos(2 pi phase) = sin(2 pi (phase + 1/4))
					real ps = 8.0f * q * (1.0f - 2.0f * fabsf(q));
					const real cs = ps * (0.775f + 0.225f * fabsf(ps));
					q = (real)(int32_t)(pb + 0x40000000u) * M32;
					ps = 8.0f * q * (1.0f - 2.0f * fabsf(q));
					const real cs2 = ps * (0.775f + 0.225f * fabsf(ps));
					const real h = rmax(cs - c, 0.0f), h2 = rmax(cs2 - cb, 0.0f);
					const real u = h * h * inv + h2 * h2 * inv2;
					nr = nr * 1664525u + 1013904223u;
					const real nz = (real)(int32_t)nr * 4.656613e-10f;
					src[k * CTRL + j] = sg * (u + br * u * nz - mn) + tnl * nz;
				}
			} else {
#pragma GCC unroll 2
				for (int j = 0; j < CTRL; j++) {
					sg += dsg;
					c += dc;
					inv += dinv;
					mn += dmn;
					tnl += dtn;
					inv2 += dinv2;
					cb += dcb;
					ia += (uint32_t)dA;
					pa += ia;
					const real q = (real)(int32_t)(pa + 0x40000000u) * M32;
					const real ps = 8.0f * q * (1.0f - 2.0f * fabsf(q));
					const real cs = ps * (0.775f + 0.225f * fabsf(ps));
					const real h = rmax(cs - c, 0.0f);
					const real u = h * h * inv;
					nr = nr * 1664525u + 1013904223u;
					const real nz = (real)(int32_t)nr * 4.656613e-10f;
					src[k * CTRL + j] = sg * (u + br * u * nz - mn) + tnl * nz;
				}
			}
			sg = cur[0];
			c = cur[1];
			inv = cur[2];
			mn = cur[3];
			tnl = cur[4];
			inv2 = cur[5];
			cb = cur[6];
		}
		if (!pair_on) {
			ib = inc2;
			pb += inc2 * (uint32_t)BLOCK;
		}
		s.ph = pa;
		s.ph2 = pb;
		s.inc = inc1;
		s.inc2 = inc2;
		s.nrng = nr;
		s.ctrl_prev[0] = sg;
		s.ctrl_prev[1] = c;
		s.ctrl_prev[2] = inv;
		s.ctrl_prev[3] = mn;
		s.ctrl_prev[4] = tnl;
		s.ctrl_prev[5] = inv2;
		s.ctrl_prev[6] = cb;
		(void)ib;
	}

	// ---- audio rate: bore, bell, body, mute ----------------------------------------------------
	float* B1 = bore_buf[v][0];
	float* B2 = bore_buf[v][1];
	// level keytracking: the radiation high-pass takes the fundamental of notes below rad_hz; give
	// part of it back ((1 + (rad_hz/f0)^2)^0.375) so low notes keep their weight
	const real rq = fr / fl;
	const real rx = 1.0f + rq * rq;
	const real rx4 = fsqrt(fsqrt(rx));
	const real gain = rmin(rmax(P.gain, 0.0f), 8.0f) * rmin(rx4 * fsqrt(rx4), 2.5f);
	audio_bore(e, s, B1, B2, d1t, d2t, ks, ks2, rl, g, Gb);
	if (mute_on) audio_body<true>(e, s, Gr, cupc, cupG, mc, mdep, wh, wb, wl, gain);
	else audio_body<false>(e, s, Gr, cupc, cupG, mc, mdep, wh, wb, wl, gain);

	// peak of this voice's block
	const real* __restrict__ out = e.vout;
	real peak = 0.0f;
	for (int n = 0; n < BLOCK; n++) peak = rmax(peak, fabsf(out[n]));
	// guard: non-finite or absurd state -> reset this voice and drop its block
	const real st = fabsf(s.lps) + fabsf(s.lpt) + fabsf(s.rs) + fabsf(s.c1) + fabsf(s.c2) + fabsf(s.t1) + fabsf(s.t2) + fabsf(s.m1) + fabsf(s.m2) + fabsf(s.amp) + fabsf(s.lv1) + fabsf(s.lv2) + fabsf(s.d1);
	if (!(peak < 1e5f) || !(st < 1e6f)) {
#ifdef BRASS_TEST_HOOK
		brass_guard_resets++;
		fprintf(stderr, "guard: voice %d peak %g st %g note %g pres %g amp %g d1 %g lps %g gain %g sg %g\n", v, peak, st, note, pres, s.amp, s.d1, s.lps, gain, e.ctrl[3][0]);
#endif
		reset_voice(s, v);
		s.active = 0;
		return;
	}
	real* __restrict__ mix = e.mix;
	for (int n = 0; n < BLOCK; n++) mix[n] += out[n];

	// go idle once released and faded out (about -100 dB); the state is kept
	if (!held && vplev <= 0.0f && s.amp < 1e-5f && s.acc < 1e-4f && peak < 1e-5f) s.active = 0;
}

[[nodiscard]] static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

static inline void process(Engine& e, real* __restrict__ output) {
#ifdef BRASS_TEST_SWITCH
	// host test: step to the next preset every BRASS_TEST_SWITCH blocks, while voices are sounding
	{
		static int nb = 0;
		if (++nb % BRASS_TEST_SWITCH == 0) set_preset(e, (e.preset + 1) % NUM_PRESETS);
	}
#endif
#ifdef BRASS_TEST_HOOK
	brass_test_hook(e); // host test (tools/harness/perf_brass.h): live parameter writes, extra voices
#endif
	// smooth the parameters towards the set values (live edits glide in ~13 ms instead of stepping)
	{
		const real* __restrict__ pt = reinterpret_cast<const real*>(&e.p);
		real* __restrict__ ps = reinterpret_cast<real*>(&e.ps);
		for (int i = 0; i < NPARAMS; i++) {
			const real d = pt[i] - ps[i];
			ps[i] = fabsf(d) < 1e-5f ? pt[i] : ps[i] + 0.05f * d;
		}
	}
	process_global(e);
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
// Live parameters (tools/app): every Params field, with ranges that are safe at both ends.
// Writes go to e.p; the DSP follows through the smoothed copy e.ps, so edits never step or click.

struct ParamEntry {
	strisy::ParamDesc d;
	real Params::*m;
};

static constexpr ParamEntry param_table[] = {
    // level
    {{"gain", "level", 0.1f, 6.0f, strisy::ParamKind::continuous}, &Params::gain},
    {{"pres_lin", "level", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::pc_a}, // level = pres*(pres_lin + pres_sq*pres)
    {{"pres_sq", "level", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::pc_b},
    {{"pres_hz", "level", 2.0f, 200.0f, strisy::ParamKind::continuous}, &Params::lv_hz},   // breath response speed
    {{"release", "level", 0.005f, 1.0f, strisy::ParamKind::continuous}, &Params::rel},     // lip stop time constant (s)
    {{"rel_pedal", "level", 0.0f, 3.0f, strisy::ParamKind::continuous}, &Params::rel_ped}, // + rel_pedal * pedal^2
    // lips
    {{"lip_soft", "lips", -2.0f, 0.8f, strisy::ParamKind::continuous}, &Params::lip_soft},  // lip closure when soft (-2 = pure)
    {{"lip_loud", "lips", -2.0f, 0.85f, strisy::ParamKind::continuous}, &Params::lip_loud}, // lip closure when loud (0.85 = buzzy)
    {{"lip_tilt", "lips", -0.5f, 0.5f, strisy::ParamKind::continuous}, &Params::lip_tilt},  // forward tilt brightens the lips
    {{"jitter", "lips", 0.0f, 20.0f, strisy::ParamKind::continuous}, &Params::jitter},      // cents
    {{"breath", "lips", 0.0f, 0.8f, strisy::ParamKind::continuous}, &Params::breath},       // breath turbulence
    // bore
    {{"brassiness", "bore", 0.0f, 60.0f, strisy::ParamKind::continuous}, &Params::brass},    // wave steepening (samples at ff)
    {{"bore_decay", "bore", 0.005f, 0.2f, strisy::ParamKind::continuous}, &Params::bore_t},  // resonance decay (s): bloom, tail
    {{"bell_hz", "bore", 150.0f, 4000.0f, strisy::ParamKind::continuous}, &Params::bell_hz}, // bell reflection cutoff
    // body
    {{"rad_hz", "body", 20.0f, 2000.0f, strisy::ParamKind::continuous}, &Params::rad_hz},  // radiation high-pass
    {{"cup_hz", "body", 150.0f, 3000.0f, strisy::ParamKind::continuous}, &Params::cup_hz}, // mouthpiece formant
    {{"cup_q", "body", 0.5f, 5.0f, strisy::ParamKind::continuous}, &Params::cup_q},
    {{"cup_gain", "body", 0.0f, 4.0f, strisy::ParamKind::continuous}, &Params::cup_g},
    {{"tone_hz", "body", 500.0f, 12000.0f, strisy::ParamKind::continuous}, &Params::tone_hz}, // bell low-pass at ff
    {{"tone_q", "body", 0.5f, 2.0f, strisy::ParamKind::continuous}, &Params::tone_q},
    {{"tone_dyn", "body", 0.0f, 4.0f, strisy::ParamKind::continuous}, &Params::tone_dyn},    // octaves darker when soft
    {{"tilt_tone", "body", -2.0f, 2.0f, strisy::ParamKind::continuous}, &Params::tilt_tone}, // tilt: octaves (back = darker/closed)
    // mute
    {{"mute", "mute", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::mute}, // depth
    {{"mute_hz", "mute", 300.0f, 5000.0f, strisy::ParamKind::continuous}, &Params::mute_hz},
    {{"mute_q", "mute", 0.5f, 10.0f, strisy::ParamKind::continuous}, &Params::mute_q},
    {{"mute_hp", "mute", 0.0f, 3.0f, strisy::ParamKind::continuous}, &Params::mute_hp}, // straight-mute character
    {{"mute_bp", "mute", 0.0f, 4.0f, strisy::ParamKind::continuous}, &Params::mute_bp}, // harmon character
    {{"mute_lp", "mute", 0.0f, 2.0f, strisy::ParamKind::continuous}, &Params::mute_lp},
    {{"wah", "mute", -2.0f, 2.0f, strisy::ParamKind::continuous}, &Params::wah}, // tilt: mute resonance octaves
    // attack
    {{"tongue", "attack", 0.0f, 2.0f, strisy::ParamKind::continuous}, &Params::tongue},     // strike sensitivity
    {{"acc_level", "attack", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::acc_amp}, // accent overshoot
    {{"acc_decay", "attack", 0.01f, 0.3f, strisy::ParamKind::continuous}, &Params::acc_t},
    {{"acc_bright", "attack", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::acc_bright},
    {{"scoop", "attack", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::scoop},     // semitones from below
    {{"blat", "attack", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::blat},       // attack buzz (semitones)
    {{"t_noise", "attack", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::t_noise}, // tongue "t" noise
    {{"slur", "attack", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::slur},       // articulation on legato changes
    // modulation
    {{"vibrato", "mod", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::vib}, // delayed lip vibrato (semitones)
    {{"vib_hz", "mod", 2.0f, 9.0f, strisy::ParamKind::continuous}, &Params::vib_hz},
    {{"flutter", "mod", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::flutter}, // shake -> flutter tongue
    {{"flut_hz", "mod", 8.0f, 40.0f, strisy::ParamKind::continuous}, &Params::flut_hz},
    // section
    {{"pair", "section", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::pair},      // second player level
    {{"detune", "section", 0.0f, 30.0f, strisy::ParamKind::continuous}, &Params::detune}, // cents
};
static constexpr int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

} // namespace brass

namespace strisy {
template <> struct EngineParams<brass::Engine> {
	static constexpr int count = brass::NUM_LIVE_PARAMS;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &brass::param_table[i].d : nullptr; }
	static float* ptr(brass::Engine& e, int i) { return (i >= 0 && i < count) ? &(e.p.*(brass::param_table[i].m)) : nullptr; }
	static void changed(brass::Engine& e, int) { brass::update_derived(e); }
};
} // namespace strisy

#endif // VOICE_BRASS_H_
