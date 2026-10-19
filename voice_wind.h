/**
 * Wind: physically modelled woodwinds with presets (make VOICE=wind, -DWIND_PRESET=n on the host).
 * API: see the comment at the top of synth_strisy.cpp; live parameters: strisy::EngineParams.
 *
 * Presets (realistic separate instruments, level matched):
 *   0 clarinet       B-flat clarinet: hollow chalumeau (odd harmonics), clear clarion
 *   1 bass clarinet  darker, woody, an octave lower
 *   2 soprano sax    bright and focused
 *   3 alto sax       the classic sax: rich, buzzy, formants ~650 Hz and ~2.5 kHz
 *   4 tenor sax      warmer and fatter, formants ~500 Hz / 2 kHz
 *   5 baritone sax   big, dark and gruff
 *   6 oboe           nasal double reed, formants ~1.25 and 3 kHz
 *   7 bassoon        double reed, dark and reedy, formants ~480 Hz / 1.15 kHz
 *   8 flute          air jet on an open pipe: nearly sinusoidal, breathy
 *
 * Three excitations / bores, chosen by the preset:
 *   cylinder (clarinets): a pressure-controlled single reed on one waveguide (closed at the reed,
 *            open at the bell): odd harmonics below the tone-hole lattice cutoff, weak above it.
 *   cone (saxophones, oboe, bassoon): the same reed on a truncated cone, modelled as the main
 *            tube in parallel with a short open tube standing for the missing apex (the
 *            "cylindrical saxophone"). The apex has a fixed length per instrument, so its share
 *            of the period grows towards high notes as on the real instrument; its reflection is
 *            lowpassed (3 f0), which evens out the cone's impedance peaks and keeps the low notes
 *            in their register even when blown hard.
 *   jet (flute): an air jet deflected by the acoustic flow at the mouth hole, travelling half a
 *            period to the edge, which splits it (saturating, offset -> even harmonics).
 *
 * Reed: mass-spring reed with lip damping, Bernoulli flow u = zeta*max(1+x,0)*sqrt|gamma-p|,
 * solved exactly per sample (Guillemain, Kergomard & Voinier 2005). The bell / tone-hole lattice
 * is a 2-pole lowpass in the loop: above its cutoff the bore has no resonances, so the internal
 * spectrum falls and the reed can't squeak. The cutoff opens a little with breath.
 *
 * Output: the wave radiated through the lattice (complement of the loop filter) plus some
 * internal wave, a per-voice brightness that grows with breath; then, shared by all voices (they
 * all play the same preset): radiation highpass, brightness lowpass, two formant bands
 * (instrument body / bore character), the band-limited breath noise bus, DC blocker, soft limit.
 *
 * Tuning: period minus the loop filters' phase delays at f0 (block rate, polynomial atan), minus
 * a measured correction table per preset (voice_wind_tune.h). Delay lengths ramp per sample.
 * Live parameter writes go to e.p; the DSP follows a smoothed copy e.ps (~50 ms), so edits glide.
 *
 * How to play:
 *   pres   breath. Below the speaking pressure only a soft breath; then the tone blooms and gets
 *          louder and brighter. The saxes growl when pushed very hard (pressure > ~1.05) or shaken.
 *   vpres  tonguing: a fast strike gives a crisp tongued attack (pressure kick + "t" noise); a
 *          slow press a breathy swell. Ignored while the key is not pressed (stale vpres).
 *   but_y  embouchure, subtle: forward a little brighter / edgier, backward darker and breathier
 *          (subtone on the saxes, covered tone on the flute).
 *   note   pitch, vibrato and bends. Automatic vibrato is available as a parameter (off).
 *   pedal  unused.
 * No libm; no divisions in the sample loops; one vsqrt per sample for the reeds.
 */
#ifndef VOICE_WIND_H_
#define VOICE_WIND_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace wind {

typedef float real;
static const int BLOCK = strisy::BLOCK;
static const int MAX_VOICES = 10;

// delay lines (AXI SRAM): main bore up to a period (cone, flute) or half a period (cylinder) of
// the lowest note (MIDI 20: 1881 samples), short line for the cone apex / the flute's jet
static const int MAIN_LEN = 2048;
static const int SHORT_LEN = 1024;
static const real NOTE_MIN = 18.0f;
static const real NOTE_MAX = 122.0f;
static const real G_TH = 0.36f;          // gamma near the reeds' oscillation threshold
static const real G_TH_JET = 0.42f;      // jet drive near the flute's threshold

enum { KIND_CYL = 0, KIND_CONE = 1, KIND_JET = 2 };

// flute (jet) constants, found by sweeping the model on the host: the open pipe reflects without
// inversion; the jet is deflected against the acoustic flow (sign -1) and travels half a period to
// the edge, which keeps the fundamental in tune whatever the breath; gain, amplitude and the
// asymmetry term give about -12 dB 2nd and -18 dB 3rd harmonic at mezzo forte.
static const real JET_GAIN = 6.0f;
static const real JET_AMP = 0.05f;
static const real JET_H2 = 1.0f;


using strisy::rmax;
using strisy::rmin;
static inline real clampr(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }

static inline real fsqrt(real x) {
#if defined(__arm__) && defined(__ARM_FP)
	real r;
	__asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
	return r;
#else
	return __builtin_sqrtf(x);
#endif
}

