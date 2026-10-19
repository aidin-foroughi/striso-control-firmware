/**
 * va: expressive virtual analog voice. Build with `make VOICE=va`.
 *
 * Per voice:
 *   oscillators: A = band-limited saw + pulse (PWM) + square sub one octave down (all from one phase)
 *                B = saw hard-synced to A (ratio swept by forward tilt, pressure and strike), or in
 *                    some presets a free running saw detuned downwards (three-saw ensemble)
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
 * The sound design constants are in struct Params (e.p); presets are Params sets (set_preset).
 * Preset 0 "classic" is the original voice, bit-identical.
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

// Sound design parameters. Notation: y = smoothed forward/back tilt (+ y_off), yn = max(-y, 0)
// (tilted back), yp = max(y, 0) (tilted forward); b = brightness 0..1.1 from the level; acc = strike
// accent 0..0.7; semitone units for pitch and cutoff.
struct Params {
	// level
	real gain;            // output level
	real pc_a, pc_b;      // pressure -> level target: pres * (pc_a + pc_b * pres)
	real lv_hz;           // pressure smoothing (critically damped 2-pole): lower = softer attack
	real held_tc;         // fastest level decay while held (s): a quick lift leaves a tail
	real rel_min, rel_ped; // release time constant (s): rel_min + rel_ped * pedal^2
	real rel_vneg;        // a fast release divides the release time by (1 + rel_vneg * |vpres|)
	// strike accent
	real acc_amp;         // added to the level target
	real acc_t0, acc_key; // decay time constant (s): acc_t0 * 2^((132 - note) * acc_key)
	real acc_cut;         // cutoff boost (semitones)
	real acc_drive;       // extra saturation drive
	real acc_sync;        // extra sync sweep (semitones, scaled by yp)
	real acc_pitch;       // pitch offset at the strike (semitones, decays with the accent)
	// filter
	real cut_key, cut_base; // closed cutoff (as a note number): cut_key * note + cut_base
	real cut_env;         // cutoff opening by brightness b (semitones)
	real cut_drift;       // cutoff drift depth (semitones per unit drift)
	real cut_yn, cut_yp;  // cutoff lowered by back tilt, raised by forward tilt
	real res_base, res_yn, res_yp; // resonance k = 4 * (res_base + res_yn * yn - res_yp * yp)
	real bass_comp;       // ladder input gain 1 + bass_comp * k (resonance loudness compensation)
	real dr_base, dr_env; // post-filter drive dr_base + dr_env * b (+ acc_drive * acc)
	// tilt
	real y_off;           // home position on the tilt morph (added to the tilt, clamped to +-1)
	// oscillator mix
	real saw, saw_yn, saw_yp;     // saw A: saw * (1 - saw_yn * yn) * (1 - saw_yp * yp)
	real c_lvl, c_yn, c_yp;       // detuned saw C: c_lvl * (1 - c_yn * yn) * (1 - c_yp * yp)
	real c_det, c_drift, gyro_det; // C detune (semitones), its drift, extra detune from rotation
	real pul0, pul_yn;            // pulse A: pul0 + pul_yn * yn
	real pwm_depth, pwm_hz;       // pulse width 0.5 +- pwm_depth, LFO rate
	real sub, sub0, sub_yn, sub_yp; // sub: sub * (sub0 + sub_yn * yn) * (1 - sub_yp * yp)
	real b_sync;                  // 1: B hard-synced to A; 0: B free running, detuned by -b_det
	real b_lvl0, b_yp;            // B level: b_lvl0 + b_yp * yp
	real b_det;                   // B detune when free running (semitones, downwards)
	real sync0, sync_yp, sync_env; // sync ratio (semitones): yp * (sync0 + sync_yp * yp + sync_env * b + acc_sync * acc)
	// modulation
	real drift_a;         // pitch drift of A (semitones per unit drift)
	real vib_depth, vib_hz; // vibrato from shaking (semitones at full shake)
};

static const int NPARAMS = sizeof(Params) / sizeof(real);
static_assert(sizeof(Params) == NPARAMS * sizeof(real), "Params must hold only floats");

// preset 0: the original voice
static inline Params classic_params() {
	Params p;
	p.gain = 2.3f; p.pc_a = 0.3f; p.pc_b = 0.7f; p.lv_hz = 70.0f; p.held_tc = 0.045f;
	p.rel_min = 0.10f; p.rel_ped = 2.9f; p.rel_vneg = 3.0f;
	p.acc_amp = 0.75f; p.acc_t0 = 0.04f; p.acc_key = 1.0f / 36; p.acc_cut = 28.0f; p.acc_drive = 0.7f;
	p.acc_sync = 10.0f; p.acc_pitch = 0.0f;
	p.cut_key = 0.75f; p.cut_base = 26.0f; p.cut_env = 46.0f; p.cut_drift = 0.3f; p.cut_yn = 9.0f; p.cut_yp = 5.0f;
	p.res_base = 0.2f; p.res_yn = 0.72f; p.res_yp = 0.08f; p.bass_comp = 0.45f;
	p.dr_base = 0.6f; p.dr_env = 0.9f;
	p.y_off = 0.0f;
	p.saw = 0.5f; p.saw_yn = 0.75f; p.saw_yp = 0.65f;
	p.c_lvl = 0.36f; p.c_yn = 0.5f; p.c_yp = 0.6f; p.c_det = 0.07f; p.c_drift = 0.025f; p.gyro_det = 0.22f;
	p.pul0 = 0.0f; p.pul_yn = 0.45f; p.pwm_depth = 0.13f; p.pwm_hz = 0.37f;
	p.sub = 0.5f; p.sub0 = 0.2f; p.sub_yn = 0.25f; p.sub_yp = 0.6f;
	p.b_sync = 1.0f; p.b_lvl0 = 0.0f; p.b_yp = 0.5f; p.b_det = 0.0f;
	p.sync0 = 5.0f; p.sync_yp = 12.0f; p.sync_env = 9.0f;
	p.drift_a = 0.03f; p.vib_depth = 0.35f; p.vib_hz = 5.3f;
	return p;
}

static const int NUM_PRESETS = 8;

static inline const char* preset_name(int i) {
	static const char* const names[NUM_PRESETS] = {"classic", "pad", "lead", "bass", "brass", "keys", "acid", "strings"};
	return (i >= 0 && i < NUM_PRESETS) ? names[i] : "classic";
}

static inline Params preset_params(int i) {
	Params p = classic_params();
	switch (i) {
	case 1:  // pad: three detuned saws + PWM, slow swell from pressure, long release, wide chorus
		p.gain = 3.6f; p.pc_a = 0.45f; p.pc_b = 0.55f; p.lv_hz = 3.0f; p.held_tc = 0.35f;
		p.rel_min = 0.9f; p.rel_ped = 3.5f; p.rel_vneg = 1.0f;
		p.acc_amp = 0.12f; p.acc_t0 = 0.08f; p.acc_cut = 8.0f; p.acc_drive = 0.2f;
		p.cut_key = 0.6f; p.cut_base = 40.0f; p.cut_env = 34.0f; p.cut_yn = 6.0f; p.cut_yp = 8.0f;
		p.res_base = 0.12f; p.res_yn = 0.45f; p.res_yp = 0.05f;
		p.dr_base = 0.45f; p.dr_env = 0.5f;
		p.saw = 0.32f; p.saw_yn = 0.5f; p.saw_yp = 0.3f;
		p.c_lvl = 0.3f; p.c_yn = 0.3f; p.c_yp = 0.3f; p.c_det = 0.11f; p.c_drift = 0.04f; p.gyro_det = 0.35f;
		p.pul0 = 0.12f; p.pul_yn = 0.3f; p.pwm_depth = 0.2f; p.pwm_hz = 0.5f;
		p.sub = 0.5f; p.sub0 = 0.1f; p.sub_yn = 0.25f; p.sub_yp = 0.5f;
		p.b_sync = 0.0f; p.b_lvl0 = 0.28f; p.b_yp = 0.1f; p.b_det = 0.1f;
		p.drift_a = 0.05f; p.vib_depth = 0.25f; p.vib_hz = 4.6f;
		break;
	case 2:  // lead: sync and drive at rest, fast response, strong vibrato from shaking
		p.gain = 2.4f; p.pc_a = 0.35f; p.pc_b = 0.65f; p.lv_hz = 120.0f; p.held_tc = 0.03f;
		p.rel_min = 0.08f; p.rel_ped = 1.5f;
		p.acc_amp = 0.5f; p.acc_t0 = 0.03f; p.acc_cut = 20.0f; p.acc_drive = 0.9f; p.acc_sync = 14.0f;
		p.acc_pitch = -0.15f;
		p.cut_key = 0.7f; p.cut_base = 36.0f; p.cut_env = 40.0f; p.cut_yn = 6.0f; p.cut_yp = 6.0f;
		p.res_base = 0.3f; p.res_yn = 0.5f; p.res_yp = 0.1f;
		p.dr_base = 1.1f; p.dr_env = 1.3f;
		p.y_off = 0.35f;
		p.c_lvl = 0.3f; p.c_det = 0.05f;
		p.sub = 0.5f; p.sub0 = 0.12f;
		p.sync0 = 7.0f; p.sync_yp = 14.0f; p.sync_env = 12.0f;
		p.vib_depth = 0.6f; p.vib_hz = 5.6f;
		break;
	case 3:  // bass: heavy sub, tight keytracked filter, punchy strike decay, short release
		p.gain = 2.1f; p.pc_a = 0.4f; p.pc_b = 0.6f; p.lv_hz = 90.0f; p.held_tc = 0.03f;
		p.rel_min = 0.06f; p.rel_ped = 1.2f; p.rel_vneg = 4.0f;
		p.acc_amp = 0.9f; p.acc_t0 = 0.018f; p.acc_key = 1.0f / 30; p.acc_cut = 34.0f; p.acc_drive = 0.9f;
		p.cut_key = 0.4f; p.cut_base = 42.0f; p.cut_env = 36.0f; p.cut_drift = 0.2f; p.cut_yn = 8.0f; p.cut_yp = 6.0f;
		p.res_base = 0.22f; p.res_yn = 0.6f;
		p.dr_base = 0.8f; p.dr_env = 1.0f;
		p.saw = 0.45f; p.c_lvl = 0.2f; p.c_det = 0.04f;
		p.sub = 0.5f; p.sub0 = 0.75f; p.sub_yn = 0.15f; p.sub_yp = 0.4f;
		p.sync0 = 3.0f; p.sync_yp = 12.0f; p.sync_env = 6.0f;
		p.drift_a = 0.02f; p.vib_depth = 0.2f;
		break;
	case 4:  // brass: cutoff follows pressure closely, a bite and a slight scoop on strikes
		p.gain = 2.65f; p.pc_a = 0.2f; p.pc_b = 0.8f; p.lv_hz = 40.0f; p.held_tc = 0.06f;
		p.rel_min = 0.12f; p.rel_ped = 2.0f;
		p.acc_amp = 0.35f; p.acc_t0 = 0.022f; p.acc_cut = 22.0f; p.acc_drive = 1.0f; p.acc_pitch = -0.35f;
		p.cut_key = 0.85f; p.cut_base = 12.0f; p.cut_env = 62.0f; p.cut_yn = 8.0f; p.cut_yp = 6.0f;
		p.res_base = 0.15f; p.res_yn = 0.55f;
		p.dr_base = 0.7f; p.dr_env = 1.2f;
		p.saw = 0.5f; p.c_lvl = 0.4f; p.c_det = 0.06f;
		p.pul0 = 0.1f; p.sub0 = 0.12f;
		p.vib_depth = 0.4f; p.vib_hz = 5.0f;
		break;
	case 5:  // keys: the strike sets a decaying note (like an electric piano), pressure sustains softly
		p.gain = 3.8f; p.pc_a = 0.15f; p.pc_b = 0.45f; p.lv_hz = 110.0f; p.held_tc = 0.06f;
		p.rel_min = 0.22f; p.rel_ped = 3.0f; p.rel_vneg = 2.0f;
		p.acc_amp = 1.5f; p.acc_t0 = 0.1f; p.acc_key = 1.0f / 28; p.acc_cut = 30.0f; p.acc_drive = 0.5f;
		p.cut_key = 0.7f; p.cut_base = 30.0f; p.cut_env = 34.0f; p.cut_yn = 8.0f; p.cut_yp = 8.0f;
		p.res_base = 0.15f; p.res_yn = 0.6f;
		p.dr_base = 0.5f; p.dr_env = 0.7f;
		p.saw = 0.35f; p.c_lvl = 0.25f; p.c_det = 0.05f;
		p.pul0 = 0.3f; p.pwm_depth = 0.05f; p.sub0 = 0.15f;
		p.vib_depth = 0.3f;
		break;
	case 6:  // acid: resonant squelch, strikes spike the cutoff, tilt back = screaming resonance
		p.gain = 2.3f; p.pc_a = 0.4f; p.pc_b = 0.6f; p.lv_hz = 100.0f; p.held_tc = 0.03f;
		p.rel_min = 0.07f; p.rel_ped = 1.5f;
		p.acc_amp = 0.6f; p.acc_t0 = 0.025f; p.acc_cut = 40.0f; p.acc_drive = 1.2f; p.acc_sync = 0.0f;
		p.cut_key = 0.5f; p.cut_base = 34.0f; p.cut_env = 44.0f; p.cut_yn = 10.0f; p.cut_yp = 14.0f;
		p.res_base = 0.62f; p.res_yn = 0.33f; p.res_yp = 0.3f; p.bass_comp = 0.5f;
		p.dr_base = 1.0f; p.dr_env = 1.0f;
		p.saw = 0.55f; p.saw_yn = 0.4f; p.saw_yp = 0.0f;
		p.c_lvl = 0.0f; p.pul_yn = 0.5f; p.sub0 = 0.1f; p.sub_yp = 0.3f;
		p.b_yp = 0.0f; p.sync0 = 0.0f; p.sync_yp = 0.0f; p.sync_env = 0.0f;
		p.drift_a = 0.02f;
		break;
	case 7:  // strings: ensemble of three saws, bowed swell from pressure, rotation for chorus
		p.gain = 3.1f; p.pc_a = 0.25f; p.pc_b = 0.75f; p.lv_hz = 9.0f; p.held_tc = 0.2f;
		p.rel_min = 0.45f; p.rel_ped = 3.0f; p.rel_vneg = 2.0f;
		p.acc_amp = 0.2f; p.acc_t0 = 0.05f; p.acc_cut = 12.0f; p.acc_drive = 0.3f;
		p.cut_key = 0.8f; p.cut_base = 22.0f; p.cut_env = 50.0f; p.cut_yn = 8.0f; p.cut_yp = 10.0f;
		p.res_base = 0.08f; p.res_yn = 0.4f; p.res_yp = 0.0f;
		p.dr_base = 0.5f; p.dr_env = 0.6f;
		p.saw = 0.32f; p.saw_yn = 0.4f; p.saw_yp = 0.2f;
		p.c_lvl = 0.32f; p.c_yn = 0.3f; p.c_yp = 0.2f; p.c_det = 0.09f; p.c_drift = 0.04f; p.gyro_det = 0.3f;
		p.pul_yn = 0.35f; p.pwm_depth = 0.18f;
		p.sub0 = 0.05f; p.sub_yn = 0.2f;
		p.b_sync = 0.0f; p.b_lvl0 = 0.3f; p.b_yp = 0.1f; p.b_det = 0.13f;
		p.drift_a = 0.04f; p.vib_depth = 0.3f; p.vib_hz = 5.2f;
		break;
	default:
		break;
	}
	return p;
}

#ifndef VA_PRESET
#define VA_PRESET 0
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
	int preset;
	Params p;                   // parameters as set (presets, live edits)
	Params ps;                  // smoothed copy used by the DSP (de-zippers live edits, ~13 ms)
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

static inline real note_freq(real note) { return 8.1757989f * strisy::fastpow2(rmax(note, 0.0f) * (1.0f / 12)); }

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

// Select a preset; callable between blocks at any time (after init). Sounding voices continue with
// the new parameters: every state stays bounded for any preset, so no reset is needed.
// values derived from the parameters (rates are not smoothed: they cannot click)
static inline void update_derived(Engine& e) {
	e.vib_inc = e.p.vib_hz * e.block_dt;
	e.pwm_inc = e.p.pwm_hz * e.block_dt;
	e.lv_c = strisy::svf_from_g(strisy::tan0(e.pi_sr * CTRL * e.p.lv_hz), 0.5f);   // critically damped
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
	e.pi_sr = 3.14159265f / sr;
	e.ph_scale = 4294967296.0f / sr;
	e.ctrl_dt = (real)CTRL / sr;
	e.block_dt = (real)BLOCK / sr;
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
	set_preset(e, VA_PRESET);
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

#ifdef VA_TEST_HOOK
static int va_guard_resets = 0;      // host test: voices reset by the guard
static void va_test_hook(Engine& e);
#endif

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const Params& P = e.ps;
	const strisy::VoiceControls c = e.vc[v];
	const real pres = rmin(rmax(c.pres, 0.0f), 1.5f);
	// after a release the firmware leaves the last vpres in place: a positive leftover (sensor noise)
	// must not strike or hold a released voice; negative values still set the release speed
	const real vpres = rmin(rmax(c.vpres, -1.0f), pres > 0.0f ? 1.0f : 0.0f);
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
	y = rmin(rmax(y + P.y_off, -1.0f), 1.0f);
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
	const real vib = P.vib_depth * e.shake * psin(s.vib_ph);

	// brightness of the previous block (for the sync sweep, which is pitch: set per block)
	const real bl0 = rmin(s.amp, 1.5f);
	const real b0 = bl0 * (1.5f - 0.5f * bl0);

	// pitches
	const real nA = note + P.drift_a * drift[0] + vib + P.acc_pitch * s.acc;
#ifdef VA_TEST_CLEAN
	const real nC = nA;
#else
	const real nC = nA + P.c_det + P.c_drift * drift[1] + P.gyro_det * e.gyro;
#endif
	const bool bsync = P.b_sync > 0.5f;
	const real fmaxA = 0.45f * 48828.0f, fmaxB = 0.35f * 48828.0f;
	const real fA = rmin(note_freq(nA), fmaxA);
	real fB;
	if (bsync) {
		const real sync_st = yp * (P.sync0 + P.sync_yp * yp + P.sync_env * b0 + P.acc_sync * s.acc);
		fB = rmin(fA * strisy::fastpow2(sync_st * (1.0f / 12)), fmaxB);
	} else {
#ifdef VA_TEST_CLEAN
		fB = fA;
#else
		fB = rmin(note_freq(nA - P.b_det - P.c_drift * drift[1] - P.gyro_det * e.gyro), fmaxA);
#endif
	}
	const real fC = rmin(note_freq(nC), fmaxA);
	const uint32_t iA1 = (uint32_t)(fA * e.ph_scale) | 1u;
	const uint32_t iB1 = (uint32_t)(fB * e.ph_scale) | 1u;
	const uint32_t iC1 = (uint32_t)(fC * e.ph_scale) | 1u;
	if (jump || s.incA == 0) { s.incA = iA1; s.incB = iB1; s.incC = iC1; }
	const int32_t dA = (int32_t)(iA1 - s.incA) / BLOCK;
	const int32_t dB = (int32_t)(iB1 - s.incB) / BLOCK;
	const int32_t dC = (int32_t)(iC1 - s.incC) / BLOCK;
	const real invA = 1.0f / (real)iA1, invB = 1.0f / (real)iB1, invC = 1.0f / (real)iC1;

	// oscillator mix (timbre morph by tilt): in classic, back = hollow PWM square + sub, centre =
	// two detuned saws + sub, forward = sync lead
	const real lSaw = P.saw * (1.0f - P.saw_yn * yn) * (1.0f - P.saw_yp * yp);
	const real lC = P.c_lvl * (1.0f - P.c_yn * yn) * (1.0f - P.c_yp * yp);
	const real lPul = P.pul0 + P.pul_yn * yn;
	const real lSub = P.sub * (P.sub0 + P.sub_yn * yn) * (1.0f - P.sub_yp * yp);
	const real lB = P.b_lvl0 + P.b_yp * yp;
	const real pwf = 0.5f + P.pwm_depth * psin(s.pwm_ph);
	const uint32_t pw = (uint32_t)(pwf * 4294967296.0f);
	const real M32 = 2.3283064e-10f;   // 2^-32
	const real cA = 2.0f * lSaw * M32, cB = 2.0f * lB * M32, cC = 2.0f * lC * M32;
	const real K0 = -(lSaw + lB + lC) - lPul * (2.0f * pwf - 1.0f);   // zero-mean pulse
	const real hA = -2.0f * lSaw + 2.0f * lPul;   // saw wrap + pulse rising edge
	const real hB = -2.0f * lB, hC = -2.0f * lC, hP = -2.0f * lPul;

	// filter resonance and input gain (partial bass compensation)
	const real kres = rmin(rmax(4.0f * (P.res_base + P.res_yn * yn - P.res_yp * yp), 0.0f), 3.95f);
	const real din = 1.0f + P.bass_comp * kres;

	// envelope rates
	if (vpres > 0.1f) s.vneg = 0.0f;
	if (held && vpres < 0.0f) s.vneg = rmax(s.vneg, -vpres);
	const real ped = rmin(rmax(e.g.pedal, 0.0f), 1.0f);
	const real rel_tc = (P.rel_min + P.rel_ped * ped * ped) / (1.0f + P.rel_vneg * s.vneg);
	const real fac = 1.0f - e.ctrl_dt / (held ? P.held_tc : rel_tc);
	const real acc_tc = P.acc_t0 * strisy::fastpow2((132.0f - note) * P.acc_key);   // 0.16 s at note 60 (classic)
	const real fac_acc = 1.0f - e.ctrl_dt / acc_tc;
	const real pc = pres * (P.pc_a + P.pc_b * pres);
	const real cut0 = P.cut_key * note + P.cut_base + P.cut_drift * drift[2] - P.cut_yn * yn + P.cut_yp * yp;

	// ---- control rate: level, accent, cutoff -> ladder coefficients ---------------------------
	{
		real lv1 = s.lv1, lv2 = s.lv2, amp = s.amp, acc = s.acc;
		const strisy::SvfCoef lc = e.lv_c;
		const real pi_sr = e.pi_sr;
		const real acc_amp = P.acc_amp, cut_env = P.cut_env, acc_cut = P.acc_cut;
		const real dr_base = P.dr_base, dr_env = P.dr_env, acc_drive = P.acc_drive, gain = P.gain;
		for (int k = 0; k < NCTRL; k++) {
			acc = rmax(acc * fac_acc, vplev);
			real t1, ls;
			SVF_TICK(pc + acc_amp * acc, lv1, lv2, lc, t1, ls);
			amp = rmax(rmax(ls, 0.0f), amp * fac);
			const real bl = rmin(amp, 1.5f);
			const real b = bl * (1.5f - 0.5f * bl);
			const real cut = rmin(cut0 + cut_env * b + acc_cut * acc, 128.0f);   // (note_freq clamps at 0)
			const real f = rmin(note_freq(cut), 16000.0f);
			// G = g/(1+g), g = tan(x) as the [7/6] Pade of strisy::svf_pade, one division
			const real x = pi_sr * f, x2 = x * x;
			const real gN = x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2)));
			const real D = 135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2));
			const real G = gN / (D + gN);
			const real G2 = G * G;
			const real dr = dr_base + dr_env * b + acc_drive * acc;
			real* cv = e.ctrl[k];
			cv[0] = G;
			cv[1] = 1.0f / (1.0f + kres * G2 * G2);
			cv[2] = dr;
			cv[3] = gain * amp / (0.5f + 0.5f * dr);
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
			bool b_reset = false;
			// polyBLEP for a jump h at time d (in samples) before the current sample:
			// previous sample += h/2 d^2, current sample -= h/2 (1-d)^2
			if (pa < ia) {   // A wrapped: saw, pulse rise, sub flip (and B's sync reset) coincide
				const real d = rmin((real)pa * invA, 1.0f);
				sub = -sub;
				real h = hA + 2.0f * lSub * sub;
				if (bsync) {
					const uint32_t bat = pb + (uint32_t)((1.0f - d) * (real)ib);  // B's phase at the reset
					if (bat < pb) {   // B wrapped by itself just before the reset
						const real dw = rmin(d + (real)bat * invB, 1.0f), ew = 1.0f - dw;
						prev += 0.5f * hB * dw * dw;
						cur -= 0.5f * hB * ew * ew;
					}
					h -= 2.0f * lB * ((real)bat * M32);
					pb = (uint32_t)(d * (real)ib);
					b_reset = true;
				}
				const real e1 = 1.0f - d;
				prev += 0.5f * h * d * d;
				cur -= 0.5f * h * e1 * e1;
			}
			if (!b_reset) {
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
#ifdef VA_TEST_HOOK
		va_guard_resets++;
#endif
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
#ifdef VA_TEST_SWITCH
	// host test: step to the next preset every VA_TEST_SWITCH blocks, while voices are sounding
	{ static int nb = 0; if (++nb % VA_TEST_SWITCH == 0) set_preset(e, (e.preset + 1) % NUM_PRESETS); }
#endif
#ifdef VA_TEST_HOOK
	va_test_hook(e);   // host test (tools/harness/perf_va.h): live parameter writes, extra voices
#endif
	// smooth the parameters towards the set values (live edits glide in ~13 ms instead of stepping;
	// without edits ps == p exactly)
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

// ---------------------------------------------------------------------------------------------
// Live parameters (tools/app): every Params field, with ranges that are safe at both ends.
// Writes go to e.p; the DSP follows through the smoothed copy e.ps, so edits never step or click.

struct ParamEntry {
	strisy::ParamDesc d;
	real Params::* m;
};

static const ParamEntry param_table[] = {
	// level: pressure to loudness
	{{"gain", "level", 0.5f, 5.0f, 0}, &Params::gain},
	{{"pres_lin", "level", 0.0f, 1.0f, 0}, &Params::pc_a},          // level = pres*(pres_lin + pres_sq*pres)
	{{"pres_sq", "level", 0.0f, 1.5f, 0}, &Params::pc_b},
	{{"pres_hz", "level", 1.0f, 200.0f, 0}, &Params::lv_hz},         // pressure smoothing: low = slow swell
	{{"hold_decay", "level", 0.005f, 1.0f, 0}, &Params::held_tc},    // fastest fall while held (s)
	// release
	{{"release", "release", 0.02f, 4.0f, 0}, &Params::rel_min},      // tail time constant (s)
	{{"rel_pedal", "release", 0.0f, 6.0f, 0}, &Params::rel_ped},     // + rel_pedal * pedal^2
	{{"rel_speed", "release", 0.0f, 8.0f, 0}, &Params::rel_vneg},    // fast lift shortens the tail
	// accent (strike)
	{{"acc_level", "accent", 0.0f, 2.5f, 0}, &Params::acc_amp},
	{{"acc_decay", "accent", 0.005f, 0.4f, 0}, &Params::acc_t0},     // s at the top note
	{{"acc_key", "accent", 0.0f, 0.06f, 0}, &Params::acc_key},       // longer decay low: 2^((132-note)*acc_key)
	{{"acc_cutoff", "accent", 0.0f, 48.0f, 0}, &Params::acc_cut},    // semitones
	{{"acc_drive", "accent", 0.0f, 2.0f, 0}, &Params::acc_drive},
	{{"acc_sync", "accent", 0.0f, 24.0f, 0}, &Params::acc_sync},     // semitones (with forward tilt)
	{{"acc_pitch", "accent", -1.0f, 1.0f, 0}, &Params::acc_pitch},   // semitones, decays with the accent
	// filter
	{{"cut_base", "filter", -20.0f, 80.0f, 0}, &Params::cut_base},   // closed cutoff, note number
	{{"cut_key", "filter", 0.0f, 1.2f, 0}, &Params::cut_key},        // keytracking
	{{"cut_env", "filter", 0.0f, 80.0f, 0}, &Params::cut_env},       // opening by pressure (semitones)
	{{"cut_back", "filter", -24.0f, 24.0f, 0}, &Params::cut_yn},     // lowered by back tilt
	{{"cut_fwd", "filter", -24.0f, 24.0f, 0}, &Params::cut_yp},      // raised by forward tilt
	{{"res", "filter", 0.0f, 0.98f, 0}, &Params::res_base},          // 0.98 ~ self-oscillation
	{{"res_back", "filter", -0.5f, 0.98f, 0}, &Params::res_yn},
	{{"res_fwd", "filter", -0.5f, 0.98f, 0}, &Params::res_yp},       // subtracted with forward tilt
	{{"bass_comp", "filter", 0.0f, 1.0f, 0}, &Params::bass_comp},
	{{"drive", "filter", 0.1f, 3.0f, 0}, &Params::dr_base},
	{{"drive_env", "filter", 0.0f, 3.0f, 0}, &Params::dr_env},
	// oscillators (back/fwd: how much tilting back/forward reduces or adds)
	{{"saw", "osc", 0.0f, 1.0f, 0}, &Params::saw},
	{{"saw_back", "osc", 0.0f, 1.0f, 0}, &Params::saw_yn},
	{{"saw_fwd", "osc", 0.0f, 1.0f, 0}, &Params::saw_yp},
	{{"saw2", "osc", 0.0f, 1.0f, 0}, &Params::c_lvl},
	{{"saw2_back", "osc", 0.0f, 1.0f, 0}, &Params::c_yn},
	{{"saw2_fwd", "osc", 0.0f, 1.0f, 0}, &Params::c_yp},
	{{"saw2_detune", "osc", -0.5f, 0.5f, 0}, &Params::c_det},         // semitones
	{{"pulse", "osc", 0.0f, 1.0f, 0}, &Params::pul0},
	{{"pulse_back", "osc", 0.0f, 1.0f, 0}, &Params::pul_yn},
	{{"pwm", "osc", 0.0f, 0.4f, 0}, &Params::pwm_depth},
	{{"pwm_hz", "osc", 0.05f, 8.0f, 0}, &Params::pwm_hz},
	{{"sub", "osc", 0.0f, 1.0f, 0}, &Params::sub},
	{{"sub_base", "osc", 0.0f, 1.0f, 0}, &Params::sub0},
	{{"sub_back", "osc", 0.0f, 1.0f, 0}, &Params::sub_yn},
	{{"sub_fwd", "osc", 0.0f, 1.0f, 0}, &Params::sub_yp},
	{{"b_sync", "osc", 0.0f, 1.0f, 2}, &Params::b_sync},              // B synced to A, or free detuned saw
	{{"b_level", "osc", 0.0f, 1.0f, 0}, &Params::b_lvl0},
	{{"b_fwd", "osc", 0.0f, 1.0f, 0}, &Params::b_yp},
	{{"b_detune", "osc", -0.5f, 0.5f, 0}, &Params::b_det},            // semitones down (free mode)
	{{"sync", "osc", 0.0f, 24.0f, 0}, &Params::sync0},                // sync ratio, semitones (x fwd tilt)
	{{"sync_fwd", "osc", 0.0f, 24.0f, 0}, &Params::sync_yp},
	{{"sync_env", "osc", 0.0f, 24.0f, 0}, &Params::sync_env},         // sync sweep by pressure
	{{"tilt_home", "osc", -1.0f, 1.0f, 0}, &Params::y_off},           // rest position on the tilt morph
	// modulation
	{{"drift", "mod", 0.0f, 0.3f, 0}, &Params::drift_a},              // semitones
	{{"saw2_drift", "mod", 0.0f, 0.3f, 0}, &Params::c_drift},
	{{"cut_drift", "mod", 0.0f, 2.0f, 0}, &Params::cut_drift},
	{{"gyro_chorus", "mod", 0.0f, 1.0f, 0}, &Params::gyro_det},       // detune from rotation
	{{"vibrato", "mod", 0.0f, 1.5f, 0}, &Params::vib_depth},          // shake vibrato, semitones
	{{"vib_hz", "mod", 1.0f, 9.0f, 0}, &Params::vib_hz},
};
static const int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

} // namespace va

namespace strisy {
template <> struct EngineParams<va::Engine> {
	static const int count = va::NUM_LIVE_PARAMS;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &va::param_table[i].d : 0; }
	static float* ptr(va::Engine& e, int i) { return (i >= 0 && i < count) ? &(e.p.*(va::param_table[i].m)) : 0; }
	static void changed(va::Engine& e, int) { va::update_derived(e); }
};
} // namespace strisy

#endif // VOICE_VA_H_
