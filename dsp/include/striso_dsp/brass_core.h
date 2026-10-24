/**
 * brass_core.h: the shared physical model of the brass engines (voice_trumpet.h, voice_brass2.h),
 * templated on an instrument family F (the family's bores, lip calibration, warm-start seeds,
 * presets, parameters and constants; see the Family structs in those headers).
 *
 * Everything runs at 2x the board rate (96 kHz) and is coupled: the pitch comes from the lips
 * locking to the bore's resonances, never from an oscillator.
 *
 *   mouth p_m --> LIPS: two mechanical modes, as measured on players' and artificial lips (Cullen,
 *                 Gilbert & Campbell 2000; Newton, Campbell & Gilbert 2008): an outward striking one
 *                 (the pressure difference pushes the lips open; Elliott & Bowsher 1982) tuned below
 *                 the note and an inward striking one (pushed closed) 150 c above it; the opening is
 *                 the sum. Their opposite-sign coupling through the load makes the lips buzz on their
 *                 own (coupled-mode flutter, like lips buzzing in a mouthpiece), so a tongued note
 *                 speaks in its first periods and the bore pulls it onto the resonance. Contact
 *                 stiffness when closed; swept volume flow
 *                 | Bernoulli jet u = w h sqrt(2 (p_m - p_c) / rho), regularised at small dp
 *                 CUP compliance V / (rho c^2) (implicit in p_c with the jet's conductance: stable)
 *                   |
 *                 FRONT END: throat, backbore, leadpipe as a Kelly-Lochbaum ladder of short
 *                 cylinders (integer delays at 96 kHz): with the cup it makes the mouthpiece
 *                 "popping" resonance
 *                   |
 *                 CYLINDER (leadpipe end .. bell throat): forward and backward delay lines.
 *                 Valves add tubing; while a piston moves both lengths sound, crossfaded (a
 *                 half-open valve); the tuning slide trims the length continuously. A trombone's
 *                 slide is a continuous length (F::HAS_SLIDE and the bore's slide flag) that moves
 *                 at the slide's speed; its triggers are valves.
 *                   |
 *                 BELL: the reflection function of the flaring bell (+ the losses of the whole bore)
 *                 as an exact multirate convolution: decimate 8x -> FIR at 12 kHz -> interpolate 8x,
 *                 fitted (tools/trumpet) so the model's input impedance matches a transmission-line
 *                 model of real bore profiles.
 *                   |
 *                 RADIATION (shared, after the mix): the wave arriving at the bell, steepened
 *                 (amplitude dependent propagation speed, Burgers; Hirschberg et al. 1996, Msallam et
 *                 al. 2000: the brassy "cuivre"; the loop itself stays linear), then the bell
 *                 transmission, 96 -> 48 kHz, on-axis bell directivity, an EQ fitted to recordings,
 *                 mutes, a little room.
 *
 * The player (per note): picks valves / slide + partial from the horn's own resonance chart, sets
 * the lips (the family's calibration per preset and note), and trims slide + lips with a slow pitch
 * loop (zero crossings of the lip motion) so the note lands within a few cents of the Striso's
 * pitch, also for microtonal notes and bends; what it learns per fingering is kept while powered.
 */
#ifndef BRASS_CORE_H_
#define BRASS_CORE_H_

#include <math.h>
#include <stdint.h>
#ifdef BRASS_DEBUG
#include <stdio.h>
#endif

#include "strisy.h"

namespace brasspm {

using real = float;
using strisy::rmax;
using strisy::rmin;

static constexpr int BLOCK = strisy::BLOCK;
static constexpr int MAX_VOICES = 10; // key voices (controls)
static constexpr int OS = 2;          // internal oversampling
static constexpr int NS = BLOCK * OS; // internal samples per block
static constexpr int SLEN = 32;       // front-end section rings
static constexpr uint32_t SMASK = SLEN - 1;
static constexpr int MAXSEC = 8;
static constexpr int DHL = 64;     // decimator history (double buffered)
static constexpr int NSTACK = 4;   // held keys under a slurred note
static constexpr int ROOML = 1024; // early reflections (48 kHz)
static constexpr real RHO = 1.2f;
static constexpr real C0 = 343.0f;
static constexpr real PI = 3.14159265f;

// the attack trajectory of a tongued start without a seed (warm 0, no seed for the note, a re-tongue):
// what the player does with the lips and the breath over the first tens of ms after the release (age =
// time since the release), searched with the onset metrics (single notes and the recorded takes)
struct Attack {
	real l0, lh, lt; // lip tuning offset at the release (cents), held for lh (s), relaxing with lt (s)
	real pa, ph, pt; // mouth pressure overshoot (fraction), hold, decay
	real ha, hh, ht; // lip rest opening offset (mm), hold, decay
	real sd;         // seed swing amplitude scale (x SEED_LIP)
	real sf;         // seed swing frequency: 0 the lips' own, 1 the note's
	real ovt;        // the strike's overshoot decay (s)
	real hc;         // the lips firm up against the overshoot: 1 keeps the static opening
	real bf, bft;    // breath floor at the release (x strike), its decay (s)
	real lead;       // the breath leads the key (x pressure velocity) during the first 60 ms
	real g0, tau;    // tongue: gate at the release, opening time (s) of a soft "d"
	real tp, tpt;    // pitch placement: slide/lip trim offset at the release (cents), its decay (s)
	real pr;         // pitch loop: largest step per period during the attack (cents)
	real bs;         // the breath at the release runs ahead of the breath smoother (0..1 of the raw key breath)
};
// a warm (seeded) start: the player starts the note a little softer than meant and blows up into it,
// so the upper harmonics arrive later (as in a real attack) while the note speaks at once
struct WarmAtk {
	real ws; // seed breath re the breath meant (soft .. loud: ws + wsl x dyn)
	real wsl;
	real tr;   // breath rise from the seed to the breath meant (s, at dyn 0) ..
	real trl;  //   .. shortening with the breath (s per dyn)
	real hold; // the breath meant is held while the key is still going down (1), or decays (0)
	real bft;  // its decay once the key has settled (s)
};
#ifdef BRASS_DEBUG
template <class F> static Attack dbg_atk = F::ATK_DEF;
template <class F> static WarmAtk dbg_watk = F::WATK_DEF;
template <class F> [[nodiscard]] static inline const Attack& attack_shape() { return dbg_atk<F>; }
template <class F> [[nodiscard]] static inline const WarmAtk& warm_shape() { return dbg_watk<F>; }
#else
template <class F> [[nodiscard]] static inline const Attack& attack_shape() { return F::ATK_DEF; }
template <class F> [[nodiscard]] static inline const WarmAtk& warm_shape() { return F::WATK_DEF; }
#endif

enum Mute : int { MUTE_NONE = 0,
	MUTE_HARMON,
	MUTE_CUP,
	MUTE_STRAIGHT,
	MUTE_PLUNGER };

template <class P> struct Preset {
	const char* name;
	int bore;
	int mute;
	P p;
};

// ---------------------------------------------------------------------------------------------
// player and key state

template <class F> struct Player {
	// lips and cup: outward mode (x, v), inward mode (y, vy)
	real x, v, pc, y, vy;
	// front end rings
	float ff[MAXSEC][SLEN], fb[MAXSEC][SLEN];
	uint32_t w; // ring / delay line write index
	// cylinder reads: current and (during a valve move) previous length
	real df, dprev, dnl; // forward nominal delay, the valve's other length, steepened read
	real vmix;           // valve move 0..1 (1 = arrived)
	real vrate;
	// multirate bell
	float dh[2 * DHL];
	float h12[2 * F::H12L];
	float ih[8]; // interpolator input (12 kHz reflections, newest first)
	int dpos, hpos, phase;
	// pitch tracking
	real xlp, xhp, xprev; // tracker filters
	real tlast;           // time of the last upward crossing (samples, relative to block start)
	real pprev;           // the last period (samples)
	int cross_n;
	real per_acc;
	real fmeas;
	real trim; // played offset (cents): learned + PLL
	real lipc; // lip correction (cents), for a wrong partial
	int wrong_blocks, good_blocks;
	// articulation
	real gate, gate_t; // breath gate (tongue) and its target
	real tau_on;       // current gate time constant (s)
	real ov;           // overshoot envelope
	real ovt;          // its target, rising in over a few ms (warm start; 0 otherwise)
	real tn;           // tongue noise envelope
	int stop_blocks;   // re-tongue: blocks with the tongue against the lips
	real seed;         // tongued start from silence: seed strength, applied in the next block
	real bfloor;       // breath at a tongued start (from the strike), decaying into the key pressure
	real atk;          // 1: an unseeded tongued start (the attack trajectory runs), 0: slurred or seeded
	real wb0, wb1;     // warm start: the seed's breath and the breath meant (key pressure units)
	int idle_blocks;   // released and silent for this many blocks
	real b1, b2;       // breath smoother (SVF)
	real by1, by2;     // tilt smoother
	real pm;           // mouth pressure at the end of the last block (Pa)
	real w2;           // lip omega^2 at the end of the last block
	// fingering
	int combo, k, ext; // valves, partial, extension (semitones, 0/6/12)
	real nominal;      // MIDI pitch of the chosen resonance
	real note;         // target note
	real note_prev;
	// gestures
	real gest_t, gest_len, gest_range; // fall / doit after release
	real bx_last, pres_last;
	// noise
	uint32_t rng;
	real n1, n2;   // noise filter states
	real um;       // mean jet flow: the turbulence rides on the flow pulses
	real gr1, gr2; // growl band-pass states
	real vib_ph;
	real sh_ph; // lip shake phase
	// ownership
	int owner; // key voice, -1: released
	int stack[NSTACK];
	int nstack;
	uint32_t t_on; // block counter at the start of the note
	int active;
	real peak;
	real xamp; // lip oscillation amplitude (m)
	// warm start (the family's seed table): a tongued note from silence starts in its own steady oscillation
	int warm;            // this note was started from a seed
	real wdyn;           // the strike, then the breath the seed was chosen for (key pressure units)
	real rf, rf1;        // radiated fade-in (two one-poles at block rate; 1 = full)
	real rf_tau;         // its time constant (s)
	int wpre;            // a warm start on a player still ringing: its tail fades out over one block first
	float wt[2][68];     // one period of the seeded cylinder waves (forward input, reflection)
	uint32_t wfi, wbi;   // next sample of the forward / backward line still to seed
	int wfn, wbn;        // samples left to seed
	real wfu, wbu, winc; // their table phases, the phase step per sample
	real level;          // smoothed output level (stealing)
	// slide instruments: the slide's extension and where the player wants it (one-way samples), the
	// cents the moving slide still lags the note (the lips follow the slide), placed at once next block
	real sl, sl_to, lag_c;
	int sl_jump;
	// the player layer (F::LAYER): the last transition, its context and what it does to the breath
	int tr_kind;      // Transition
	int tr_pending;   // classified in the next block, when the new fingering is known
	real tr_strike;   // its strike
	real tr_gap;      // s since this player's last release (< 0: an overlap, the old key still held)
	real tr_interval; // semitones from the previous note (signed)
	int tr_k0, tr_c0; // the previous partial and combination
	real tr_age;      // s since the transition
	real carry;       // M1: the breath carried across the transition (key pressure units)
	real ae_d, ae_push, ae_len; // M2: transition envelope (breath dip, push after it, length s)
	int dip_blocks;   // legato tongue ("doo"): blocks the tongue dips the air
	real dip_g;       // the gate it dips to
	uint32_t t_off;   // block of the last release
};

// what a new note's start is (the player layer's transition classifier, F::LAYER)
enum Transition : int { TR_ONSET = 0, // tongued from silence
	TR_SLUR,                         // connected, no tongue: the lips change partial (or the valves move)
	TR_LTONGUE,                      // connected, a soft legato tongue ("doo"): the air dips while the slide moves
	TR_RETONGUE,                     // a new tongued note soon after the last one: the breath carries
	TR_SMEAR };                      // connected, no tongue, the slide (lips) glide: a gliss

struct KeyState {
	real pres_prev;
	real note_on; // note at the onset
	real vpk;     // peak strike while pending
	int pending;  // blocks waiting for the strike peak
	int held;
	int player; // player sounding this key, -1 none
};

template <class F> struct Engine {
	static_assert(F::NDEC == 48 && F::DEC == 8 && F::NFIR % 4 == 0 && F::NFIR <= F::H12L, "multirate bell layout");
	static_assert(F::MAXSEC >= MAXSEC, "section table");
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	int bore_i;
	int mute_type;
	typename F::Params p;  // as set
	typename F::Params ps; // smoothed (de-zippered live edits)
	real sr, dt, block_dt;
	uint32_t blocks;
	strisy::SvfCoef bc, tc; // breath and tilt smoothers (block rate)
	real shake_pk, shake;
	real combo_semis[F::NCOMBO];
	real valve_len[F::NCOMBO]; // one-way samples
	real open_midi;
	real leff; // one-way effective length of the open horn (samples) at the nominal pitch
	real sl_max, lvloss; // slide instruments: the slide's reach (one-way samples), log2 of the added tubing's loss per sample
	real learn[F::NCOMBO][F::NK];
	real fpitch[F::NCOMBO][F::NK]; // fingering chart (MIDI), -1000: not playable
	float fir_rev[F::NFIR];        // bell reflection, time reversed (oldest sample first)
	real fcost[F::NCOMBO][F::NK];
	Player<F> pl[F::NP];
	KeyState ks[MAX_VOICES];
	// output chain
	real mix[NS];
	real hb[64]; // halfband history (double buffered 32)
	int hbpos;
	real rad_s[3][2]; // radiation biquads (DF2T)
	real bell_s[2];
	real mute_s[4][2]; // mute SVF states
	real fit_s[2][2];  // fit EQ states
	real plunge;       // plunger position (smoothed)
	real room_lp, dc;
	uint32_t room_w;
	real quiet;     // blocks without sound (state flush)
	int wseeds;     // warm starts in this block (at most F::WARM_PER_BLOCK: the others wait a block)
	float wtab[68]; // warm start: one period of the seeded cup pressure (scratch)
};

// ---------------------------------------------------------------------------------------------
// helpers (no libm)

[[nodiscard]] static inline real exp2s(real p) {
	p = rmin(rmax(p, -60.0f), 60.0f);
	const int wi = (int)(p + 64.0f) - 64;
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
	} v = {x};
	const real e = (real)(int)((v.i >> 23) & 255) - 127.0f;
	v.i = (v.i & 0x007FFFFFu) | 0x3F800000u; // mantissa in [1, 2)
	const real m = v.f;
	return e + 1.44269504f * (-1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

// 1 during the hold, then an exponential decay with time constant t
[[nodiscard]] static inline real atk_env(real age, real hold, real t) {
	return age < hold ? 1.0f : exp2s(-(age - hold) * 1.442695f / rmax(t, 0.0005f));
}

[[nodiscard]] static inline real midi_hz(real n) { return 8.1757989f * exp2s(n * (1.0f / 12)); }

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
	const real s = -8.0f * q * (1.0f - 2.0f * fabsf(q));
	return s * (0.775f + 0.225f * fabsf(s));
}

[[nodiscard]] static inline real noise(uint32_t& r) {
	r = r * 1664525u + 1013904223u;
	return (real)(int32_t)r * 4.656613e-10f;
}

[[nodiscard]] static inline real clampr(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }

template <class F> [[nodiscard]] static inline const typename F::Bore& bore_of(const Engine<F>& e) { return F::BORES[e.bore_i]; }

#ifdef BRASS_DEBUG
static int dbg_nocal = 0;                                                    // 1: no calibration, 2: the values below
static float dbg_cal_lip = 0.0f, dbg_cal_trim = 0.0f, dbg_cal_pthr = 600.0f; // (calibration runs)
#endif

// the player's knowledge of the horn (trumpet_cal.h): lip setting, slide trim and the pressure where
// the note speaks, interpolated between the calibrated notes
struct CalPoint {
	real trim, lip, pthr;
};
template <class F> [[nodiscard]] static inline CalPoint cal_at(const Engine<F>& e, real note) {
	const typename F::Cal& c = F::CAL[e.preset];
	const real x = clampr(note - c.lo, 0.0f, (real)(F::NCAL - 1) - 0.001f);
	const int i = (int)x;
	const real f = x - (real)i;
	CalPoint p{c.trim[i] + f * (c.trim[i + 1] - c.trim[i]), c.lip[i] + f * (c.lip[i + 1] - c.lip[i]),
	    c.pthr[i] + f * (c.pthr[i + 1] - c.pthr[i])};
#ifdef BRASS_DEBUG
	if (dbg_nocal == 1) p = {0.0f, 0.0f, 1200.0f * exp2s((note - 70.0f) * (1.0f / 18))};
	if (dbg_nocal == 2) p = {dbg_cal_trim, dbg_cal_lip, dbg_cal_pthr};
#endif
	return p;
}

// ---------------------------------------------------------------------------------------------
// setup

template <class F> static inline void reset_player(Engine<F>& e, int i) {
	Player<F>& s = e.pl[i];
	const uint32_t rng = s.rng ? s.rng : 0x9E3779B9u * (uint32_t)(i + 1) + 12345u;
	const real vph = s.vib_ph;
	s = Player<F>{};
	s.rng = rng;
	s.vib_ph = vph;
	s.owner = -1;
	s.k = 4;
	s.note = s.note_prev = 70.0f;
	s.df = s.dprev = s.dnl = bore_of(e).dpure;
	s.vmix = 1.0f;
	s.fmeas = 0.0f;
	s.rf = s.rf1 = 1.0f;
	for (int n = 0; n < F::CLEN; n++) F::cyl_f[i][n] = F::cyl_b[i][n] = 0.0f;
}

template <class F> static inline void update_bore(Engine<F>& e) {
	const typename F::Bore& b = bore_of(e);
	static constexpr int combo_valves[F::NCOMBO][3] = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}};
	static constexpr real nominal_semis[F::NCOMBO] = {0, 1, 2, 3, 4, 5, 6};
	for (int c = 0; c < F::NCOMBO; c++) {
		e.valve_len[c] = 0.0f;
		for (int v = 0; v < 3; v++)
			if (combo_valves[c][v]) e.valve_len[c] += b.valve_d[v];
		e.combo_semis[c] = nominal_semis[c];
	}
	e.open_midi = 69.0f + 12.0f * log2s(b.f_nom / 440.0f);
	for (int k = 0; k < F::NFIR; k++) e.fir_rev[k] = b.fir[F::NFIR - 1 - k];
	// the player's fingering chart: pitch of every resonance with every valve combination, and the
	// preference cost (more valves, the out-of-tune 7th/11th partials, the crowded top)
	static constexpr real pen_combo[F::NCOMBO] = {0, 3, 4, 9, 22, 30, 40};
	for (int c = 0; c < F::NCOMBO; c++) {
		for (int k = 0; k < F::NK; k++) {
			const real rc = b.res_c[c][k];
			const bool ok = k >= 2 && rc < 900.0f;
			e.fpitch[c][k] = ok ? e.open_midi - (real)c + 12.0f * log2s((real)rmax((real)k, 1.0f)) + 0.01f * rc : -1000.0f;
			real cost = pen_combo[c];
			if (k == 7 || k == 11) cost += 45.0f;
			if (k == 13 || k == 14) cost += 30.0f;
			if (k > 8) cost += 4.0f * (real)(k - 8);
			e.fcost[c][k] = cost;
		}
	}
	e.leff = (real)F::FS / (2.0f * b.f_nom);
	for (int c = 0; c < F::NCOMBO; c++)
		for (int k = 0; k < F::NK; k++) e.learn[c][k] = 0.0f; // learned on top of the calibration
	if constexpr (F::HAS_SLIDE) {
		e.sl_max = 0.0f;
		e.lvloss = log2s(b.vloss);
		if (b.slide) {
			// a slide instrument: the combinations are the triggers (none, valve 1 = F, valve 2, both),
			// the slide's continuous length does the rest (choose_slide); the added length in semitones
			static constexpr int trig[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
			for (int c = 0; c < F::NCOMBO; c++) {
				e.valve_len[c] = c < 4 ? (real)trig[c][0] * b.valve_d[0] + (real)trig[c][1] * b.valve_d[1] : 0.0f;
				e.combo_semis[c] = 12.0f * log2s((e.leff + e.valve_len[c]) / e.leff);
				for (int k = 0; k < F::NK; k++) e.fpitch[c][k] = -1000.0f;
			}
			e.sl_max = e.leff * (exp2s(b.slide_max * (1.0f / 12)) - 1.0f);
		}
	}
}