// 2^p for |p| < 60, relative error ~1e-7 (block rate)
static inline real pow2(real p) {
	p = clampr(p, -60.0f, 60.0f);
	int w = (int)p;
	if ((real)w > p) w--;
	const real z = p - (real)w;
	const real f = 1.0f + z * (0.693146839f + z * (0.240231090f + z * (0.0554789288f + z * (0.00968616940f + z * (0.00123824704f + z * 0.000218713408f)))));
	union { uint32_t i; float f; } e = { (uint32_t)(w + 127) << 23 };
	return f * e.f;
}
static inline real cos_poly(real x) {
	real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
static inline real sin_poly(real x) {
	real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
// sine of a phase in turns (any value), block rate
static inline real sin_turns(real ph) {
	ph -= (real)(int)ph;
	if (ph < 0.0f) ph += 1.0f;
	// fold to [-1/4, 1/4] turn
	real x = ph < 0.25f ? ph : (ph < 0.75f ? 0.5f - ph : ph - 1.0f);
	return sin_poly(6.2831853f * x);
}
static inline real atan_pos(real t) {
	bool inv = t > 1.0f;
	real z = inv ? 1.0f / t : t;
	real z2 = z * z;
	real r = z * (0.99997726f + z2 * (-0.33262347f + z2 * (0.19354346f + z2 * (-0.11643287f + z2 * (0.05265332f + z2 * (-0.01172120f))))));
	return inv ? 1.5707963f - r : r;
}
static inline real atan2_r(real y, real x) {
	const real ax = fabsf(x), ay = fabsf(y);
	if (ax + ay <= 0.0f) return 0.0f;
	real a = ax >= ay ? atan_pos(ay / ax) : 1.5707963f - atan_pos(ax / ay);
	if (x < 0.0f) a = 3.1415927f - a;
	return y < 0.0f ? -a : a;
}
static inline real db2lin(real db) { return pow2(db * 0.16609640f); }

// ---------------------------------------------------------------------------------------------
// Sound design parameters (all live, see param_table)

struct Params {
	// breath
	real level;        // output level
	real speak;        // pressure where the tone starts to speak
	real dyn;          // breath drive (gamma) at pressure 1
	real attack;       // breath smoothing, ms
	real dyn_gain;     // 0: loudness from the physics only .. 1: also scaled by pressure
	// articulation
	real tongue;       // pressure kick of a fast strike
	real tongue_noise; // "t" noise burst of a fast strike
	// reed (jet: unused)
	real reed_open;    // zeta: reed opening / embouchure looseness
	real reed_freq;    // reed resonance, Hz (at least 6 f0)
	real reed_damp;    // lip damping of the reed
	// bore
	real loss;         // reflection gain at low frequencies
	real lattice;      // tone-hole lattice / bell cutoff, Hz (at least 2 f0)
	real lat_breath;   // cutoff opening with breath, octaves per unit pressure
	real apex;         // cone: apex share of the period at the lowest note
	real apex_lp;      // cone: apex reflection lowpass, multiples of f0
	real jet;          // flute: jet travel delay, fraction of the period
	real jet_off;      // flute: jet offset from the edge (even harmonics)
	// tone (shared output stage)
	real body;         // internal wave mixed into the radiated one
	real rad_hp;       // radiation highpass, Hz
	real bright;       // brightness lowpass, Hz
	real br_breath;    // extra brightness with breath (per voice, grows with pressure^2)
	real f1_hz, f1_db, f2_hz, f2_db, f_q;   // formant bands
	// noise
	real breath;       // audible breath (band-limited), relative
	real turb;         // turbulence on the breath drive
	real noise_hz;     // breath noise band centre, Hz
	// expression
	real tilt_bright;  // embouchure brightness from tilt
	real tilt_breath;  // breathiness / subtone with backward tilt
	real growl;        // growl depth when pushed hard
	real growl_from;   // pressure where the growl starts
	real vib;          // automatic vibrato, semitones
	real vib_hz;
	real vib_breath;   // automatic breath (amplitude) vibrato
};
static const int NPARAMS = sizeof(Params) / sizeof(real);

struct PresetInfo {
	const char* name;
	int kind;
	real low_note;     // lowest note of the instrument (sets the cone apex length)
};

static const int NUM_PRESETS = 9;
static const PresetInfo preset_info[NUM_PRESETS] = {
	{"clarinet", KIND_CYL, 50},
	{"bass clarinet", KIND_CYL, 34},
	{"soprano sax", KIND_CONE, 56},
	{"alto sax", KIND_CONE, 49},
	{"tenor sax", KIND_CONE, 44},
	{"baritone sax", KIND_CONE, 37},
	{"oboe", KIND_CONE, 58},
	{"bassoon", KIND_CONE, 34},
	{"flute", KIND_JET, 60},
};

static inline const char* preset_name(int i) {
	return (i >= 0 && i < NUM_PRESETS) ? preset_info[i].name : preset_info[0].name;
}

static inline Params base_params() {
	Params p;
	p.level = 1.0f; p.speak = 0.12f; p.dyn = 0.75f; p.attack = 3.0f; p.dyn_gain = 0.6f;
	p.tongue = 0.35f; p.tongue_noise = 0.5f;
	p.reed_open = 0.30f; p.reed_freq = 4000.0f; p.reed_damp = 0.5f;
	p.loss = 0.985f; p.lattice = 1100.0f; p.lat_breath = 0.4f; p.apex = 0.5f; p.apex_lp = 6.0f; p.jet = 0.5f; p.jet_off = 0.3f;
	p.body = 0.4f; p.rad_hp = 250.0f; p.bright = 5000.0f; p.br_breath = 1.5f;
	p.f1_hz = 800.0f; p.f1_db = 0.0f; p.f2_hz = 2500.0f; p.f2_db = 0.0f; p.f_q = 1.5f;
	p.breath = 0.3f; p.turb = 0.02f; p.noise_hz = 2000.0f;
	p.tilt_bright = 0.5f; p.tilt_breath = 0.5f; p.growl = 0.0f; p.growl_from = 1.05f;
	p.vib = 0.0f; p.vib_hz = 5.0f; p.vib_breath = 0.0f;
	return p;
}

static inline Params preset_params(int i) {
	Params p = base_params();
	switch (i) {
	default:
	case 0:  // clarinet
		p.level = 2.05f;
		break;
	case 1:  // bass clarinet
		p.level = 2.25f;
		p.lattice = 650.0f; p.reed_open = 0.34f; p.reed_freq = 2500.0f; p.bright = 3000.0f;
		p.rad_hp = 120.0f; p.noise_hz = 1200.0f; p.br_breath = 1.5f;
		break;
	case 2:  // soprano sax
		p.level = 1.27f;
		p.apex = 0.28f; p.apex_lp = 3.0f; p.dyn = 0.7f; p.lattice = 1800.0f; p.reed_open = 0.42f; p.reed_freq = 3600.0f;
		p.f1_hz = 900.0f; p.f1_db = 4.0f; p.f2_hz = 3000.0f; p.f2_db = 10.0f; p.f_q = 1.2f; p.bright = 10000.0f; p.rad_hp = 800.0f;
		p.growl = 0.45f; p.breath = 0.4f; p.br_breath = 3.0f;
		break;
	case 3:  // alto sax
		p.level = 1.56f;
		p.apex = 0.28f; p.apex_lp = 3.0f; p.dyn = 0.7f; p.lattice = 1500.0f; p.reed_open = 0.42f; p.reed_freq = 3400.0f;
		p.f1_hz = 650.0f; p.f1_db = 4.0f; p.f2_hz = 2500.0f; p.f2_db = 10.0f; p.f_q = 1.2f; p.bright = 9000.0f; p.rad_hp = 600.0f;
		p.growl = 0.45f; p.breath = 0.4f; p.br_breath = 3.0f;
		break;
	case 4:  // tenor sax
		p.level = 1.68f;
		p.apex = 0.28f; p.apex_lp = 3.0f; p.dyn = 0.7f; p.lattice = 1200.0f; p.reed_open = 0.44f; p.reed_freq = 3000.0f;
		p.f1_hz = 500.0f; p.f1_db = 4.0f; p.f2_hz = 2000.0f; p.f2_db = 9.0f; p.f_q = 1.2f; p.bright = 8000.0f;
		p.rad_hp = 450.0f; p.growl = 0.45f; p.breath = 0.45f; p.br_breath = 3.0f;
		break;
	case 5:  // baritone sax
		p.level = 1.84f;
		p.apex = 0.28f; p.apex_lp = 3.0f; p.dyn = 0.7f; p.lattice = 900.0f; p.reed_open = 0.46f; p.reed_freq = 2600.0f;
		p.f1_hz = 380.0f; p.f1_db = 4.0f; p.f2_hz = 1500.0f; p.f2_db = 8.0f; p.f_q = 1.2f; p.bright = 6000.0f;
		p.rad_hp = 300.0f; p.growl = 0.45f; p.breath = 0.45f; p.noise_hz = 1500.0f; p.br_breath = 3.0f;
		break;
	case 6:  // oboe
		p.level = 1.16f;
		p.apex = 0.28f; p.apex_lp = 3.0f; p.dyn = 0.68f; p.lattice = 2000.0f; p.reed_open = 0.30f; p.reed_freq = 5000.0f; p.reed_damp = 0.6f;
		p.f1_hz = 1250.0f; p.f1_db = 12.0f; p.f2_hz = 3000.0f; p.f2_db = 10.0f; p.f_q = 2.0f; p.bright = 9000.0f;
		p.rad_hp = 1000.0f; p.breath = 0.2f; p.noise_hz = 2500.0f; p.br_breath = 2.0f; p.speak = 0.09f; p.tilt_breath = 0.3f;
		break;
	case 7:  // bassoon
		p.level = 2.31f;
		p.apex = 0.28f; p.apex_lp = 3.0f; p.dyn = 0.7f; p.lattice = 600.0f; p.reed_open = 0.22f; p.reed_freq = 3000.0f; p.reed_damp = 0.6f;
		p.f1_hz = 480.0f; p.f1_db = 8.0f; p.f2_hz = 1150.0f; p.f2_db = 8.0f; p.f_q = 2.0f; p.bright = 4000.0f;
		p.rad_hp = 300.0f; p.breath = 0.2f; p.noise_hz = 1200.0f; p.br_breath = 3.0f; p.bright = 5000.0f; p.speak = 0.05f; p.tilt_breath = 0.3f;
		break;
	case 8:  // flute
		p.level = 0.59f;
		p.speak = 0.12f; p.dyn = 1.2f; p.lattice = 3000.0f; p.loss = 0.95f; p.jet = 0.5f; p.jet_off = 0.6f;
		p.body = 3.0f; p.rad_hp = 300.0f; p.bright = 7000.0f; p.f_q = 1.0f;
		p.breath = 1.0f; p.turb = 0.03f; p.noise_hz = 3000.0f; p.tongue_noise = 0.8f; p.br_breath = 2.0f; p.tongue = 0.6f;
		break;
	}
#ifdef WIND_OVR
	WIND_OVR;   // host experiments: -DWIND_OVR=p.apex=0.2f,p.loss=0.98f
#endif
	return p;
}

// measured tuning correction per preset: samples of extra loop delay at notes 20 + 7k
#include "voice_wind_tune.h"

static inline real tuning_corr(int preset, real note) {
	const real* t = CORR[preset];
	real k = clampr((note - 20.0f) * (1.0f / 7.0f), 0.0f, 12.999f);
	int i = (int)k;
	real f = k - (real)i;
	return t[i] + f * (t[i + 1] - t[i]);
}

// ---------------------------------------------------------------------------------------------

struct VoiceState {
	int active;
	int quiet;
	uint32_t widx, rng;
	real note_prev;
	real ps;             // smoothed pressure
	real ty;             // smoothed tilt
	real tongue;         // articulation envelope
	real growl;          // smoothed growl amount
	real gph;            // growl phase (turns)
	real gm;             // breath multiplier (growl, breath vibrato) at the end of the last block
	real gam;            // breath drive at the end of the last block
	real og;             // output gain at the end of the last block
	real d1, d2;         // delay lengths at the end of the last block
	real lp, lp2;        // loop filter states
	real ls;             // apex lowpass state
	real dcx, dcy;       // flute loop DC blocker
	real x1, x2, pm1, gm1;   // reed
	real tn;             // turbulence noise lowpass
	real yp;             // previous output sample (breath brightness)
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset, kind;
	real sr;
	Params p;            // as set (presets, live edits)
	Params ps;           // smoothed copy used by the DSP
	real low_note;
	real inv_sr;         // 1 / sample rate
	real kp;             // breath smoothing coefficient (per block)
	real inv_1mp0;       // 1 / (1 - speak)
	real inv_p0;         // 1 / speak
	real p_low;          // period of the lowest note (cone apex length)
	real shake;
	real vph;            // vibrato phase (turns)
	// shared output stage
	real hp_s, lp_s;     // radiation highpass / brightness lowpass states
	strisy::SvfState f1s, f2s, nbs;
	real dc1, dcx;
	VoiceState vs[MAX_VOICES];
	real mix[BLOCK];     // voices
	real nmix[BLOCK];    // breath noise bus
};

static float wind_main[MAX_VOICES][MAIN_LEN];
static float wind_short[MAX_VOICES][SHORT_LEN];

static inline void reset_voice(Engine& e, int v, bool clear) {
	VoiceState& s = e.vs[v];
	real note = s.note_prev;
	s = VoiceState();
	s.rng = 0x9E3779B9u * (uint32_t)(v + 1);
	s.note_prev = note;
	s.gm = 1.0f;
	s.d1 = 100.0f; s.d2 = 10.0f;
	if (clear) {
		for (int i = 0; i < MAIN_LEN; i++) wind_main[v][i] = 0.0f;
		for (int i = 0; i < SHORT_LEN; i++) wind_short[v][i] = 0.0f;
	}
}

// Select a preset (between blocks, any time). A change of excitation kind silences the voices.
static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	const int kind = preset_info[i].kind;
	e.preset = i;
	e.p = preset_params(i);
	e.ps = e.p;
	e.low_note = preset_info[i].low_note;
	if (kind != e.kind) {
		e.kind = kind;
		for (int v = 0; v < MAX_VOICES; v++) reset_voice(e, v, true);
	}
}

#ifndef WIND_PRESET
#define WIND_PRESET 0
#endif

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	e.shake = 0.0f; e.vph = 0.0f;
	e.hp_s = e.lp_s = 0.0f;
	e.f1s = e.f2s = e.nbs = strisy::SvfState();
	e.dc1 = e.dcx = 0.0f;
	for (int n = 0; n < BLOCK; n++) e.mix[n] = e.nmix[n] = 0.0f;
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.vs[v].note_prev = 69.0f;
		reset_voice(e, v, true);
	}
	e.kind = preset_info[WIND_PRESET < NUM_PRESETS ? WIND_PRESET : 0].kind;
	set_preset(e, WIND_PRESET);
}

// breath pressure -> drive (gamma for the reeds, jet gain for the flute); linear to the speaking
// pressure, then linear to q.dyn at pressure 1
static inline real drive_of(const Params& q, real p, int kind, real inv_p0, real inv_1mp0) {
	const real p0 = clampr(q.speak, 0.02f, 0.6f);
	const real th = kind == KIND_JET ? G_TH_JET : G_TH;
	return p < p0 ? p * th * inv_p0 : th + (q.dyn - th) * (p - p0) * inv_1mp0;
}

// 3rd order Lagrange read at delay d (>= 2) behind write index w
static inline real lagrange_read(const float* __restrict__ buf, uint32_t w, real dl) {
	const int i0 = (int)dl - 1;
	const real d = dl - (real)i0;
	const real dm1 = d - 1.0f, dm2 = d - 2.0f, dm3 = d - 3.0f;
	const real p01 = d * dm1, p23 = dm2 * dm3;
	const uint32_t b0 = w - (uint32_t)i0;
	return -(dm1 * p23) * (1.0f / 6.0f) * buf[b0 & (MAIN_LEN - 1)] + (d * p23) * 0.5f * buf[(b0 - 1u) & (MAIN_LEN - 1)]
			- (p01 * dm3) * 0.5f * buf[(b0 - 2u) & (MAIN_LEN - 1)] + (p01 * dm2) * (1.0f / 6.0f) * buf[(b0 - 3u) & (MAIN_LEN - 1)];
}