template <class F> static inline void update_derived(Engine<F>& e) {
	e.bc = strisy::svf_from_g(strisy::tan0(PI / e.sr * BLOCK * rmin(e.p.resp_hz, 300.0f)), 0.5f);
}

template <class F> static inline void set_preset(Engine<F>& e, int i) {
	if (i < 0 || i >= F::NUM_PRESETS) i = 0;
	const Preset pr = F::preset_def(i);
	const int old_bore = e.bore_i;
	e.preset = i;
	e.p = pr.p;
	e.ps = e.p;
	e.bore_i = pr.bore;
	e.mute_type = pr.mute;
	if (pr.bore != old_bore || e.blocks == 0) {
		update_bore(e);
		// sounding players move to the new bore's lengths through a valve move
		for (int v = 0; v < F::NP; v++) {
			Player<F>& s = e.pl[v];
			s.dprev = rmin(s.df, F::DMAXC);
			s.vmix = 0.0f;
			s.vrate = 1.0f / 16.0f;
		}
	}
	update_derived(e);
}

template <class F> static inline void init(Engine<F>& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.dt = 1.0f / (e.sr * OS);
	e.block_dt = (real)BLOCK / e.sr;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	e.blocks = 0;
	e.bore_i = 0;
	e.tc = strisy::svf_from_g(strisy::tan0(PI / e.sr * BLOCK * 10.0f), 0.5f);
	e.shake_pk = e.shake = 0.0f;
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.ks[v] = KeyState{};
		e.ks[v].player = -1;
	}
	for (int n = 0; n < NS; n++) e.mix[n] = 0.0f;
	for (int n = 0; n < 64; n++) e.hb[n] = 0.0f;
	e.hbpos = 0;
	for (auto& r : e.rad_s) r[0] = r[1] = 0.0f;
	e.bell_s[0] = e.bell_s[1] = 0.0f;
	for (auto& m : e.mute_s) m[0] = m[1] = 0.0f;
	for (auto& m : e.fit_s) m[0] = m[1] = 0.0f;
	e.plunge = 0.0f;
	e.room_lp = e.dc = 0.0f;
	e.room_w = 0;
	e.quiet = 0.0f;
	for (int n = 0; n < ROOML; n++) F::room_buf[n] = 0.0f;
	set_preset(e, F::DEFAULT_PRESET);
	for (int i = 0; i < F::NP; i++) {
		e.pl[i].rng = 0;
		e.pl[i].vib_ph = 0.23f * (real)i;
		reset_player(e, i);
	}
	e.blocks = 1;
}

template <class F> [[nodiscard]] static inline int active_voices(const Engine<F>& e) {
	int n = 0;
	for (int v = 0; v < F::NP; v++) n += e.pl[v].active;
	return n;
}

// ---------------------------------------------------------------------------------------------
// the player's choices: valves, partial, extension

struct Fingering {
	int combo, k, ext;
	real nominal;
};