// per-block values of a voice, handed to the sample loops
struct BlockCoef {
	real d1, dd1, d2, dd2, gam, dgam, gm, dgm, og, dog;
	real bb0, ba1, ba2, g;         // loop lowpass (2-pole) and reflection gain
	real cbs, gs;                  // apex lowpass, apex reflection (cone) / flute: one-pole and DC
	real zeta, rb, r1, r2;         // reed
	real turb, ct, body, bo;       // turbulence depth, its lowpass, body mix, breath bus level
	real joff, soff, ja, jn;       // flute: jet offset, its saturated value, jet amplitude, jet noise
	real kb;                       // breath brightness (first difference)
};

template <int KIND>
static inline real run_voice(Engine& e, int v, const BlockCoef& c) {
	VoiceState& s = e.vs[v];
	float* __restrict__ mb = wind_main[v];
	float* __restrict__ sb = wind_short[v];
	real* __restrict__ mix = e.mix;
	real* __restrict__ nmix = e.nmix;
	uint32_t widx = s.widx, rng = s.rng;
	real lp = s.lp, lp2 = s.lp2, ls = s.ls, dcx = s.dcx, dcy = s.dcy;
	real x1 = s.x1, x2 = s.x2, pm1 = s.pm1, gm1 = s.gm1, tn = s.tn, yp = s.yp;
	real d1 = c.d1, d2 = c.d2, gam = c.gam, gm = c.gm, og = c.og;
	const real bb0 = c.bb0, ba1 = c.ba1, ba2 = c.ba2, g = c.g, cbs = c.cbs, gs = c.gs;
	const real zeta = c.zeta, rb = c.rb, r1 = c.r1, r2 = c.r2, turb = c.turb, ct = c.ct, body = c.body, bo = c.bo;
	const real joff = c.joff, soff = c.soff, ja = c.ja, jn = c.jn, kb = c.kb;
	real peak = 0.0f;
	for (int n = 0; n < BLOCK; n++) {
		d1 += c.dd1; gam += c.dgam; gm += c.dgm; og += c.dog;
		if (KIND != KIND_CYL) d2 += c.dd2;
		rng = rng * 1664525u + 1013904223u;
		const real wn = (real)(int32_t)rng * (1.0f / 2147483648.0f);
		tn += ct * (wn - tn);
		nmix[n] += bo * wn;
		const real drv = gam * (gm + turb * tn);
		const real bi = lagrange_read(mb, widx, d1);
		real y;
		if (KIND == KIND_JET) {
			// bore: one-pole loss lowpass, DC blocker, reflection
			lp += cbs * (bi - lp);
			dcy = lp - dcx + gs * dcy;
			dcx = lp;
			const real tmp = g * dcy;
			// jet: deflected by the acoustic flow at the mouth hole, the deflection travels with
			// the jet to the edge (delay d2); the edge splits it (saturating, offset -> even
			// harmonics). The gain grows with the breath (jet speed).
			sb[widx & (SHORT_LEN - 1)] = tmp;
			const int j0 = (int)d2;
			const real fj = d2 - (real)j0;
			const uint32_t c0 = widx - (uint32_t)j0;
			const real s0 = sb[c0 & (SHORT_LEN - 1)], s1 = sb[(c0 - 1u) & (SHORT_LEN - 1)];
			const real jx = clampr(drv * (jn * tn - JET_GAIN * (s0 + fj * (s1 - s0))) + joff, -1.5f, 1.5f);
			const real jt = (jx * (1.0f - 0.148148148f * jx * jx) - soff) * ja;
			mb[widx & (MAIN_LEN - 1)] = jt + tmp;
			widx++;
			y = bi + body * jt + JET_H2 * joff * bi * bi;   // bore, jet sound at the mouth hole, jet asymmetry
		} else {
			// lattice / bell: 2-pole lowpass, reflection
			const real yl = bb0 * bi + lp;
			lp = 2.0f * bb0 * bi - ba1 * yl + lp2;
			lp2 = bb0 * bi - ba2 * yl;
			const real am = -g * yl;
			real V, bm = 0.0f;
			if (KIND == KIND_CONE) {
				const int j0 = (int)d2;
				const real fj = d2 - (real)j0;
				const uint32_t c0 = widx - (uint32_t)j0;
				const real s0 = sb[c0 & (SHORT_LEN - 1)], s1 = sb[(c0 - 1u) & (SHORT_LEN - 1)];
				ls += cbs * (s0 + fj * (s1 - s0) - ls);
				bm = -gs * ls;
				V = am + bm;
			} else {
				V = 2.0f * am;
			}
			// reed displacement from the previous pressure difference, then the flow
			const real x = rb * (pm1 - gm1) + r1 * x1 + r2 * x2;
			x2 = x1; x1 = x;
			const real W = zeta * rmax(1.0f + x, 0.0f);
			const real X = drv - V;
			const real uabs = 0.5f * W * (fsqrt(W * W + 4.0f * fabsf(X)) - W);
			const real u = X >= 0.0f ? uabs : -uabs;
			const real p = V + u;
			pm1 = p; gm1 = drv;
			if (KIND == KIND_CONE) {
				mb[widx & (MAIN_LEN - 1)] = bm + u;
				sb[widx & (SHORT_LEN - 1)] = am + u;
			} else {
				mb[widx & (MAIN_LEN - 1)] = am + u;
			}
			widx++;
			y = (bi + am) + body * bi;   // radiated through the lattice + some internal wave
		}
		const real yo = og * (y + kb * (y - yp));
		yp = y;
		y = yo;
		mix[n] += y;
		peak = rmax(peak, fabsf(y));
	}
	s.widx = widx; s.rng = rng;
	s.lp = lp; s.lp2 = lp2; s.ls = ls; s.dcx = dcx; s.dcy = dcy;
	s.x1 = x1; s.x2 = x2; s.pm1 = pm1; s.gm1 = gm1; s.tn = tn; s.yp = yp;
	return peak;
}