// slide instruments (F::HAS_SLIDE, the bore's slide flag): resonance k's deviation (cents) with u
// semitones of added length (slide + triggers), interpolated in the bore's chart; >= 900: unusable
template <class F> [[nodiscard]] static inline real res_u_at(const typename F::Bore& b, int k, real u) {
	const real x = clampr(u, 0.0f, (real)(F::NU - 1) - 0.001f);
	const int i = (int)x;
	const real a = b.res_u[i][k], c = b.res_u[i + 1][k];
	if (a > 900.0f || c > 900.0f) return 999.0f;
	return a + (x - (real)i) * (c - a);
}
// the pitch (MIDI) of resonance k with u semitones of added length
template <class F> [[nodiscard]] static inline real slide_pitch(const Engine<F>& e, int k, real u) {
	return e.open_midi + 12.0f * log2s((real)k) - u + 0.01f * res_u_at<F>(bore_of(e), k, u);
}
// the slide extension (one-way samples, not yet limited to the slide's reach) that puts resonance k of
// trigger combination c on the note, and what the lips must add (semitones, + up) where it can't reach
struct SlideSol {
	real sl, lip;
	bool ok;
};
template <class F> [[nodiscard]] static inline SlideSol slide_solve(const Engine<F>& e, int c, int k, real note) {
	const typename F::Bore& b = bore_of(e);
	const real u0 = e.open_midi + 12.0f * log2s((real)k) - note;
	real r = res_u_at<F>(b, k, u0);
	if (r > 900.0f) return {0.0f, 0.0f, false};
	r = res_u_at<F>(b, k, u0 + 0.01f * r);
	if (r > 900.0f) return {0.0f, 0.0f, false};
	const real u = u0 + 0.01f * r;
	const real sl = e.leff * (exp2s(u * (1.0f / 12)) - 1.0f) - e.valve_len[c];
	real lip = 0.0f;
	if (sl < 0.0f) lip = note - slide_pitch(e, k, e.combo_semis[c]);
	else if (sl > e.sl_max) lip = note - slide_pitch(e, k, 12.0f * log2s((e.leff + e.valve_len[c] + e.sl_max) / e.leff));
	return {sl, lip, true};
}
// the slide position (semitones out from 1st position) of an extension
template <class F> [[nodiscard]] static inline real slide_pos(const Engine<F>& e, real sl) {
	return 12.0f * log2s((e.leff + clampr(sl, 0.0f, e.sl_max)) / e.leff);
}
// a trombonist's choice: trigger, partial and slide position. Short positions and few triggers first
// (the triggers replace the long 6th and 7th positions), the pedal only where nothing else reaches,
// the out-of-tune 7th and 11th partials as alternates; a held note keeps its partial while the slide
// reaches (a gliss), and breaks to another partial where it can't
template <class F> static inline Fingering choose_slide(const Engine<F>& e, real note, const Player<F>& s, bool keep) {
	const typename F::Bore& b = bore_of(e);
	static constexpr real pen_trig[4] = {0.0f, 14.0f, 16.0f, 26.0f};
	Fingering best = {0, 2, 0, note};
	real best_cost = 1e9f;
	const int nc = b.ncombo < 4 ? b.ncombo : 4;
	for (int c = 0; c < nc; c++) {
		for (int k = 1; k < F::NK; k++) {
			const SlideSol so = slide_solve(e, c, k, note);
			if (!so.ok || fabsf(so.lip) > 1.2f) continue;
			const real pos = slide_pos(e, so.sl);
			real cost = 100.0f * fabsf(so.lip) + pen_trig[c] + 3.0f * pos + 12.0f * rmax(pos - 4.5f, 0.0f);
			if (k == 1) cost += 20.0f;
			if (k == 7) cost += 25.0f;
			if (k == 11 || k >= 13) cost += 30.0f;
			if (k > 9) cost += 4.0f * (real)(k - 9);
			if (keep && c == s.combo && k == s.k) cost -= 35.0f;
			if (cost < best_cost) {
				best_cost = cost;
				best = {c, k, 0, note - so.lip};
			}
		}
	}
	return best;
}

template <class F> static inline Fingering choose_fingering(const Engine<F>& e, real note, const Player<F>& s, bool keep) {
	if constexpr (F::HAS_SLIDE) {
		if (bore_of(e).slide) return choose_slide(e, note, s, keep);
	}
	// lowest normal note: partial 2 with 1-2-3; below it the horn grows a 4th (and 5th) valve loop
	const real lowest = e.open_midi + 12.0f - 6.0f - 0.6f;
	int ext = 0;
	if (note < lowest) ext = note < lowest - 6.0f ? 12 : 6;
	const real nn = note + (real)ext; // in the open horn's terms
	Fingering best = {0, 2, ext, e.open_midi + 12.0f - (real)ext};
	real best_cost = 1e9f;
	for (int c = 0; c < F::NCOMBO; c++) {
		for (int k = 2; k < F::NK; k++) {
			const real d = fabsf(nn - e.fpitch[c][k]);
			if (d > 1.2f) continue;
			real cost = 100.0f * d + e.fcost[c][k];
			if (keep && c == s.combo && k == s.k && ext == s.ext) cost -= 35.0f;
			if (cost < best_cost) {
				best_cost = cost;
				best = {c, k, ext, e.fpitch[c][k] - (real)ext};
			}
		}
	}
	return best;
}

// note range of the instrument (octave folding outside it)
template <class F> [[nodiscard]] static inline real fold_note(const Engine<F>& e, real n) {
	// the range where the notes speak, re the open horn's nominal pitch (per bore: the family's)
	const real lo = e.open_midi + F::range_lo(e.bore_i);
	const real hi = e.open_midi + F::range_hi(e.bore_i);
	while (n < lo) n += 12.0f;
	while (n > hi) n -= 12.0f;
	return n;
}

// ---------------------------------------------------------------------------------------------
// one player, one block

template <class F> static inline void start_note(Engine<F>& e, int i, real hard, bool tongue, bool from_silence) {
	Player<F>& s = e.pl[i];
	const typename F::Params& P = e.ps;
	s.warm = 0;
	if (tongue) {
		const real h = clampr(hard * P.attack, 0.0f, 1.5f);
		s.wdyn = h; // the strike, until the warm start picks the breath
		const Attack& A = attack_shape<F>();
		s.tau_on = A.tau * exp2s(-3.3f * h); // 20 ms "d" .. 2 ms "t"
		s.ov = P.overshoot * h;
		s.atk = from_silence || s.gate < 0.3f ? 1.0f : 0.0f;
		s.ovt = 0.0f;
		s.bfloor = from_silence || s.gate < 0.3f ? clampr(A.bf * h, 0.0f, 0.9f) : 0.0f;
		s.tn = h;
		if (!from_silence && s.gate > 0.05f) s.stop_blocks = 6 + (int)(10.0f * (1.0f - rmin(h, 1.0f))); // re-tongue
		if (from_silence) {
			// the tongue releases air already under pressure: the full breath hits the lips at once,
			// which the tongue held tensed and ready to swing; the seed (applied in the first block,
			// when the fingering is known) starts the lips swinging at their frequency
			s.gate = clampr(A.g0 + 0.25f * h, 0.0f, 0.97f);
			s.seed = clampr(0.4f + 0.6f * h, 0.0f, 1.0f);
		}
	}
	if (!tongue) s.atk = 0.0f; // a slur: no attack trajectory
	if constexpr (F::HAS_SLIDE) {
		if (from_silence) s.sl_jump = 1; // the slide is in place before the note
	}
	s.t_on = e.blocks;
	s.active = 1;
	s.good_blocks = s.wrong_blocks = 0;
	s.gest_len = 0.0f;
}

// the front-end junctions for one sample (N sections; N = 0: any number, n)
template <int N, class F> static inline void ladder(Player<F>& s, uint32_t w, const int* __restrict__ d, const real* __restrict__ k, int n = N) {
	const uint32_t wm = w & SMASK;
	const int nj = N > 0 ? N - 1 : n - 1;
#pragma GCC unroll 8
	for (int j = 0; j < (N > 0 ? N - 1 : MAXSEC - 1); j++) {
		if (N == 0 && j >= nj) break;
		const real fa = s.ff[j][(w - (uint32_t)d[j]) & SMASK];
		const real ba = s.fb[j + 1][(w - (uint32_t)d[j + 1]) & SMASK];
		const real t = k[j] * (fa - ba);
		s.ff[j + 1][wm] = fa + t;
		s.fb[j][wm] = ba + t;
	}
}

#ifdef BRASS_DEBUG
template <class F> static float dbg_rb = F::LIP_IN_R; // inward lip mode: ratio, cents, Q (0: lip_q)
template <class F> static float dbg_cb = F::LIP_IN_C;
static float dbg_qb = 0.0f;
static int dbg_norefl = 0;
template <class F> static float dbg_seed = F::SEED_LIP;
static int dbg_impulse = 0, dbg_nopll = 0;
static long dbg_n = 0, dbg_len = 0;
static float* dbg_pc = nullptr;
static float* dbg_u = nullptr;
static float* dbg_x = nullptr;
static float* dbg_f = nullptr;  // cylinder forward input (CF writes)
static float* dbg_r = nullptr;  // bell reflection (CB writes)
static float* dbg_xo = nullptr; // outward lip mode (x; dbg_x is the opening x + y)
static float* dbg_y = nullptr;  // inward lip mode
#endif

// ---------------------------------------------------------------------------------------------
// warm start: a tongued note from silence starts in the horn's own steady oscillation for this
// note and breath, captured offline from this model (trumpet_seed.h: the first harmonics of the
// cylinder waves, the cup pressure and both lip modes, phase aligned to the lips' fundamental).
// From rest the long bore of the low notes needs several round trips (~25 ms) to fill; seeded on
// its own limit cycle the note speaks at once. Only the lowest harmonics are seeded (more for a
// harder, louder attack): the lips and the bore grow the rest within a few periods, so the
// brightness builds as in a real attack; the radiated sound fades in over a few ms (the tongue
// release), so the seeded bore doesn't click.

template <class F> struct WarmSeed {
	real trim, um;
	real dc[F::NSIG];
	real re[F::NSIG][F::NH], im[F::NSIG][F::NH];
};

#ifdef BRASS_DEBUG
template <class F> static const typename F::SeedState* dbg_seed_ovr = nullptr; // capture: seed from this state
static int dbg_nowarm = 0;
static float dbg_force_dyn = -1.0f, dbg_bright = -1.0f, dbg_walpha = 1.0f; // capture: the seed's breath and harmonics
#endif

// the seed for this note and breath (interpolated between the captured breaths); false: none
template <class F> static inline bool warm_lookup(const Engine<F>& e, real note, real dyn, const Fingering& fg, WarmSeed<F>& w) {
	const typename F::SeedState* a = nullptr;
	const typename F::SeedState* c = nullptr;
	real u = 0.0f;
#ifdef BRASS_DEBUG
	if (dbg_nowarm) return false;
	if (dbg_seed_ovr<F>) a = c = dbg_seed_ovr<F>;
#endif
	if (a == nullptr) {
		// the nearest captured note with this fingering (a bent note may sit nearer the next semitone)
		const typename F::SeedSet& st = F::SEEDS[e.preset];
		if (st.st == nullptr) return false;
		const int n0 = (int)(note + 0.5f) - st.lo, n1 = note + 0.5f - (real)(int)(note + 0.5f) < 0.5f ? n0 - 1 : n0 + 1;
		int n = -1;
		for (int c = 0; c < 2 && n < 0; c++) {
			const int m = c ? n1 : n0;
			if (m < 0 || m >= st.n) continue;
			for (int d = 0; d < F::NDYN; d++) {
				const typename F::SeedState& q = st.st[m * F::NDYN + d];
				if (q.ok && q.combo == fg.combo && q.k == fg.k && q.ext == fg.ext) n = m;
			}
		}
		if (n < 0) return false;
		const typename F::SeedState* row = &st.st[n * F::NDYN];
		int j = 0;
		while (j < F::NDYN - 2 && dyn > F::DYN[j + 1]) j++;
		u = clampr((dyn - F::DYN[j]) / (F::DYN[j + 1] - F::DYN[j]), 0.0f, 1.0f);
		a = &row[j];
		c = &row[j + 1];
		if (!a->ok) {
			a = c;
			u = 0.0f;
		}
		if (!c->ok) c = a;
	}
	if (!a->ok || a->combo != fg.combo || a->k != fg.k || a->ext != fg.ext) return false;
	if (c->combo != a->combo || c->k != a->k || c->ext != a->ext) c = a;
	w.trim = a->trim + u * (c->trim - a->trim);
	w.um = a->um + u * (c->um - a->um);
	for (int q = 0; q < F::NSIG; q++) {
		w.dc[q] = a->dc[q] + u * (c->dc[q] - a->dc[q]);
		for (int k = 0; k < F::NH; k++) {
			w.re[q][k] = a->re[q][k] + u * (c->re[q][k] - a->re[q][k]);
			w.im[q][k] = a->im[q][k] + u * (c->im[q][k] - a->im[q][k]);
		}
	}
	return true;
}

// the fingering of the nearest captured note, if it reaches this one (lipped within 1.2 semitones)
template <class F> [[nodiscard]] static inline bool warm_fingering(const Engine<F>& e, real note, Fingering& f) {
	const typename F::SeedSet& st = F::SEEDS[e.preset];
	const int n = (int)(note + 0.5f) - st.lo;
	if (st.st == nullptr || n < 0 || n >= st.n) return false;
	for (int d = 0; d < F::NDYN; d++) {
		const typename F::SeedState& q = st.st[n * F::NDYN + d];
		if (!q.ok || q.combo < 0 || q.combo >= F::NCOMBO || q.k < 0 || q.k >= F::NK) continue;
		const real fp = e.fpitch[q.combo][q.k];
		if (fp < -100.0f || fabsf(note + (real)q.ext - fp) > 1.2f) continue;
		f = {q.combo, q.k, q.ext, fp - (real)q.ext};
		return true;
	}
	return false;
}

// a seed exists for this note (some breath)
template <class F> [[nodiscard]] static inline bool warm_has(const Engine<F>& e, real note) {
	const typename F::SeedSet& st = F::SEEDS[e.preset];
	const int n = (int)(note + 0.5f) - st.lo;
	if (st.st == nullptr || n < 0 || n >= st.n) return false;
	for (int d = 0; d < F::NDYN; d++)
		if (st.st[n * F::NDYN + d].ok) return true;
	return false;
}