static inline void process_voice(Engine& e, int v) {
	VoiceState& s = e.vs[v];
	const strisy::VoiceControls c = e.vc[v];
	const Params& q = e.ps;
	real pres = clampr(c.pres, 0.0f, 1.5f);
	if (!(pres == pres)) pres = 0.0f;
	real vp = clampr(c.vpres - 0.02f, 0.0f, 1.0f);
	if (!(vp == vp)) vp = 0.0f;
	// velocity only while pressed (the firmware leaves a stale vpres on released voices)
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

	const real sr = e.sr;
	const int kind = e.kind;
	// breath smoothing (attack), tilt, tongue, growl
	s.ps += e.kp * (pres - s.ps);
	if (s.ps < 1e-6f) s.ps = 0.0f;
	const real ps = s.ps;
	s.ty += 0.03f * (clampr(c.but_y, -1.0f, 1.0f) - s.ty);
	const real ty = s.ty;
	s.tongue = rmax(s.tongue * 0.94f, vplev);   // ~16 ms
	if (s.tongue < 1e-6f) s.tongue = 0.0f;
	const real tg = s.tongue;
	const real gr_t = clampr((ps - q.growl_from) * 3.0f, 0.0f, 1.0f) + e.shake;
	s.growl += 0.05f * (rmin(gr_t, 1.2f) - s.growl);
	if (s.growl < 1e-6f) s.growl = 0.0f;
	const real ebr = q.tilt_bright * ty;                       // embouchure brightness -1..1
	const real sub = q.tilt_breath * rmax(-ty, 0.0f);          // subtone / covered amount 0..1

	// pitch (with the automatic vibrato) and the fundamental
	const real vibs = q.vib * sin_turns(e.vph + 0.13f * (real)v);
	const real nt = note + vibs;
	const real P = (sr * (1.0f / 440.0f)) * pow2((69.0f - nt) * (1.0f / 12.0f));
	const real f0 = 440.0f * pow2((nt - 69.0f) * (1.0f / 12.0f));
	const real isr = e.inv_sr;
	const real P_2pi = P * 0.15915494f;     // 1 / w0
	const real w0 = 6.2831853f / P;
	const real sw = sin_poly(w0), cw = cos_poly(w0);

	BlockCoef b;
	real tau;
	// loop filter cutoff: lattice, opening with breath and forward embouchure, at least 2 f0
	real fc = q.lattice * pow2(q.lat_breath * (ps - 0.5f) + 0.5f * ebr - 0.3f * sub);
	if (kind == KIND_JET) {
		fc = rmax(fc, 3.0f * f0);
		const real cp = pow2(-fc * (6.2831853f * 1.442695f) * isr);
		b.cbs = 1.0f - cp;
		b.gs = 0.995f;   // DC blocker pole
		// phase delays at f0: one-pole lowpass (lag) and DC blocker (lead)
		const real t_lp = atan_pos(cp * sw / (1.0f - cp * cw)) * P_2pi;
		const real ph_dc = atan2_r(sw, 1.0f - cw) - atan2_r(b.gs * sw, 1.0f - b.gs * cw);
		tau = t_lp - ph_dc * P_2pi;
		b.bb0 = b.ba1 = b.ba2 = 0.0f;
	} else {
		fc = rmax(fc, 2.0f * f0);
		const real K = strisy::tan0(rmin(fc * 3.1415927f * isr, 1.2f));
		const real bn = 1.0f / (1.0f + 1.4142136f * K + K * K);
		b.bb0 = K * K * bn;
		b.ba1 = 2.0f * (K * K - 1.0f) * bn;
		b.ba2 = (1.0f - 1.4142136f * K + K * K) * bn;
		const real c2w = 2.0f * cw * cw - 1.0f, s2w = 2.0f * sw * cw;
		tau = 1.0f + atan2_r(-(b.ba1 * sw + b.ba2 * s2w), 1.0f + b.ba1 * cw + b.ba2 * c2w) * P_2pi;
		b.cbs = 1.0f; b.gs = 0.997f;
	}
	// release: once the breath is gone, the bore dies out in ~0.1 s whatever its length
	const real rel = clampr(1.0f - ps * 20.0f, 0.0f, 1.0f) * (9.97f / (0.1f * 48828.0f));
	b.g = clampr(q.loss, 0.5f, 0.999f);

	// reed (mass-spring, centered differences, stable for omega*T < 2)
	{
		const real zeta = clampr(q.reed_open, 0.05f, 1.0f) * (1.0f + 0.25f * ebr) * (1.0f - 0.15f * sub);
		const real fr = rmax(clampr(q.reed_freq, 500.0f, 12000.0f), 6.0f * f0);
		const real wr = rmin(fr * 6.2831853f * isr, 1.4f);
		const real qr = clampr(q.reed_damp, 0.1f, 2.0f) * (1.0f - 0.2f * ebr);
		const real iw = 1.0f / wr;
		const real A = iw * iw, B = 0.5f * qr * iw;
		const real ia0 = 1.0f / (A + B);
		b.zeta = zeta; b.rb = ia0; b.r1 = (2.0f * A - 1.0f) * ia0; b.r2 = -(A - B) * ia0;
	}

	// delay lengths: loop delay + filter phase delays + correction = period
	const real corr = tuning_corr(e.preset, nt);
	real D1, D2 = 1.0f;
	if (kind == KIND_CYL) {
		D1 = clampr(0.5f * P - tau - corr, 2.0f, (real)(MAIN_LEN - 5));
	} else if (kind == KIND_CONE) {
		// apex: fixed length (its share at the lowest note is q.apex), at most 45% of the period
		const real ap = clampr(q.apex, 0.05f, 0.5f);
		const real beta = clampr(ap * e.p_low * f0 * isr, ap, 0.45f);     // p_low / P
		const real cps = pow2(-clampr(q.apex_lp, 1.0f, 30.0f) * f0 * (6.2831853f * 1.442695f) * isr);
		b.cbs = 1.0f - cps;
		const real taus = atan_pos(cps * sw / (1.0f - cps * cw)) * P_2pi;
		D2 = clampr(beta * P - taus, 1.0f, (real)(SHORT_LEN - 2));
		D1 = clampr(P - D2 - taus - tau - corr, 2.0f, (real)(MAIN_LEN - 5));
		b.gs = 0.997f * pow2(-rel * D2);
	} else {
		D1 = clampr(P - tau - corr, 2.0f, (real)(MAIN_LEN - 5));
		D2 = clampr(clampr(q.jet, 0.05f, 1.0f) * P, 1.0f, (real)(SHORT_LEN - 2));
	}
	b.g *= pow2(-rel * D1);
	if (jump || wake) { s.d1 = D1; s.d2 = D2; }
	b.d1 = s.d1; b.dd1 = (D1 - s.d1) * (1.0f / BLOCK);
	b.d2 = s.d2; b.dd2 = (D2 - s.d2) * (1.0f / BLOCK);
	s.d1 = D1; s.d2 = D2;

	// breath drive with tongue kick; growl and breath vibrato as a multiplier
	const real gam_t = rmin(drive_of(q, ps, kind, e.inv_p0, e.inv_1mp0) * (1.0f + q.tongue * tg) * (1.0f - 0.08f * sub), kind == KIND_JET ? 1.4f : 0.94f);
	b.gam = s.gam; b.dgam = (gam_t - s.gam) * (1.0f / BLOCK); s.gam = gam_t;
	s.gph += clampr(0.5f * f0 + 3.0f, 25.0f, 140.0f) * (BLOCK * isr);
	s.gph -= (real)(int)s.gph;
	const real gm_t = 1.0f + q.growl * s.growl * sin_turns(s.gph) + q.vib_breath * sin_turns(e.vph + 0.13f * (real)v + 0.25f);
	b.gm = s.gm; b.dgm = (gm_t - s.gm) * (1.0f / BLOCK); s.gm = gm_t;
	// output gain: physics, optionally scaled with breath
	const real psc = clampr(ps, 0.0f, 1.3f);
	const real og_t = q.level * ((1.0f - q.dyn_gain) + q.dyn_gain * psc);
	b.og = s.og; b.dog = (og_t - s.og) * (1.0f / BLOCK); s.og = og_t;
	// noise: turbulence on the drive (lowpass ~2 kHz); audible breath: rises with pres^2 up to
	// the speaking pressure, then settles to a quieter part of the tone; tongue "t" burst
	b.turb = q.turb * (1.0f + 2.0f * sub) + 0.1f * tg;
	b.ct = 0.25f;
	const real nr = clampr(ps * e.inv_p0, 0.0f, 1.0f);
	const real speaking = clampr((ps - clampr(q.speak, 0.02f, 0.6f)) * 4.0f, 0.0f, 1.0f);
	b.bo = 0.02f * (q.breath * nr * nr * (1.0f - 0.6f * speaking) * (1.0f + 2.0f * sub) * (0.5f + 0.5f * psc)
			+ q.tongue_noise * tg * nr);
	b.body = q.body;
	b.kb = clampr(q.br_breath, 0.0f, 8.0f) * psc * psc;
	b.joff = clampr(q.jet_off, 0.0f, 0.9f);
	b.soff = b.joff * (1.0f - 0.148148148f * b.joff * b.joff);
	b.ja = JET_AMP;
	b.jn = (kind == KIND_JET) ? 4.0f * b.turb : 0.0f;

	const real peak = kind == KIND_CYL ? run_voice<KIND_CYL>(e, v, b)
			: kind == KIND_CONE ? run_voice<KIND_CONE>(e, v, b) : run_voice<KIND_JET>(e, v, b);

	// safety: reset a voice whose state went non-finite or absurd
	if (!(fabsf(s.lp) + fabsf(s.x1) + fabsf(s.pm1) + fabsf(s.dcy) + peak < 1000.0f)) {
		reset_voice(e, v, true);
		s.note_prev = note;
		return;
	}
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

static inline strisy::SvfCoef svf_hz(real hz, real sr, real Q) {
	return strisy::svf_from_g(strisy::tan0(clampr(hz, 20.0f, 0.4f * sr) * 3.1415927f / sr), Q);
}

static inline void process(Engine& e, real* __restrict__ output) {
	// smooth the live parameters towards the written values (~50 ms time constant: bore and reed
	// edits then glide the pitch of a held note by a few cents instead of stepping it)
	{
		real* a = &e.ps.level;
		const real* t = &e.p.level;
		for (int i = 0; i < NPARAMS; i++) {
			const real d = t[i] - a[i];
			a[i] = (d == d) ? a[i] + 0.012f * d : t[i];
		}
	}
	const Params& q = e.ps;
	const real sr = e.sr;
	e.kp = 1.0f - pow2(-1442.7f * BLOCK * e.inv_sr / clampr(q.attack, 0.3f, 100.0f));
	e.inv_1mp0 = 1.0f / (1.0f - clampr(q.speak, 0.02f, 0.6f));
	e.inv_p0 = 1.0f / clampr(q.speak, 0.02f, 0.6f);
	e.p_low = (sr * (1.0f / 440.0f)) * pow2((69.0f - e.low_note) * (1.0f / 12.0f));
	const real sh = clampr((e.g.acc_abs - 1.15f) * 0.8f, 0.0f, 1.0f);
	e.shake += (sh > e.shake ? 0.2f : 0.02f) * (sh - e.shake);
	if (!(e.shake == e.shake)) e.shake = 0.0f;
	e.vph += clampr(q.vib_hz, 0.1f, 12.0f) * (BLOCK / sr);
	e.vph -= (real)(int)e.vph;

	for (int n = 0; n < BLOCK; n++) e.mix[n] = e.nmix[n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);

	// shared output stage: radiation highpass, brightness lowpass, formants, breath bus
	const real chp = 1.0f - pow2(-clampr(q.rad_hp, 10.0f, 5000.0f) * (6.2831853f * 1.442695f) / sr);
	const real clp = 1.0f - pow2(-clampr(q.bright, 200.0f, 20000.0f) * (6.2831853f * 1.442695f) / sr);
	const real fq = clampr(q.f_q, 0.3f, 8.0f);
	const strisy::SvfCoef c1 = svf_hz(q.f1_hz, sr, fq), c2 = svf_hz(q.f2_hz, sr, fq);
	const real g1 = (db2lin(clampr(q.f1_db, -24.0f, 24.0f)) - 1.0f) * c1.k;
	const real g2 = (db2lin(clampr(q.f2_db, -24.0f, 24.0f)) - 1.0f) * c2.k;
	const strisy::SvfCoef cn = svf_hz(q.noise_hz, sr, 0.7f);
	real hp = e.hp_s, lpo = e.lp_s, a1 = e.f1s.ic1, a2 = e.f1s.ic2, b1 = e.f2s.ic1, b2 = e.f2s.ic2;
	real n1 = e.nbs.ic1, n2 = e.nbs.ic2;
	real s1 = e.dc1, xp = e.dcx;
	for (int n = 0; n < BLOCK; n++) {
		real x = e.mix[n];
		x = fabsf(x) < 1e4f ? x : 0.0f;    // also catches NaN
		hp += chp * (x - hp);
		const real xr = x - hp;            // radiation highpass
		lpo += clp * (xr - lpo);           // brightness lowpass
		real v1, v2, w1, w2, u1, u2;
		SVF_TICK(lpo, a1, a2, c1, v1, v2);
		SVF_TICK(lpo, b1, b2, c2, w1, w2);
		real nb = e.nmix[n];
		nb = fabsf(nb) < 1e4f ? nb : 0.0f;
		SVF_TICK(nb, n1, n2, cn, u1, u2);
		(void)v2; (void)w2; (void)u2;
		const real y = lpo + g1 * v1 + g2 * w1 + cn.k * u1;
		// DC blocker (~10 Hz) and soft limit above 4 (asymptote 8)
		s1 = 0.99871f * s1 + y - xp;
		xp = y;
		const real a = fabsf(s1);
		const real o = a > 4.0f ? 4.0f + (a - 4.0f) / (1.0f + 0.25f * (a - 4.0f)) : a;
		output[n] = clampr(s1 < 0.0f ? -o : o, -8.0f, 8.0f);
	}
	// NaN guards; exact 0 when silent
	if (!(fabsf(hp) + fabsf(lpo) + fabsf(a1) + fabsf(a2) + fabsf(b1) + fabsf(b2) + fabsf(n1) + fabsf(n2) + fabsf(s1) < 1e6f)) {
		hp = lpo = a1 = a2 = b1 = b2 = n1 = n2 = s1 = xp = 0.0f;
	}
	if (fabsf(hp) + fabsf(lpo) + fabsf(a1) + fabsf(a2) + fabsf(b1) + fabsf(b2) + fabsf(n1) + fabsf(n2) + fabsf(s1) + fabsf(xp) < 1e-9f) {
		hp = lpo = a1 = a2 = b1 = b2 = n1 = n2 = s1 = xp = 0.0f;
	}
	e.hp_s = hp; e.lp_s = lpo; e.f1s = {a1, a2}; e.f2s = {b1, b2}; e.nbs = {n1, n2};
	e.dc1 = s1; e.dcx = xp;
}

// ---------------------------------------------------------------------------------------------
// Live parameters

struct ParamEntry {
	strisy::ParamDesc d;
	real Params::* m;
};

static const ParamEntry param_table[] = {
	{{"level", "breath", 0.1f, 4.0f, 0}, &Params::level},
	{{"speak", "breath", 0.03f, 0.5f, 0}, &Params::speak},          // pressure where the tone speaks
	{{"dyn", "breath", 0.4f, 1.5f, 0}, &Params::dyn},               // drive at full pressure (reeds <= 0.94)
	{{"attack", "breath", 0.5f, 60.0f, 0}, &Params::attack},        // breath smoothing (ms)
	{{"dyn_gain", "breath", 0.0f, 1.0f, 0}, &Params::dyn_gain},     // loudness also scaled by breath
	{{"tongue", "artic", 0.0f, 1.0f, 0}, &Params::tongue},          // pressure kick of a fast strike
	{{"tongue_noise", "artic", 0.0f, 2.0f, 0}, &Params::tongue_noise},
	{{"reed_open", "reed", 0.08f, 0.7f, 0}, &Params::reed_open},    // embouchure looseness
	{{"reed_freq", "reed", 1500.0f, 8000.0f, 0}, &Params::reed_freq},
	{{"reed_damp", "reed", 0.2f, 1.5f, 0}, &Params::reed_damp},
	{{"loss", "bore", 0.9f, 0.995f, 0}, &Params::loss},
	{{"lattice", "bore", 300.0f, 6000.0f, 0}, &Params::lattice},    // tone-hole cutoff (Hz)
	{{"lat_breath", "bore", 0.0f, 1.5f, 0}, &Params::lat_breath},   // octaves brighter per pressure
	{{"apex", "bore", 0.15f, 0.45f, 0}, &Params::apex},             // cone apex share (lowest note)
	{{"apex_lp", "bore", 1.5f, 20.0f, 0}, &Params::apex_lp},
	{{"jet", "bore", 0.4f, 0.6f, 0}, &Params::jet},                 // flute jet delay (of period)
	{{"jet_off", "bore", 0.0f, 0.8f, 0}, &Params::jet_off},         // flute jet offset
	{{"body", "tone", 0.0f, 2.0f, 0}, &Params::body},
	{{"rad_hp", "tone", 20.0f, 2000.0f, 0}, &Params::rad_hp},
	{{"bright", "tone", 500.0f, 16000.0f, 0}, &Params::bright},
	{{"br_breath", "tone", 0.0f, 8.0f, 0}, &Params::br_breath},
	{{"f1_hz", "tone", 150.0f, 5000.0f, 0}, &Params::f1_hz},
	{{"f1_db", "tone", -12.0f, 15.0f, 0}, &Params::f1_db},
	{{"f2_hz", "tone", 300.0f, 8000.0f, 0}, &Params::f2_hz},
	{{"f2_db", "tone", -12.0f, 15.0f, 0}, &Params::f2_db},
	{{"f_q", "tone", 0.5f, 6.0f, 0}, &Params::f_q},
	{{"breath", "noise", 0.0f, 3.0f, 0}, &Params::breath},          // audible breath
	{{"turb", "noise", 0.0f, 0.15f, 0}, &Params::turb},             // turbulence on the drive
	{{"noise_hz", "noise", 300.0f, 8000.0f, 0}, &Params::noise_hz},
	{{"tilt_bright", "expr", 0.0f, 1.0f, 0}, &Params::tilt_bright},
	{{"tilt_breath", "expr", 0.0f, 1.0f, 0}, &Params::tilt_breath},
	{{"growl", "expr", 0.0f, 0.8f, 0}, &Params::growl},
	{{"growl_from", "expr", 0.5f, 1.5f, 0}, &Params::growl_from},
	{{"vib", "mod", 0.0f, 0.6f, 0}, &Params::vib},                  // automatic vibrato, semitones
	{{"vib_hz", "mod", 2.0f, 8.0f, 0}, &Params::vib_hz},
	{{"vib_breath", "mod", 0.0f, 0.3f, 0}, &Params::vib_breath},
};
static const int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

} // namespace wind

namespace strisy {
template <> struct EngineParams<wind::Engine> {
	static const int count = wind::NUM_LIVE_PARAMS;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &wind::param_table[i].d : 0; }
	static float* ptr(wind::Engine& e, int i) { return (i >= 0 && i < count) ? &(e.p.*(wind::param_table[i].m)) : 0; }
	static void changed(wind::Engine&, int) {}   // the DSP follows e.p through its smoothed copy
};
} // namespace strisy

#endif // VOICE_WIND_H_