// a phase into a 64-point period table (64 frac(t / per)) and a read of the table there
[[nodiscard]] static inline real wph(real t, real iper) {
	real u = t * iper;
	u -= (real)(int)u;
	if (u < 0.0f) u += 1.0f;
	return u * 64.0f;
}
[[nodiscard]] static inline real wlerp(const float* T, real u) {
	const int j = (int)u & 63;
	return T[j] + (u - (real)j) * (T[j + 1] - T[j]);
}

// one period (64 points and the wrap point) of a seeded signal, harmonic k weighted by g[k]
template <class F> static inline void wtab_make(float* __restrict__ T, real dc, const real* re, const real* im, const real* g) {
	constexpr int NH = F::NH;
	real ca[NH], sb[NH];
	for (int k = 0; k < NH; k++) {
		ca[k] = g[k] * re[k];
		sb[k] = g[k] * im[k];
	}
	// T[j] = dc + sum a cos(2 pi (k + 1) j / 64) - b sin(..); T[64 - j]: the same cosines, sines negated
	for (int j = 0; j <= 32; j++) {
		real c = 0.0f, sn = 0.0f;
		int ci = 0;
		for (int k = 0; k < NH; k++) {
			ci = (ci + j) & 63;
			c += ca[k] * F::COS64[ci];
			sn += sb[k] * F::COS64[(ci + 48) & 63];
		}
		T[j] = dc + c - sn;
		if (j > 0 && j < 32) T[64 - j] = dc + c + sn;
	}
	T[64] = T[0];
}

// a seeded cylinder line, filled from its oldest sample on (spread over the blocks after the seed,
// ahead of the reads)
template <class F> static inline void wline_fill(float* __restrict__ L, uint32_t& idx, int& left, real& u, real inc, const float* __restrict__ T, int n) {
	n = n < left ? n : left;
	left -= n;
	uint32_t j = idx;
	real v = u;
	for (; n > 0; n--) {
		L[j & F::CMASK] = wlerp(T, v);
		j++;
		v += inc;
		if (v >= 64.0f) v -= 64.0f;
	}
	idx = j;
	u = v;
}

// a seeded lip mode at t = 0 and t = -1 (x, v): its harmonics summed, cos / sin of k 2 pi / per by
// recurrence from a short series (per >= ~60 samples)
template <class F> static inline void wlip(const WarmSeed<F>& w, int q, const real* g, real per, real& x0, real& v0) {
	const real th = 2.0f * PI / per, t2 = th * th;
	const real c1 = 1.0f - t2 * (0.5f - t2 * (1.0f / 24)), s1 = th * (1.0f - t2 * (1.0f / 6 - t2 * (1.0f / 120)));
	real ck = 1.0f, sk = 0.0f, a = w.dc[q], m1 = w.dc[q];
	for (int k = 0; k < F::NH; k++) {
		const real cn = ck * c1 - sk * s1;
		sk = sk * c1 + ck * s1;
		ck = cn;
		a += g[k] * w.re[q][k];
		m1 += g[k] * (w.re[q][k] * ck + w.im[q][k] * sk); // the harmonic one sample earlier
	}
	x0 = a;
	v0 = (a - m1) * (real)F::FS;
}

// cos and sin of 2 pi ph (polynomial, ~1e-3)
static inline void wcs(real ph, real& c, real& sn) {
	ph -= (real)(int)ph;
	if (ph < 0.0f) ph += 1.0f;
	sn = psin(ph);
	ph += 0.25f;
	if (ph >= 1.0f) ph -= 1.0f;
	c = psin(ph);
}

// the front end's steady state for the seeded waves at the cylinder junction: per harmonic (and the
// mean), from the junction back to the cup, each Kelly-Lochbaum junction solved for the waves
// arriving at it (ff[j + 1] = fa + t, fb[j] = ba + t, t = k (fa - ba)), the sections' delays as
// phase turns; then the rings' last d samples (all the ladder reads) synthesised
template <class F> static inline void warm_front(Player<F>& s, const typename F::Bore& b, const WarmSeed<F>& w, const real* g, real per, real df) {
	constexpr int NH = F::NH;
	const int nsec = b.nsec;
	const real iper = 1.0f / per;
	// per harmonic: the forward wave into the cylinder, the wave returning from it (r delayed by df)
	real fre[NH + 1], fim[NH + 1], bre[NH + 1], bim[NH + 1]; // index 0: the mean
	fre[0] = w.dc[0];
	fim[0] = 0.0f;
	bre[0] = w.dc[1];
	bim[0] = 0.0f;
	for (int k = 1; k <= NH; k++) {
		real c, sn;
		wcs(-(real)k * df * iper, c, sn);
		fre[k] = g[k - 1] * w.re[0][k - 1];
		fim[k] = g[k - 1] * w.im[0][k - 1];
		const real rr = g[k - 1] * w.re[1][k - 1], ri = g[k - 1] * w.im[1][k - 1];
		bre[k] = rr * c - ri * sn;
		bim[k] = rr * sn + ri * c;
	}
	// junctions from the cylinder end: harmonics of ff[j] and fb[j] (at t = 0)
	real FFr[MAXSEC][NH + 1], FFi[MAXSEC][NH + 1], FBr[MAXSEC][NH + 1], FBi[MAXSEC][NH + 1];
	for (int k = 0; k <= NH; k++) {
		real outr = fre[k], outi = fim[k], bar = bre[k], bai = bim[k]; // the wave leaving right, arriving from right
		for (int j = nsec - 1; j >= 0; j--) {
			const real kj = b.sec_k[j], ik = 1.0f / (1.0f + kj);
			const real far = (outr + kj * bar) * ik, fai = (outi + kj * bai) * ik;
			FBr[j][k] = bar + kj * (far - bar);
			FBi[j][k] = bai + kj * (fai - bai);
			// ff[j] arrives at the junction d_j later: ff[j](t) = fa(t + d_j)
			real c = 1.0f, sn = 0.0f;
			if (k > 0) wcs((real)(k * b.sec_d[j]) * iper, c, sn);
			FFr[j][k] = far * c - fai * sn;
			FFi[j][k] = far * sn + fai * c;
			if (j > 0) {
				// next junction to the left: its outgoing right wave is ff[j], the wave arriving from
				// its right is fb[j] delayed d_j
				outr = FFr[j][k];
				outi = FFi[j][k];
				real c2 = 1.0f, s2 = 0.0f;
				if (k > 0) wcs(-(real)(k * b.sec_d[j]) * iper, c2, s2);
				bar = FBr[j][k] * c2 - FBi[j][k] * s2;
				bai = FBr[j][k] * s2 + FBi[j][k] * c2;
			}
		}
	}
	// the rings: samples t = -1 .. -(d + 1) of each wave (the ladder reads w - d)
	real sc[NH + 1], ss[NH + 1]; // e^{-i k 2 pi / per}
	sc[0] = 1.0f;
	ss[0] = 0.0f;
	for (int k = 1; k <= NH; k++) {
		wcs(-(real)k * iper, sc[k], ss[k]);
	}
	const uint32_t w0 = s.w;
	for (int j = 0; j < MAXSEC; j++) {
		for (int n = 0; n < SLEN; n++) s.ff[j][n] = s.fb[j][n] = 0.0f;
		if (j >= nsec) continue;
		const int len = b.sec_d[j] + 1;
		for (int q = 0; q < 2; q++) {
			float* R = q ? s.fb[j] : s.ff[j];
			real zr[NH + 1], zi[NH + 1];
			for (int k = 1; k <= NH; k++) {
				zr[k] = q ? FBr[j][k] : FFr[j][k];
				zi[k] = q ? FBi[j][k] : FFi[j][k];
			}
			const real dc = q ? FBr[j][0] : FFr[j][0];
			for (int t = 1; t <= len; t++) {
				real v = dc;
				for (int k = 1; k <= NH; k++) {
					const real nr = zr[k] * sc[k] - zi[k] * ss[k];
					zi[k] = zr[k] * ss[k] + zi[k] * sc[k];
					zr[k] = nr;
					v += nr;
				}
				R[(w0 - (uint32_t)t) & SMASK] = v;
			}
		}
	}
}

// writes the seed into player i: the cylinder lines (the oldest parts now, the rest ahead of the
// reads in the next blocks), the front end (run in on the seeded cup pressure and the returning
// wave), the bell's multirate filter histories, the cup, both lip modes and the tracker. Signals
// (trumpet_seed.h): 0 the cylinder's forward input, 1 the bell's reflection, 2 the cup pressure,
// 3 and 4 the outward and inward lip modes. bright 0..1: the seeded harmonics (0: the first two,
// 1: all NH)
template <class F> static inline void warm_fill(Engine<F>& e, int i, const WarmSeed<F>& w, real per, real df, real loss_b, real knl, real bright, real alpha) {
	Player<F>& s = e.pl[i];
	const typename F::Bore& b = bore_of(e);
	constexpr int NH = F::NH;
	real g[NH];
	for (int k = 0; k < NH; k++) g[k] = clampr(bright * (real)(NH - 2) + 2.0f - (real)k, 0.0f, 1.0f);
#ifdef BRASS_DEBUG
	for (int k = 0; k < NH; k++) g[k] *= dbg_walpha;
#endif
	for (int k = 0; k < NH; k++) g[k] *= alpha;
	float* Tf = s.wt[0];
	float* Tr = s.wt[1];
	wtab_make<F>(Tf, w.dc[0], w.re[0], w.im[0], g);
	wtab_make<F>(Tr, w.dc[1], w.re[1], w.im[1], g);
	const real iper = 1.0f / per, inc = 64.0f * iper;
	const uint32_t w0 = s.w;
	// cylinder lines: the forward one as far back as the (steepened) reads reach, the backward one
	// for its reads (seeded in the next blocks, ahead of the reads)
	const int lf = (int)rmin(df + 96.0f, (real)(F::CLEN - 2)), lb = (int)rmin(df + 4.0f, (real)(F::CLEN - 2));
	s.winc = inc;
	s.wfi = w0 - (uint32_t)lf;
	s.wfn = lf;
	s.wfu = wph(-(real)lf, iper);
	s.wbi = w0 - (uint32_t)lb;
	s.wbn = lb;
	s.wbu = wph(-(real)lb, iper);
	// front end: the steady waves of every section, harmonic by harmonic, back from the cylinder
	// junction (the forward wave into the cylinder and the one returning from it are seeded): each
	// Kelly-Lochbaum junction solved for its incoming waves, the sections' delays as phase turns
	warm_front(s, b, w, g, per, df);
	// bell: the decimator input (the wave arriving at the bell), the 12 kHz history (the decimator
	// delays 23.5 samples), the interpolator input (the reflection 23.5 samples ahead of its output)
	{
		real u = wph(-1.0f - df, iper);
		for (int m = 0; m < DHL; m++) {
			const int j = (s.dpos - 1 - m) & (DHL - 1);
			s.dh[j] = s.dh[j + DHL] = wlerp(Tf, u);
			u -= inc;
			if (u < 0.0f) u += 64.0f;
		}
		const real td = (real)(-1 - s.phase);
		const real inc8 = 8.0f * inc - 64.0f * (real)(int)(8.0f * iper);
		u = wph(td - 23.5f - df, iper);
		for (int m = 0; m < F::H12L; m++) {
			const int j = (s.hpos - 1 - m) & (F::H12L - 1);
			s.h12[j] = s.h12[j + F::H12L] = wlerp(Tf, u);
			u -= inc8;
			if (u < 0.0f) u += 64.0f;
		}
		const real il = 1.0f / rmax(loss_b, 0.3f);
		u = wph(td + 23.5f, iper);
		for (int m = 0; m < 8; m++) {
			s.ih[m] = m < 6 ? wlerp(Tr, u) * il : 0.0f;
			u -= inc8;
			if (u < 0.0f) u += 64.0f;
		}
	}
	// cup, lips, tracker
	{
		real pc0 = w.dc[2];
		for (int k = 0; k < NH; k++) pc0 += g[k] * w.re[2][k];
		s.pc = pc0;
	}
	wlip(w, 3, g, per, s.x, s.v);
	wlip(w, 4, g, per, s.y, s.vy);
	s.um = w.um;
	s.xlp = s.x + s.y;
	s.xhp = w.dc[3] + w.dc[4];
	s.xprev = s.xlp - s.xhp;
	s.tlast = -1e4f;
	s.pprev = per;
	const real o1r = w.re[3][0] + w.re[4][0], o1i = w.im[3][0] + w.im[4][0];
	s.xamp = g[0] * fsqrt(o1r * o1r + o1i * o1i);
	s.df = s.dprev = df;
	s.dnl = clampr(df - knl * wlerp(Tf, wph(-df, iper)), 1.0f, F::DMAXC); // the steepened read, where it would be
	s.vmix = 1.0f;
	s.n1 = 0.0f;
}

// a block without this player's samples (warm start): the debug capture stays in time with the output
static inline void warm_skip_debug() {
#ifdef BRASS_DEBUG
	for (int n = 0; n < NS && dbg_pc && dbg_n < dbg_len; n++, dbg_n++) {
		dbg_pc[dbg_n] = 0.0f;
		if (dbg_u) dbg_u[dbg_n] = dbg_x[dbg_n] = 0.0f;
		if (dbg_xo) dbg_xo[dbg_n] = dbg_y[dbg_n] = 0.0f;
		if (dbg_f) dbg_f[dbg_n] = dbg_r[dbg_n] = 0.0f;
	}
#endif
}

template <class F> static inline void process_player(Engine<F>& e, int i) {
	Player<F>& s = e.pl[i];
	const typename F::Params& P = e.ps;
	const typename F::Bore& b = bore_of(e);
	const bool held = s.owner >= 0;
	strisy::VoiceControls cc{s.note, 0.0f, 0.0f, s.bx_last, 0.0f};
	if (held) cc = e.vc[s.owner];
	const real pres = held ? clampr(cc.pres, 0.0f, 1.5f) : 0.0f;
	if (held) {
		s.bx_last = cc.but_x;
		// the breath of the last ~0.15 s (a release ramps the pressure down before the key lets go)
		s.pres_last = rmax(pres, s.pres_last * (1.0f - e.block_dt / 0.15f));
	}

	// ---- block rate: target note, gestures ---------------------------------------------------
	real note = held ? clampr(cc.note, 0.0f, 132.0f) : s.note;
	if (!held && s.gest_len > 0.0f) {
		s.gest_t += e.block_dt;
		const real t = rmin(s.gest_t / s.gest_len, 1.0f);
		note = s.note_prev + s.gest_range * t * fsqrt(t);
	}
	if (held) s.note_prev = note;
	note = fold_note(e, note);
	// vibrato (pedal above its rest position) and motion shake
	const real ped = clampr((e.g.pedal - 0.22f) * (1.0f / 0.78f), 0.0f, 1.0f);
	s.vib_ph += P.vib_hz * e.block_dt * (1.0f + 0.05f * s.n2);
	if (s.vib_ph >= 1.0f) s.vib_ph -= 1.0f;
	const real vib = P.vibrato * ped * psin(s.vib_ph);
	const real target = note + vib;
	s.note = held ? note : s.note;

	// fingering (keeps the current one while it is still reasonable: lip bends within a partial)
	const Fingering fg = choose_fingering(e, target, s, s.active && s.gate > 0.01f && s.seed == 0.0f);
	const bool newfing = fg.combo != s.combo || fg.ext != s.ext || fg.k != s.k;
	const bool newvalves = fg.combo != s.combo || fg.ext != s.ext;
	if (newfing) {
		const real old_nom = s.nominal;
		if (newvalves) {
			s.dprev = s.df;
			s.vmix = 0.0f;
			s.vrate = rmin(e.block_dt / rmax(P.valve_ms * 0.001f, 0.002f), 1.0f);
		}
		s.combo = fg.combo;
		s.k = fg.k;
		s.ext = fg.ext;
		s.trim = cal_at(e, target).trim + e.learn[s.combo][s.k];
		s.good_blocks = 0;
		(void)old_nom;
	}
	s.nominal = fg.nominal;
	// warm start: a tongued note from silence, seeded for the breath the strike announces (the key
	// pressure takes 10-30 ms to arrive); the player holds that breath through the attack
	WarmSeed<F> wseed;
	bool wfill = false;
	real wbright = 0.0f, walpha = 1.0f;
	bool wprefade = false;
	if (s.seed > 0.0f && held && P.warm > 0.05f) {
		const real h = s.wdyn;
		const WarmAtk& W = warm_shape<F>();
		const real dyn_m = clampr(rmax(F::WARM_D0 + F::WARM_D1 * h, pres), 0.05f, 1.3f);  // the breath meant
		real dyn = clampr(dyn_m * clampr(W.ws + W.wsl * dyn_m, 0.2f, 1.0f), 0.05f, 1.3f); // the seed's
#ifdef BRASS_DEBUG
		if (dbg_force_dyn > 0.0f) dyn = dbg_force_dyn;
#endif
		const bool pre = s.wpre != 0;
		s.wpre = 0;
		Fingering wf = fg;
		if (!warm_lookup(e, target, dyn, wf, wseed) && warm_fingering(e, target, wf)) {
			// a bent note between the captured ones: the player takes the nearest note's fingering
			s.combo = wf.combo;
			s.k = wf.k;
			s.ext = wf.ext;
			s.nominal = wf.nominal;
		}
		if (pre && warm_lookup(e, target, dyn, wf, wseed)) {
			wprefade = true; // this block: the old tail fades out; the seed in the next
		} else if (warm_lookup(e, target, dyn, wf, wseed)) {
			if (e.wseeds >= F::WARM_PER_BLOCK) {
				// another player was seeded in this block (CPU): this one starts in the next
				warm_skip_debug();
				return;
			}
			e.wseeds++;
			wfill = true;
			s.warm = 1;
			s.atk = 0.0f;
			s.wdyn = dyn;
			s.wb0 = dyn;
			s.wb1 = rmax(dyn_m, dyn);
			s.trim = wseed.trim;
			s.bfloor = F::WARM_BF_K * dyn;
			s.b1 = 0.0f;
			s.b2 = dyn;   // the breath the seed was captured at; the floor below it
			s.ovt = s.ov; // the overshoot rises in (a step in the breath would click through the seeded bore)
			s.ov = 0.0f;
			s.gate = 1.0f;
			s.rf = s.rf1 = 0.0f;
			s.rf_tau = F::WARM_FADE_SOFT + (F::WARM_FADE_HARD - F::WARM_FADE_SOFT) * clampr(h, 0.0f, 1.0f);
			wbright = clampr(h + 1.5f * (dyn - 0.45f), 0.0f, 1.0f);
			walpha = clampr(P.warm, 0.0f, 1.0f); // below 1: the note grows into its level and brightness (slower)
#ifdef BRASS_DEBUG
			if (dbg_bright >= 0.0f) wbright = dbg_bright;
#endif
		}
	}
	const real off_c = 100.0f * (target - s.nominal); // cents the slide/lips must add
	const real f_t = midi_hz(target);

	// cylinder length: nominal + valves + extension + slide (the slide does the tuning offset)
	const real ext_len = s.ext ? e.leff * (exp2s((real)s.ext * (1.0f / 12)) - 1.0f) * exp2s(0.0f) : 0.0f;
	const real leff_c = e.leff * exp2s(((real)s.ext + e.combo_semis[s.combo]) * (1.0f / 12));
	const Attack& A = attack_shape<F>();
	const real age = (real)(e.blocks - s.t_on) * e.block_dt;
	const real tp_atk = A.tp * s.atk * atk_env(age, 0.0f, A.tpt);
	const real slide = -leff_c * (off_c + s.trim + tp_atk) * (0.693147f / 1200.0f);
	const real d_target = clampr(b.dpure + e.valve_len[s.combo] + ext_len + clampr(slide, 4.0f - b.dpure, 80.0f), 2.0f, F::DMAXC - 2.0f);

	// lips: tension follows the target; tilt changes the embouchure
	real ty1 = s.by1, ty2 = s.by2, yv, t1;
	const real by = held ? clampr(cc.but_y, -1.0f, 1.0f) : 0.0f;
	SVF_TICK(by, ty1, ty2, e.tc, t1, yv);
	s.by1 = ty1;
	s.by2 = ty2;
	(void)t1;
	yv = clampr(yv, -1.0f, 1.0f);
	const real tilt = e.mute_type == MUTE_PLUNGER ? 0.0f : yv;
	const CalPoint cp = cal_at(e, target);
	// lip shake (shaking the instrument): a fast lip trill up to the next partial and back
	const real shk = clampr((e.shake - 0.08f) * 2.0f * P.shake, 0.0f, 1.0f);
	s.sh_ph += 7.0f * e.block_dt;
	if (s.sh_ph >= 1.0f) s.sh_ph -= 1.0f;
	const bool shaking = shk > 0.05f;
	const real shake_c = (shaking && s.sh_ph < 0.5f) ? shk * 1200.0f * log2s((real)(s.k + 1) / (real)s.k) : 0.0f;
	const real lip_atk = A.l0 * s.atk * atk_env(age, A.lh, A.lt);
	const real f_lip = f_t * exp2s((P.lip_tune + cp.lip + s.lipc + s.trim + tp_atk + shake_c + lip_atk) * (1.0f / 1200) - 0.116f); // the lips follow the horn
	const real wl = 2.0f * PI * rmin(f_lip, 6000.0f);
	const real w2_t = wl * wl;
	const real q = clampr(P.lip_q * exp2s(0.3f * tilt * P.tilt_lip), 0.3f, 500.0f);
	const real reg = exp2s((target - 70.0f) * (1.0f / 12));               // register factor (1 at concert Bb4)
	const real mu = clampr(1.5f * P.lip_mass / fsqrt(reg), 0.05f, 20.0f); // kg/m^2 (lighter lips up high)
	real h0 = clampr(P.lip_open * 0.001f * exp2s(-tilt * P.tilt_lip) / fsqrt(fsqrt(reg)) + 0.001f * A.ha * s.atk * atk_env(age, A.hh, A.ht), -0.002f, 0.002f);
	const real lw = clampr(P.lip_width * 0.001f, 0.002f, 0.02f);

	// breath: pressure -> mouth pressure (threshold grows with register)
	real bl;
	{
		real b1 = s.b1, b2 = s.b2, t;
		// during the attack the breath runs ahead of the key: a key takes ~10-30 ms to be pressed
		// fully, the player's air arrives at the tongue release (the pressure velocity tells how hard)
		real pin = pres;
		if (held && !s.warm && age < 0.06f) pin = rmin(pres + A.lead * rmax(cc.vpres, 0.0f), rmax(pres, 1.1f));
		SVF_TICK(pin, b1, b2, e.bc, t, bl);
		s.b1 = b1;
		s.b2 = b2;
		(void)t;
		if (age < 0.06f) bl = rmax(bl, A.bs * s.atk * pin);
		// a tongued note starts on the breath the player built up behind the tongue: the strike tells
		// how hard (the key pressure itself takes ~10 ms to arrive); it hands over to the pressure
		bl = clampr(rmax(bl, s.bfloor), 0.0f, 1.5f);
		if (s.warm && s.wb1 > 0.0f) {
			// a warm start: the breath rises from the seed's to the one meant, held while the key goes down
			const WarmAtk& W = warm_shape<F>();
			const real tr = rmax(W.tr - W.trl * s.wb1, 0.001f);
			s.wb0 += (s.wb1 - s.wb0) * rmin(e.block_dt / tr, 1.0f);
			bl = clampr(rmax(bl, s.wb0), 0.0f, 1.5f);
			if (!(W.hold > 0.5f && cc.vpres > 0.0f && age < 0.08f)) s.wb1 *= 1.0f - rmin(e.block_dt / rmax(W.bft, 0.002f), 1.0f);
			if (s.wb1 < 0.01f) s.wb1 = 0.0f;
		}
		s.bfloor *= 1.0f - e.block_dt / (s.warm ? F::WARM_BF_T : A.bft);
	}
	const real pthr = cp.pthr; // Pa
	const real p_lo = pthr * P.p_soft;
	const real p_hi = rmax(P.p_loud * 1000.0f * exp2s((target - 70.0f) * (1.0f / 24)), p_lo * 1.2f);
	// the player's touch: pres_ff (the key pressure of a firm press) is ff; a normal touch is mf
	const real xb = clampr(bl / clampr(P.pres_ff, 0.2f, 1.5f), 0.0f, 1.3f);
	real pm_t = p_lo + (p_hi - p_lo) * xb * (0.7f + 0.3f * xb);
	if (!held && s.gest_len > 0.0f && s.gest_t < s.gest_len) pm_t = (p_lo + (p_hi - p_lo) * clampr(s.pres_last / clampr(P.pres_ff, 0.2f, 1.5f), 0.0f, 1.3f)) * (1.0f - 0.6f * s.gest_t / s.gest_len);

	// gate: tongue stop, attack, release
	const bool sounding = held || (s.gest_len > 0.0f && s.gest_t < s.gest_len);
	if (s.stop_blocks > 0) {
		s.stop_blocks--;
		s.gate *= 0.6f;
	} else if (sounding) {
		const real a = rmin(e.block_dt / rmax(s.tau_on, 0.0008f), 1.0f);
		s.gate += a * (1.0f - s.gate);
	} else {
		s.gate *= 1.0f - rmin(e.block_dt / rmax(P.release, 0.005f), 1.0f);
	}
	s.ov *= 1.0f - e.block_dt / (s.warm ? 0.04f : rmax(A.ovt, 0.002f));
	s.ovt *= 1.0f - e.block_dt / 0.04f;
	const real pov = A.pa * s.atk * atk_env(age, A.ph, A.pt);
	h0 = clampr(h0 - A.hc * s.atk * pm_t * (s.ov + pov) / (mu * w2_t), -0.002f, 0.002f);
	if (s.ovt > s.ov) s.ov += (s.ovt - s.ov) * rmin(e.block_dt * (1.0f / F::WARM_OV_T), 1.0f);
	s.tn *= 1.0f - e.block_dt / 0.006f;
	// growl: throat flutter, band-passed noise around 45 Hz on the mouth pressure
	const real growl = clampr(P.growl * (-yv - 0.4f) * 1.67f, 0.0f, 1.0f);
	s.gr1 += 0.06f * (noise(s.rng) - s.gr1);
	s.gr2 += 0.06f * (s.gr1 - s.gr2);
	s.n2 += 0.02f * (noise(s.rng) - s.n2); // slow wander
	const real pm_end = rmax(pm_t * s.gate * (1.0f + s.ov + pov) * (1.0f + 0.012f * s.n2 + 3.0f * growl * (s.gr1 - s.gr2)), 0.0f);
	if (s.gate < 1e-6f && !sounding) {
		// silent: the bore rings out, then the player goes idle
	}

	// half valve: far bends (beyond the lips' comfortable range, re the slide the note normally takes)
	// lose resonance strength
	const real hv = clampr((fabsf(off_c + s.trim - cp.trim) - 30.0f) * (1.0f / 60.0f), 0.0f, 1.0f) * P.halfvalve;
	const real loss_b = clampr(1.0f - 0.25f * P.bore_loss - 0.3f * hv - 0.4f * (1.0f - s.vmix) * s.vmix * 4.0f * 0.5f, 0.3f, 1.0f);

	// steepening: time shift per Pa (Burgers), over ~the cylinder and leadpipe length
	const real lnl = (b.dpure + 60.0f) * (C0 / (real)F::FS) + 0.3f;
	const real knl = clampr(P.brassy, 0.0f, 8.0f) * 1.2f * lnl / (RHO * C0 * C0 * C0) * (real)F::FS / fsqrt(reg);

	if (wfill) {
		// this block: the seed instead of the sound (the next block starts in the oscillation)
		warm_fill(e, i, wseed, (real)F::FS / f_t, d_target, loss_b, knl, wbright, walpha);
		s.seed = 0.0f;
		s.pm = pm_end;
		s.w2 = w2_t;
		s.peak = 0.0f;
		s.idle_blocks = 0;
		warm_skip_debug();
		return;
	}
	if (s.wfn > 0 || s.wbn > 0) {
		// a warm start's cylinder lines: seeded ahead of this block's reads (all at once if the bore changes)
		const bool all = newfing || s.vmix < 1.0f;
		const int nf = all ? s.wfn : (int)rmax((real)(int32_t)(s.w + (uint32_t)(NS + 96) - (uint32_t)(int)s.df - s.wfi), 0.0f);
		const int nb = all ? s.wbn : (int)rmax((real)(int32_t)(s.w + (uint32_t)(NS + 4) - (uint32_t)(int)s.df - s.wbi), 0.0f);
		wline_fill<F>(F::cyl_f[i], s.wfi, s.wfn, s.wfu, s.winc, s.wt[0], nf);
		wline_fill<F>(F::cyl_b[i], s.wbi, s.wbn, s.wbu, s.winc, s.wt[1], nb);
	}
	if (s.seed > 0.0f && !wprefade) {
		// the lips start in their swing (phase: opening, through the rest position), with roughly the
		// amplitude they will settle at; the lip motion locks onto the horn within a few periods
#ifdef BRASS_DEBUG
		const real seed_lip = dbg_seed<F>;
#else
		const real seed_lip = F::SEED_LIP;
#endif
		const real amp = A.sd * seed_lip * s.seed * (0.12f + 0.25f * fsqrt(bl)) * 0.001f / fsqrt(fsqrt(reg));
		s.x = h0;
		s.v = amp * (fsqrt(w2_t) + A.sf * (2.0f * PI * f_t - fsqrt(w2_t)));
		s.w2 = w2_t;
		s.seed = 0.0f;
	}
	// ---- audio: 64 internal samples -------------------------------------------------------------
	const int nsec = b.nsec;
	int dl[MAXSEC];
	real kl[MAXSEC];
	for (int j = 0; j < MAXSEC; j++) {
		dl[j] = j < nsec ? b.sec_d[j] : 1;
		kl[j] = j < nsec ? b.sec_k[j] : 0.0f;
	}
	real x = s.x, v = s.v, pc = s.pc, y = s.y, vy = s.vy;
#ifdef BRASS_DEBUG
	const real rb = dbg_rb<F>, w2b_f = exp2s(dbg_cb<F> * (1.0f / 600)), qb = dbg_qb > 0.0f ? dbg_qb : q;
#else
	const real rb = F::LIP_IN_R, w2b_f = exp2s(F::LIP_IN_C * (1.0f / 600)), qb = q;
#endif
	real pm = s.pm, w2 = s.w2;
	if (s.active == 1 && s.w2 == 0.0f) w2 = w2_t;
	const real dpm = (pm_end - pm) * (1.0f / NS), dw2 = (w2_t - w2) * (1.0f / NS);
	const real dt = e.dt;
	const real Cdt = clampr(P.cup, 0.2f, 2.5f) * 1e-6f / (RHO * C0 * C0) / dt;
	const real iz0 = 1.0f / b.z0;
	const real wq = rmax(fsqrt(w2_t), 1.0f) / q;
	const real damp = 1.0f / (1.0f + dt * wq);
	const real kc = 3.0f * w2_t; // contact stiffness when closed
	const real cdamp = 1.0f / (1.0f + dt * (wq + 2.0f * fsqrt(kc)));
	const real wqb = rmax(fsqrt(w2_t * w2b_f), 1.0f) / qb;
	const real dampb = 1.0f / (1.0f + dt * wqb);
	const real cdampb = 1.0f / (1.0f + dt * (wqb + 2.0f * fsqrt(kc * rmax(rb, 0.01f))));
	const real imu = 1.0f / mu;
	const real two_rho = 2.0f / RHO;
	const real spump = F::SPUMP * lw * 0.002f; // swept area of the lip motion (m^2)
	// turbulence of the established jet: it grows with the lip oscillation (none in the first instants)
	const real br = P.breath * (1.0f + 2.0f * hv) * rmin(s.xamp * (1.0f / 2e-4f), 1.0f);
	const real tnl = P.t_noise * s.tn * s.tn * 2e-5f;
	real n1 = s.n1, um = s.um;
	uint32_t rng = s.rng;
	uint32_t w = s.w;
	real df = s.df;
	const real ddf = (d_target - df) * (1.0f / NS);
	const real dprev = s.dprev;
	real vmix = s.vmix;
	const real dvm = (rmin(vmix + s.vrate, 1.0f) - vmix) * (1.0f / NS);
	real dnl = s.dnl;
	float* __restrict__ CF = F::cyl_f[i];
	float* __restrict__ CB = F::cyl_b[i];
	int dpos = s.dpos, hpos = s.hpos, phase = s.phase;
	// tracker
	const real a_lp = clampr(2.0f * PI * f_t * 1.3f * dt, 0.001f, 0.9f);
	real xlp = s.xlp, xhp = s.xhp, xprev = s.xprev;
	real tlast = s.tlast - (real)NS;
	real pprev = s.pprev;
	int ncross = 0;
	real per_sum = 0.0f;
	real peak = 0.0f, xamp = 0.0f;
	// register balance: the bell radiates low notes far less than high ones (~10 dB per octave);
	// a real player and room even that out to a few dB per octave
	const real gout = clampr(exp2s(-(target - 70.0f) * (target < 70.0f ? 1.5f / 12 : 1.0f / 12)), 0.25f, 5.0f);
	real* __restrict__ mix = e.mix;
	// radiated fade-in of a warm start (two one-poles at block rate, linear within the block)
	real gr = gout * s.rf;
	if (wprefade) {
		s.rf = s.rf1 = 0.0f;
	} else if (s.rf < 1.0f) {
		const real a = rmin(e.block_dt / rmax(s.rf_tau, 1e-4f), 1.0f);
		s.rf1 += a * (1.0f - s.rf1);
		s.rf += a * (s.rf1 - s.rf);
		if (s.rf > 0.9999f) s.rf = s.rf1 = 1.0f;
	}
	const real dgr = (gout * s.rf - gr) * (1.0f / NS);
	const real rl = 1.5f, ru = 0.9f;
	for (int n = 0; n < NS; n++) {
		pm += dpm;
		w2 += dw2;
		df += ddf;
		vmix += dvm;
		// cup <- front end
		const real b0 = s.fb[0][(w - (uint32_t)b.sec_d[0]) & SMASK];
		const real dp = pm - pc;
		const real sj = fsqrt(two_rho * fabsf(dp) + 4.0f);
		const real isj = 1.0f / sj;
		const real h = rmax(x + y, 0.0f);
		const real wh = lw * h;
		// flow: Bernoulli jet + turbulence + swept volume; tongue noise
		n1 += 0.35f * (noise(rng) - n1);
		const real uj = wh * two_rho * dp * isj;
#ifdef BRASS_DEBUG
		um += 0.002f * (uj - um);
		real u0 = uj + br * (uj - um) * n1 + spump * v + tnl * n1;
		if (dbg_impulse) { u0 = (dbg_n == 0) ? 1e-6f * (real)F::FS : 0.0f; }
		if (dbg_pc && dbg_n < dbg_len) {
			dbg_pc[dbg_n] = pc;
			if (dbg_u) {
				dbg_u[dbg_n] = u0;
				dbg_x[dbg_n] = x + y;
			}
			if (dbg_xo) {
				dbg_xo[dbg_n] = x;
				dbg_y[dbg_n] = y;
			}
		}
		dbg_n++;
#else
		um += 0.002f * (uj - um); // the jet's mean: turbulence rides on the flow pulses, not the mean
		const real u0 = uj + br * (uj - um) * n1 + spump * v + tnl * n1;
#endif
		const real G = wh * isj * (1.0f / RHO);
		pc = ((Cdt + G) * pc + u0 + 2.0f * b0 * iz0) / (Cdt + G + iz0);
		s.ff[0][w & SMASK] = pc - b0;
		// lips: the outward striking mode x (the pressure difference pushes it open) and the inward
		// striking mode y (pushed closed, rb x as strongly); opening x + y, contact when closed
		const real fl = (pm - pc) * imu;
		const real hc = x + y;
		if (hc >= 0.0f) {
			v = (v + dt * (-w2 * (x - h0) + fl)) * damp;
			vy = (vy + dt * (-w2 * w2b_f * y - rb * fl)) * dampb;
		} else {
			v = (v + dt * (-w2 * (x - h0) - kc * hc + fl)) * cdamp;
			vy = (vy + dt * (-w2 * w2b_f * y - rb * (kc * hc + fl))) * cdampb;
		}
		x += dt * v;
		y += dt * vy;
		// front end ladder (Kelly-Lochbaum, one multiply per junction)
		if (nsec == 7) ladder<7>(s, w, dl, kl);
		else if (nsec == 5) ladder<5>(s, w, dl, kl);
		else ladder<0>(s, w, dl, kl, nsec);
		// junction to the cylinder; the backward wave arrives after df (valve move: two lengths)
		real cbv;
		{
			const real dd = df;
			const int i0 = (int)dd;
			const real fr = dd - (real)i0;
			const real c0 = CB[(w - (uint32_t)i0) & F::CMASK], c1 = CB[(w - (uint32_t)i0 - 1u) & F::CMASK];
			cbv = c0 + fr * (c1 - c0);
			if (vmix < 1.0f) {
				const int j0 = (int)dprev;
				const real fq = dprev - (real)j0;
				const real e0 = CB[(w - (uint32_t)j0) & F::CMASK], e1 = CB[(w - (uint32_t)j0 - 1u) & F::CMASK];
				cbv = (e0 + fq * (e1 - e0)) + vmix * (cbv - (e0 + fq * (e1 - e0)));
			}
		}
		const real fa = s.ff[nsec - 1][(w - (uint32_t)b.sec_d[nsec - 1]) & SMASK];
		const real tj = b.sec_k[nsec - 1] * (fa - cbv);
		CF[w & F::CMASK] = fa + tj;
#ifdef BRASS_DEBUG
		if (dbg_f && dbg_n - 1 < dbg_len) dbg_f[dbg_n - 1] = fa + tj;
#endif
		s.fb[nsec - 1][w & SMASK] = cbv + tj;
		// forward wave at the bell entry: the linear wave closes the loop (the bell reflects mostly
		// low frequencies), the steepened read (crests travel faster) radiates; keeping the loop
		// linear keeps the oscillation clean at fff (no chaos from the shock fronts)
		real yb, yl;
		{
			const int i0 = (int)df;
			const real fr = df - (real)i0;
			const real a0 = CF[(w - (uint32_t)i0) & F::CMASK], a1 = CF[(w - (uint32_t)i0 - 1u) & F::CMASK];
			real ynom = a0 + fr * (a1 - a0);
			if (vmix < 1.0f) {
				const int j0 = (int)dprev;
				const real fq = dprev - (real)j0;
				const real e0 = CF[(w - (uint32_t)j0) & F::CMASK], e1 = CF[(w - (uint32_t)j0 - 1u) & F::CMASK];
				ynom = (e0 + fq * (e1 - e0)) + vmix * (ynom - (e0 + fq * (e1 - e0)));
			}
			real dn = df - knl * ynom;
			dn = clampr(dn, dnl - rl, dnl + ru);
			dnl = clampr(dn, 1.0f, F::DMAXC);
			yl = ynom;
			if (vmix < 1.0f) {
				yb = ynom;
			} else {
				const int k0 = (int)dnl;
				const real fk = dnl - (real)k0;
				const real s0 = CF[(w - (uint32_t)k0) & F::CMASK], s1 = CF[(w - (uint32_t)k0 - 1u) & F::CMASK];
				yb = s0 + fk * (s1 - s0);
			}
		}
		// bell reflection: decimate 8x, FIR at 12 kHz, interpolate 8x
		s.dh[dpos] = yl;
		s.dh[dpos + DHL] = yl;
		dpos = (dpos + 1) & (DHL - 1);
		if (++phase == F::DEC) {
			phase = 0;
			// newest sample at dpos - 1: window dh[dpos + DHL - NDEC .. dpos + DHL - 1]
			const float* __restrict__ hd = &s.dh[dpos + DHL - F::NDEC];
			real a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
			for (int k = 0; k < F::NDEC; k += 4) {
				a0 += F::HDEC[k] * hd[k];
				a1 += F::HDEC[k + 1] * hd[k + 1];
				a2 += F::HDEC[k + 2] * hd[k + 2];
				a3 += F::HDEC[k + 3] * hd[k + 3];
			}
			const real acc = (a0 + a1) + (a2 + a3);
			// 12 kHz FIR
			s.h12[hpos] = acc;
			s.h12[hpos + F::H12L] = acc;
			hpos = (hpos + 1) & (F::H12L - 1);
			const float* __restrict__ hh = &s.h12[hpos + F::H12L - F::NFIR];
			const float* __restrict__ fr = e.fir_rev;
			real r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
			for (int k = 0; k < F::NFIR; k += 4) {
				r0 += fr[k] * hh[k];
				r1 += fr[k + 1] * hh[k + 1];
				r2 += fr[k + 2] * hh[k + 2];
				r3 += fr[k + 3] * hh[k + 3];
			}
			const real r = (r0 + r1) + (r2 + r3);
			for (int k = 5; k > 0; k--) s.ih[k] = s.ih[k - 1];
			s.ih[0] = r;
		}
		real rfl = 0.0f;
		{
			for (int m = 0; m < 6; m++) rfl += F::HINT[8 * m + phase] * s.ih[m];
		}
#ifdef BRASS_DEBUG
		if (dbg_norefl) rfl = 0.0f;
#endif
		CB[w & F::CMASK] = rfl * loss_b;
#ifdef BRASS_DEBUG
		if (dbg_r && dbg_n - 1 < dbg_len) dbg_r[dbg_n - 1] = rfl * loss_b;
#endif
		w++;
		// radiated: the forward wave at the bell entry
		gr += dgr;
		mix[n] += gr * yb;
		peak = rmax(peak, fabsf(yb));
		// pitch tracker: lip displacement, low-passed, DC removed, upward zero crossings
		xlp += a_lp * (x + y - xlp);
		xhp += F::TRK_HP * (xlp - xhp);
		const real xc = xlp - xhp;
		xamp = rmax(xamp, fabsf(xc));
		if (xprev < 0.0f && xc >= 0.0f) {
			const real tc = (real)n - xc / (xc - xprev);
			if (tlast > -4000.0f) {
				const real per = tc - tlast;
				// count only periods consistent with the previous one (a clean, periodic regime)
				if (fabsf(per - pprev) < F::TRK_TOL * per) {
					per_sum += per;
					ncross++;
				}
				pprev = per;
			}
			tlast = tc;
		}
		xprev = xc;
	}
	s.x = x;
	s.v = v;
	s.y = y;
	s.vy = vy;
	s.pc = pc;
	s.pm = pm;
	s.w2 = w2;
	s.n1 = n1;
	s.um = um;
	s.rng = rng;
	s.w = w;
	s.df = df;
	s.vmix = rmin(vmix, 1.0f);
	s.dnl = dnl;
	s.dpos = dpos;
	s.hpos = hpos;
	s.phase = phase;
	s.xlp = xlp;
	s.xhp = xhp;
	s.xprev = xprev;
	s.tlast = tlast;
	s.pprev = pprev;
	s.peak = peak;
	s.level += 0.05f * (peak - s.level);

	// ---- pitch: the lips and slide trim towards the target -------------------------------------
#ifdef BRASS_DEBUG
	if (dbg_nopll) ncross = 0;
#endif
	s.xamp = rmax(xamp, s.xamp + 0.1f * (xamp - s.xamp)); // fast up, slow down
	if (s.xamp < 4e-5f || shaking) ncross = 0;            // no oscillation (yet), or a shake: nothing to measure
	if (ncross > 0) {
		const real fm = (real)F::FS * (real)ncross / rmax(per_sum, 1.0f);
		s.fmeas = fm;
		const real err = 1200.0f * log2s(fm / f_t);
		if (fabsf(err) < 160.0f && s.gate > 0.3f && s.stop_blocks == 0) {
			// settle the attack fast once the note is locked near its pitch; never jump (a big slide
			// move at once would knock the lips out of their regime)
			const bool early = (e.blocks - s.t_on) * e.block_dt < 0.05f;
			const real tc = early && fabsf(err) < 40.0f ? 0.015f : 0.05f;
			const real step = clampr(err * rmin(per_sum * dt / tc, 0.6f), -(early ? A.pr : 4.0f), early ? A.pr : 4.0f);
			s.trim = clampr(s.trim - step, -250.0f, 150.0f);
			s.wrong_blocks = 0;
			if (fabsf(err) < 8.0f) {
				if (++s.good_blocks > 100) e.learn[s.combo][s.k] += 0.02f * (s.trim - cp.trim - e.learn[s.combo][s.k]);
			}
		} else if (s.gate > 0.3f && fabsf(err) > 250.0f) {
			if (++s.wrong_blocks > 30) s.lipc = clampr(s.lipc - (err > 0.0f ? 8.0f : -8.0f), -400.0f, 400.0f);
		}
	}
	if (s.good_blocks > 50) s.lipc *= 0.995f;

	// guard
	const real st = fabsf(s.pc) + (fabsf(s.x) + fabsf(s.y)) * 1e5f + fabsf(s.v) + fabsf(s.vy);
	if (!(peak < 1e6f) || !(st < 1e7f)) {
#ifdef BRASS_DEBUG
		fprintf(stderr, "guard reset: player %d peak %g pc %g x %g v %g%c", i, (double)peak, (double)s.pc, (double)s.x, (double)s.v, 10);
#endif
		reset_player(e, i);
		return;
	}
	// idle once released and silent
	if (!sounding && s.gate < 1e-4f && peak < 1e-3f) {
		if (++s.idle_blocks > 4) {
			s.active = 0;
			s.idle_blocks = 0;
		}
	} else {
		s.idle_blocks = 0;
	}
}

// ---------------------------------------------------------------------------------------------
// keys -> players (slurs, stealing, slur back)

template <class F> static inline int alloc_player(Engine<F>& e) {
	const int np = (int)clampr(e.ps.voices, 1.0f, (real)F::NP);
	int best = -1;
	real bl = 1e30f;
	for (int i = 0; i < np; i++) {
		const Player<F>& s = e.pl[i];
		if (!s.active) return i;
		if (s.owner < 0 && s.level < bl) {
			bl = s.level;
			best = i;
		}
	}
	if (best >= 0) return best;
	// all held: the oldest note
	uint32_t old = 0xFFFFFFFFu;
	for (int i = 0; i < np; i++)
		if (e.pl[i].t_on < old) {
			old = e.pl[i].t_on;
			best = i;
		}
	return best < 0 ? 0 : best;
}

template <class F> static inline void detach_key(Engine<F>& e, int key) {
	for (int i = 0; i < F::NP; i++) {
		Player<F>& s = e.pl[i];
		int m = 0;
		for (int j = 0; j < s.nstack; j++)
			if (s.stack[j] != key) s.stack[m++] = s.stack[j];
		s.nstack = m;
	}
}

template <class F> static inline void key_on(Engine<F>& e, int key, real strike) {
	KeyState& k = e.ks[key];
	const typename F::Params& P = e.ps;
	const int mode = (int)(P.legato + 0.5f);
	int slur_from = -1;
	if (mode >= 1) {
		// the most recent sounding player whose key is held
		uint32_t newest = 0;
		for (int i = 0; i < F::NP; i++) {
			const Player<F>& s = e.pl[i];
			if (s.active && s.owner >= 0 && s.owner != key && s.t_on >= newest) {
				const bool old_enough = e.blocks - s.t_on > (uint32_t)(0.06f / e.block_dt);
				if (mode == 2 || (old_enough && strike < P.slur_thr)) {
					newest = s.t_on;
					slur_from = i;
				}
			}
		}
	}
	if (slur_from >= 0) {
		Player<F>& s = e.pl[slur_from];
		if (s.nstack < NSTACK) s.stack[s.nstack++] = s.owner;
		else {
			for (int j = 1; j < NSTACK; j++) s.stack[j - 1] = s.stack[j];
			s.stack[NSTACK - 1] = s.owner;
		}
		e.ks[s.owner].player = -1;
		s.owner = key;
		k.player = slur_from;
		start_note(e, slur_from, strike, false, false);
		return;
	}
	int i = -1;
	if (mode == 2) {
		// mono: the one player (the most recent)
		uint32_t newest = 0;
		for (int j = 0; j < F::NP; j++)
			if (e.pl[j].active && e.pl[j].t_on >= newest) {
				newest = e.pl[j].t_on;
				i = j;
			}
	}
	if (i < 0) i = alloc_player(e);
	Player<F>& s = e.pl[i];
	if (s.owner >= 0) e.ks[s.owner].player = -1;
	const bool from_silence = !s.active || s.gate < 0.02f;
	// a released player still ringing quietly: a new tongued note there starts warm too, once the
	// tail has faded out (one block)
	const bool quiet_tail = !from_silence && s.owner < 0 && s.gate < F::WARM_TAIL_GATE && e.ps.warm > 0.05f && warm_has(e, fold_note(e, e.vc[key].note));
	s.owner = key;
	s.nstack = 0;
	k.player = i;
	if (from_silence) {
		const int keep_rng = (int)s.rng;
		(void)keep_rng;
		s.gate = 0.0f;
		s.lipc = 0.0f;
		s.b1 = 0.0f;
		s.b2 = e.vc[key].pres; // start the breath smoother at the current pressure
		s.by1 = 0.0f;
		s.by2 = clampr(e.vc[key].but_y, -1.0f, 1.0f);
		s.x = 0.0f;
		s.v = 0.0f;
		s.y = 0.0f;
		s.vy = 0.0f;
		s.rf = s.rf1 = 1.0f;
	}
	s.wpre = quiet_tail ? 1 : 0;
	start_note(e, i, rmax(strike, 0.15f), true, from_silence || quiet_tail);
}

template <class F> static inline void key_off(Engine<F>& e, int key) {
	KeyState& k = e.ks[key];
	detach_key(e, key);
	const int i = k.player;
	k.player = -1;
	if (i < 0) return;
	Player<F>& s = e.pl[i];
	if (s.owner != key) return;
	// slur back to a key still held under this note
	while (s.nstack > 0) {
		const int back = s.stack[--s.nstack];
		if (back >= 0 && back < e.nvoices && e.ks[back].held) {
			s.owner = back;
			e.ks[back].player = i;
			start_note(e, i, 0.0f, false, false);
			return;
		}
	}
	s.owner = -1;
	// release gesture: fall (tilt left) or doit (tilt right)
	const real bx = s.bx_last;
	if (fabsf(bx) > 0.35f && s.pres_last > 0.08f) {
		s.gest_t = 0.0f;
		s.gest_len = bx < 0.0f ? 0.32f : 0.22f;
		s.gest_range = bx < 0.0f ? -e.ps.fall * fabsf(bx) : e.ps.doit * fabsf(bx);
	} else {
		s.gest_len = 0.0f;
	}
}

template <class F> static inline void process_keys(Engine<F>& e) {
	for (int v = 0; v < e.nvoices; v++) {
		KeyState& k = e.ks[v];
		const strisy::VoiceControls& c = e.vc[v];
		const real pres = c.pres;
		const real vp = pres > 0.0f ? c.vpres : rmin(c.vpres, 0.0f); // stale vpres after release
		if (pres > 0.0f) {
			const bool jump = k.held && fabsf(c.note - k.note_on) >= 1.0f && k.player >= 0 && fabsf(c.note - e.pl[k.player].note_prev) >= 1.0f;
			if (!k.held || jump) {
				if (jump) key_off(e, v);
				k.held = 1;
				k.pending = 2;
				k.vpk = rmax(vp, 0.0f);
				k.note_on = c.note;
			} else if (k.pending > 0) {
				k.vpk = rmax(k.vpk, vp);
			}
			if (k.pending > 0 && --k.pending == 0) key_on(e, v, k.vpk);
			k.note_on = k.pending > 0 ? c.note : k.note_on;
			if (k.player >= 0) k.note_on = c.note;
		} else if (k.held) {
			k.held = 0;
			k.pending = 0;
			key_off(e, v);
		}
		k.pres_prev = pres;
	}
}

// ---------------------------------------------------------------------------------------------
// shared radiation, mutes, room

// an SVF (strisy) at the 48 kHz output rate: lowpass, bandpass (peak gain Q), highpass
struct Svf2 {
	strisy::SvfCoef c;
	real k;
};
template <class F> [[nodiscard]] static inline Svf2 svf_at(const Engine<F>& e, real hz, real q) {
	const real qq = clampr(q, 0.3f, 20.0f);
	return {strisy::svf_pade(PI / e.sr * clampr(hz, 20.0f, 0.45f * e.sr), qq), 1.0f / qq};
}

// soft clipper: x - 4/27 x^3 on |x| <= 1.5, +-1 beyond
[[nodiscard]] static inline real sat(real x) {
	x = clampr(x, -1.5f, 1.5f);
	return x * (1.0f - 0.148148148f * x * x);
}

template <class F> static inline void process_output(Engine<F>& e, real* __restrict__ out) {
	const typename F::Bore& b = bore_of(e);
	const typename F::Params& P = e.ps;
	// 96 -> 48 kHz: 31 tap halfband (kaiser, -0.1 dB at 18 kHz)
	static constexpr real HB[8] = {-0.00012586f, 0.00106450f, -0.00377247f, 0.00980408f, -0.02159190f, 0.04398898f, -0.09309054f, 0.31373747f};
	// bell brightness: first order high shelf around 1.2 kHz, +-(bell * 2) dB
	const real gsh = exp2s(clampr(P.bell, -6.0f, 6.0f) * 2.0f * (1.0f / 6.02f)) - 1.0f;
	const real ash = clampr(2.0f * PI * 1200.0f / e.sr, 0.01f, 0.9f);
	const Svf2 fit_hp = svf_at(e, F::FIT_HP_HZ, 0.7f), fit_pk = svf_at(e, F::FIT_PK_HZ, F::FIT_PK_Q);
	const real fit_lo = F::FIT_LO, fit_pg = F::FIT_PK_G;
	const real adir = clampr(2.0f * PI * 2400.0f * (0.0615f / b.bell_r) / e.sr, 0.01f, 0.9f);
	// mutes (the radiated sound only: the mute's effect on the bore is the bore_loss / bell params)
	const real md = clampr(P.mute, 0.0f, 1.0f);
	const int mt = md > 0.0f ? e.mute_type : MUTE_NONE;
	// the plunger / hand: the tilt of the loudest held player
	{
		real best = -1.0f, y = 0.0f;
		for (int i = 0; i < F::NP; i++) {
			const Player<F>& s = e.pl[i];
			if (s.active && s.owner >= 0 && s.level > best) {
				best = s.level;
				y = clampr(e.vc[s.owner].but_y, -1.0f, 1.0f);
			}
		}
		e.plunge += 0.05f * (y - e.plunge);
	}
	const real wy = clampr(P.wah * e.plunge, -1.0f, 1.0f);
	Svf2 m0{}, m1{}, m2{};
	real g_bp = 0.0f, g_bp2 = 0.0f, mgain = 1.0f;
	switch (mt) {
	case MUTE_HARMON:                                     // stem out: no lows, a strong nasal peak, buzzy highs; hand on the stem = wah
		m0 = svf_at(e, 950.0f, 0.75f);                    // high-pass (twice)
		m1 = svf_at(e, 1750.0f * exp2s(0.8f * wy), 2.6f); // the "wa" peak
		m2 = svf_at(e, 4300.0f, 3.0f);                    // upper buzz
		g_bp = 3.2f * exp2s(0.5f * wy);
		g_bp2 = 1.2f;
		mgain = 2.6f;
		break;
	case MUTE_CUP: // dark, soft-edged: lows and highs cut, a hollow peak around 700 Hz
		m0 = svf_at(e, 280.0f, 0.7f);
		m1 = svf_at(e, 720.0f, 1.6f);
		m2 = svf_at(e, 1500.0f, 0.75f); // low-pass (twice)
		g_bp = 1.4f;
		mgain = 2.2f;
		break;
	case MUTE_STRAIGHT: // bright, thin, piercing
		m0 = svf_at(e, 1150.0f, 0.85f);
		m1 = svf_at(e, 2500.0f, 2.0f);
		m2 = svf_at(e, 9000.0f, 0.7f);
		g_bp = 1.6f;
		mgain = 2.4f;
		break;
	case MUTE_PLUNGER: { // back = closed: low-pass with a resonance, quieter
		const real open = 0.5f * (1.0f + wy);
		m1 = svf_at(e, 550.0f * exp2s(3.8f * open), 1.2f + 2.5f * open * (1.0f - open) * 4.0f * 0.5f);
		mgain = 0.55f + 0.45f * open;
		break;
	}
	default:
		break;
	}
	// early reflections (floor, side walls): the bell's direct sound plus a little room colour
	static constexpr int RT[4] = {139, 254, 389, 610}; // 2.9, 5.3, 8.1, 12.7 ms
	static constexpr real RG[4] = {0.36f, 0.28f, 0.22f, 0.16f};
	const real room = clampr(P.room, 0.0f, 1.0f);
	const real arl = 2.0f * PI * 3500.0f / e.sr;
	const real g = 0.0107f * clampr(P.gain, 0.0f, 8.0f) * F::PRESET_GAIN[e.preset];
	for (int n = 0; n < BLOCK; n++) {
		e.hb[e.hbpos] = e.mix[2 * n];
		e.hb[e.hbpos + 32] = e.mix[2 * n];
		e.hbpos = (e.hbpos + 1) & 31;
		e.hb[e.hbpos] = e.mix[2 * n + 1];
		e.hb[e.hbpos + 32] = e.mix[2 * n + 1];
		e.hbpos = (e.hbpos + 1) & 31;
		const real* h = &e.hb[e.hbpos + 32 - 31]; // oldest .. newest (31 samples)
		real acc = 0.5f * h[15];
		for (int k = 0; k < 8; k++) acc += HB[k] * (h[2 * k] + h[30 - 2 * k]);
		// radiation biquads (DF2T)
		real y = acc * b.rad_g;
		for (int st = 0; st < 3; st++) {
			const float* c = b.rad[st];
			const real o = y + e.rad_s[st][0];
			e.rad_s[st][0] = c[0] * y - c[2] * o + e.rad_s[st][1];
			e.rad_s[st][1] = c[1] * y - c[3] * o;
			y = o;
		}
		// on-axis bell directivity (Levine & Schwinger: the bell beams its highs forward, ~+3 dB at
		// ka = 1 rising to ~+10 dB at ka = 3) and the brightness shelf
		e.bell_s[1] += adir * (y - e.bell_s[1]);
		y += 3.0f * (y - e.bell_s[1]);
		e.bell_s[0] += ash * (y - e.bell_s[0]);
		y += gsh * (y - e.bell_s[0]);
		// fitted to the recordings (cmpref.py): the radiated fundamental of low notes is weaker and the
		// 1-1.5 kHz formant stronger than the idealised bell gives: a gentle low cut and a formant
		{
			real v1, v2;
			SVF_TICK(y, e.fit_s[0][0], e.fit_s[0][1], fit_hp.c, v1, v2);
			y = y - fit_hp.k * v1 - v2 + fit_lo * v2;
			SVF_TICK(y, e.fit_s[1][0], e.fit_s[1][1], fit_pk.c, v1, v2);
			y += fit_pg * fit_pk.k * v1;
		}
		// mute
		if (mt != MUTE_NONE) {
			real wet = y, v1, v2;
			if (mt == MUTE_HARMON || mt == MUTE_STRAIGHT || mt == MUTE_CUP) {
				SVF_TICK(wet, e.mute_s[0][0], e.mute_s[0][1], m0.c, v1, v2);
				real hp = wet - m0.k * v1 - v2;
				if (mt != MUTE_STRAIGHT) {
					SVF_TICK(hp, e.mute_s[3][0], e.mute_s[3][1], m0.c, v1, v2);
					hp = hp - m0.k * v1 - v2;
				}
				SVF_TICK(hp, e.mute_s[1][0], e.mute_s[1][1], m1.c, v1, v2);
				wet = hp + g_bp * m1.k * v1;
				if (mt == MUTE_CUP) {
					SVF_TICK(wet, e.mute_s[2][0], e.mute_s[2][1], m2.c, v1, v2);
					SVF_TICK(v2, e.mute_s[3][0], e.mute_s[3][1], m2.c, v1, wet);
				} else {
					SVF_TICK(wet, e.mute_s[2][0], e.mute_s[2][1], m2.c, v1, v2);
					wet = (mt == MUTE_HARMON) ? wet + g_bp2 * m2.k * v1 : v2;
				}
			} else { // plunger
				SVF_TICK(wet, e.mute_s[1][0], e.mute_s[1][1], m1.c, v1, v2);
				wet = v2;
			}
			y += md * (mgain * wet - y);
		}
		// room
		F::room_buf[e.room_w & (ROOML - 1)] = y;
		real r = 0.0f;
		for (int k = 0; k < 4; k++) r += RG[k] * F::room_buf[(e.room_w - (uint32_t)RT[k]) & (ROOML - 1)];
		e.room_w++;
		e.room_lp += arl * (r - e.room_lp);
		y = (y + room * e.room_lp) * g;
		// DC blocker and a soft ceiling at 4.5
		e.dc += 0.0016f * (y - e.dc);
		out[n] = 4.5f * sat((y - e.dc) * (1.0f / 4.5f));
	}
	// flush the shared filters to exact zero after a while of silence (no denormals, exact silence)
	bool loud = false;
	for (int n = 0; n < NS; n++) loud = loud || fabsf(e.mix[n]) > 1e-6f;
	e.quiet = loud ? 0.0f : e.quiet + 1.0f;
	if (e.quiet > 1500.0f) {
		for (auto& x : e.hb) x = 0.0f;
		for (auto& rs : e.rad_s) rs[0] = rs[1] = 0.0f;
		for (auto& ms : e.mute_s) ms[0] = ms[1] = 0.0f;
		for (auto& fs : e.fit_s) fs[0] = fs[1] = 0.0f;
		e.bell_s[0] = e.bell_s[1] = 0.0f;
		e.room_lp = e.dc = 0.0f;
		for (int n = 0; n < BLOCK; n++) out[n] = 0.0f;
		e.quiet = 1500.0f;
	}
}

template <class F> static inline void process(Engine<F>& e, real* __restrict__ output) {
	{
		const real* __restrict__ pt = reinterpret_cast<const real*>(&e.p);
		real* __restrict__ ps = reinterpret_cast<real*>(&e.ps);
		for (int i = 0; i < F::NPARAMS; i++) {
			const real d = pt[i] - ps[i];
			ps[i] = fabsf(d) < 1e-5f ? pt[i] : ps[i] + 0.05f * d;
		}
	}
	e.blocks++;
	const real sh = clampr(e.g.acc_abs - 1.1f, 0.0f, 1.0f);
	e.shake_pk = rmax(sh, e.shake_pk * (1.0f - e.block_dt * 2.5f));
	e.shake += 0.03f * (e.shake_pk - e.shake);
	e.wseeds = 0;
	process_keys(e);
	for (int n = 0; n < NS; n++) e.mix[n] = 0.0f;
	for (int i = 0; i < F::NP; i++)
		if (e.pl[i].active) process_player(e, i);
	process_output(e, output);
}

} // namespace brasspm

#endif // BRASS_CORE_H_
