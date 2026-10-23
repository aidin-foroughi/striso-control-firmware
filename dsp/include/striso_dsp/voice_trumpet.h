/**
 * trumpet: a physical model of the trumpet family (Bb, C, piccolo, cornet; mutes), built for
 * realism first. Build with `VOICE=trumpet`; presets with set_preset().
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
 *                 speaks in its first periods and the bore pulls it onto the resonance (a one-mass
 *                 outward valve is passive until the bore's reflections return: low notes grew over
 *                 150 ms). Contact stiffness when closed; swept volume flow
 *                 | Bernoulli jet u = w h sqrt(2 (p_m - p_c) / rho), regularised at small dp
 *                 CUP compliance V / (rho c^2) (implicit in p_c with the jet's conductance: stable)
 *                   |
 *                 FRONT END: throat, backbore, leadpipe as a Kelly-Lochbaum ladder of short
 *                 cylinders (integer delays at 96 kHz): with the cup it makes the mouthpiece
 *                 "popping" resonance, the tall impedance peaks around 0.7-1.1 kHz
 *                   |
 *                 CYLINDER (leadpipe end .. bell throat): forward and backward delay lines.
 *                 Valves add tubing (0, 2, 1, 12, 23, 13, 123, plus 4th/5th-valve-like loops for the
 *                 lowest notes); while a piston moves both lengths sound, crossfaded (a half-open
 *                 valve); the tuning slide trims the length continuously.
 *                   |
 *                 BELL: the reflection function of the flaring bell (+ the losses of the whole bore)
 *                 as an exact multirate convolution: decimate 8x -> 128 tap FIR at 12 kHz ->
 *                 interpolate 8x. Fitted (trumpet_bore.h, tools/trumpet) so the model's input
 *                 impedance matches a transmission-line model of real bore profiles: resonances
 *                 within a few cents of the TMM, including the flat first resonance (pedal tones are
 *                 privileged frequencies) and the slightly out of tune upper partials.
 *                   |
 *                 RADIATION (shared, after the mix): the wave arriving at the bell, steepened
 *                 (amplitude dependent propagation speed, Burgers: c + (gamma+1)/2 u; Hirschberg et
 *                 al. 1996, Msallam et al. 2000; soft is round, ff grows near-shock fronts = the
 *                 brassy "cuivre"; the loop itself stays linear, so fff can't turn chaotic), then the
 *                 bell transmission (high-pass to ~1.5 kHz), 96 -> 48 kHz, on-axis bell directivity,
 *                 a gentle EQ fitted to recordings (Iowa MIS Bb trumpet), mutes, a little room.
 *
 * The player (per note): picks valves + partial from the horn's own resonance chart, sets the lips
 * (trumpet_cal.h: measured per preset and note), and trims slide + lips with a slow pitch loop
 * (zero crossings of the lip motion) so the note lands within a few cents of the Striso's pitch,
 * also for microtonal notes and bends; what it learns per fingering is kept while powered.
 *
 * Playing:
 *   pressure    breath (mouth pressure, pp..fff): loudness and brassiness together
 *   strike      tonguing: hard = "t" (fast, overshoot, tongue noise), soft = "d"
 *   new key while one is held (soft press): slurred legato, no tongue, the lips change partial /
 *               valves move; release it and the held key comes back (lip trills by tapping)
 *               (legato param: 0 poly, 1 auto, 2 mono)
 *   tilt fwd/back  embouchure: forward tighter / brighter, back looser / darker (+ growl if set);
 *               harmon: hand over the stem (wah); plunger: tilt = the plunger (back = closed)
 *   tilt sideways  pitch bend (from the firmware); far bends get the half-valve sound; released
 *               while tilted: fall (left) or doit (right)
 *   pedal       vibrato depth (above the default position)
 *   shake       lip shake (trill to the next partial)
 *
 * Polyphony: 4 players (trumpets); a fifth key steals the quietest released player, else the
 * oldest. Cost (QEMU Cortex-M7 instruction counts, tools/target_render): ~35k instructions per
 * sounding player per block, ~140k for 4 at ff with valves moving (~170k cycles at 1.2 cycles per
 * instruction). Memory: cylinder lines 32 KB + room 4 KB in ram1, Engine ~17 KB in DTCM.
 */
#ifndef VOICE_TRUMPET_H_
#define VOICE_TRUMPET_H_

#include <math.h>
#include <stdint.h>
#ifdef TRUMPET_DEBUG
#include <stdio.h>
#endif

#include "strisy.h"
#include "trumpet_bore.h"
#include "trumpet_cal.h"
#include "trumpet_seed.h"

namespace trumpet {

using real = float;
using strisy::rmax;
using strisy::rmin;

static constexpr int BLOCK = strisy::BLOCK;
static constexpr int MAX_VOICES = 10; // key voices (controls)
static constexpr int NP = 4;          // players: physical trumpets (CPU: ~33k instructions per block each)
static constexpr int OS = 2;          // internal oversampling
static constexpr int NS = BLOCK * OS; // internal samples per block
static constexpr int CLEN = 1024;     // cylinder delay lines (one way, 96 kHz)
static constexpr uint32_t CMASK = CLEN - 1;
static constexpr real DMAXC = (real)(CLEN - 4);
static constexpr int SLEN = 32; // front-end section rings
static constexpr uint32_t SMASK = SLEN - 1;
static constexpr int MAXSEC = 8;
static constexpr int DHL = 64;     // decimator history (double buffered)
static constexpr int H12L = 128;   // 12 kHz history (double buffered)
static constexpr int NSTACK = 4;   // held keys under a slurred note
static constexpr int ROOML = 1024; // early reflections (48 kHz)
static constexpr real RHO = 1.2f;
static constexpr real C0 = 343.0f;
static constexpr real PI = 3.14159265f;
#ifdef TRUMPET_SPUMP
static constexpr real SPUMP = TRUMPET_SPUMP;
#else
static constexpr real SPUMP = 0.6f;
#endif
// tongued attacks: the breath leads the key during the first 60 ms (LEAD x pressure velocity: a
// key takes 10-30 ms to go down, the air arrives at the tongue release), a breath floor from the
// strike (SEED_BREATH x strike, decaying over BFLOOR_T) and the lips released already swinging
// (SEED_LIP x their settling amplitude)
// the lips' second (inward striking) mode: its response to the pressure difference re the outward
// mode's, and its tuning above the outward mode (cents); see the LIPS comment in process_player
static constexpr real LIP_IN_R = 1.0f, LIP_IN_C = 150.0f;
static constexpr real LEAD = 0.7f, SEED_BREATH = 0.3f, BFLOOR_T = 0.035f, SEED_LIP = 1.0f;
// warm start (trumpet_seed.h): the breath a tongued note is seeded for (key pressure units:
// WARM_D0 + WARM_D1 x strike, or the pressure if more), the hold of that breath (s), the front-end
// run-in (samples at 96 kHz), the radiated fade-in time constant (s, per stage: hard .. soft)
#ifndef TRUMPET_WARM_D0
static constexpr real WARM_D0 = 0.2f, WARM_D1 = 0.5f, WARM_BF_K = 0.7f, WARM_BF_T = 0.03f;
#else
static constexpr real WARM_D0 = TRUMPET_WARM_D0, WARM_D1 = TRUMPET_WARM_D1, WARM_BF_K = TRUMPET_WARM_BFK, WARM_BF_T = TRUMPET_WARM_BFT;
#endif
static constexpr real WARM_FADE_HARD = 0.001f, WARM_FADE_SOFT = 0.004f;
#ifndef TRUMPET_WARM_NPRE
static constexpr int WARM_NPRE = 192;
#else
static constexpr int WARM_NPRE = TRUMPET_WARM_NPRE;
#endif
// pitch tracker: DC removal of the lip motion (per internal sample), period consistency
static constexpr real TRK_HP = 0.004f, TRK_TOL = 0.05f;
// output EQ fitted to the Iowa MIS Bb trumpet recordings (cmpref.py)
#ifndef TRUMPET_FIT
static constexpr real FIT_HP_HZ = 450.0f, FIT_LO = 0.12f, FIT_PK_HZ = 1250.0f, FIT_PK_Q = 0.8f, FIT_PK_G = 1.6f;
#else
static constexpr real FIT_HP_HZ = TRUMPET_FIT_HP, FIT_LO = TRUMPET_FIT_LO, FIT_PK_HZ = TRUMPET_FIT_PK, FIT_PK_Q = TRUMPET_FIT_Q, FIT_PK_G = TRUMPET_FIT_G;
#endif
static_assert(bore::NDEC == 48 && bore::DEC == 8 && bore::NFIR % 4 == 0, "multirate bell layout");
static_assert(bore::MAXSEC >= MAXSEC, "section table");

// cylinder delay lines: AHB SRAM1/2 (ram1), 48 KB; early reflections 4 KB
static float cyl_f[NP][CLEN] __attribute__((aligned(32), section(".ram1_clear")));
static float cyl_b[NP][CLEN] __attribute__((aligned(32), section(".ram1_clear")));
static float room_buf[ROOML] __attribute__((aligned(32), section(".ram1_clear")));

// ---------------------------------------------------------------------------------------------
// Parameters (all floats: the live parameters)

struct Params {
	// level / breath
	real gain;    // output level
	real p_soft;  // mouth pressure at the lightest touch, re the oscillation threshold
	real p_loud;  // mouth pressure at full pressure (kPa at concert Bb4, scales with register)
	real pres_ff; // key pressure that plays ff (the player's firm press)
	real resp_hz; // breath smoothing
	real release; // breath stop time constant (s)
	// lips
	real lip_tune;  // lip tension trim (cents): pitch pull / partial choice
	real lip_q;     // lip resonance Q
	real lip_mass;  // effective lip mass scale
	real lip_open;  // rest opening (mm)
	real lip_width; // lip width (mm)
	real tilt_lip;  // tilt fwd/back -> embouchure (octaves of opening)
	// bore
	real cup;       // cup volume (cm^3)
	real bore_loss; // extra bore damping 0..1
	real bell;      // bell brightness (dB per octave above 1 kHz)
	real brassy;    // wave steepening (1 = physical)
	// air
	real breath;  // breath turbulence
	real t_noise; // tongue noise burst
	// attack
	real attack;    // tonguing hardness scale
	real overshoot; // attack pressure overshoot
	// modulation
	real vibrato; // pedal vibrato depth (semitones at full pedal)
	real vib_hz;
	real shake; // motion -> lip shake
	real growl; // tilt back -> growl
	// bends / release
	real fall;      // fall range (semitones)
	real doit;      // doit range (semitones)
	real halfvalve; // half-valve character on far bends
	// mute
	real mute; // mute depth (the preset's mute)
	real wah;  // tilt -> mute / plunger
	// playing
	real legato;   // 0 poly, 1 auto, 2 mono
	real slur_thr; // strike below which a new key slurs (auto)
	real valve_ms; // valve travel time
	real voices;   // players
	real room;     // early reflections
};
static constexpr int NPARAMS = sizeof(Params) / sizeof(real);
static_assert(sizeof(Params) == NPARAMS * sizeof(real), "Params must hold only floats");

enum Mute : int { MUTE_NONE = 0,
	MUTE_HARMON,
	MUTE_CUP,
	MUTE_STRAIGHT,
	MUTE_PLUNGER };

struct Preset {
	const char* name;
	int bore;
	int mute;
	Params p;
};

[[nodiscard]] static inline Params base_params() {
	Params p{};
	p.gain = 1.0f;
	p.p_soft = 1.8f;
	p.p_loud = 9.0f;
	p.pres_ff = 0.72f;
	p.resp_hz = 60.0f;
	p.release = 0.035f;
	p.lip_tune = 0.0f;
	p.lip_q = 12.0f;
	p.lip_mass = 3.3f;
	p.lip_open = 0.15f;
	p.lip_width = 8.0f;
	p.tilt_lip = 0.5f;
	p.cup = 0.8f;
	p.bore_loss = 0.0f;
	p.bell = 0.0f;
	p.brassy = 2.5f;
	p.breath = 0.02f;
	p.t_noise = 0.12f;
	p.attack = 1.0f;
	p.overshoot = 0.25f;
	p.vibrato = 0.4f;
	p.vib_hz = 5.6f;
	p.shake = 1.0f;
	p.growl = 0.0f;
	p.fall = 7.0f;
	p.doit = 4.0f;
	p.halfvalve = 1.0f;
	p.mute = 0.0f;
	p.wah = 0.0f;
	p.legato = 1.0f;
	p.slur_thr = 0.3f;
	p.valve_ms = 14.0f;
	p.voices = (real)NP;
	p.room = 0.5f;
	return p;
}

static constexpr int NUM_PRESETS = 9;

[[nodiscard]] static inline Preset preset_def(int i) {
	Params p = base_params();
	switch (i) {
	default:
	case 0: // Bb trumpet, classical: warm, deeper cup, controlled brassiness
		p.cup = 0.85f;
		p.bell = 0.0f;
		p.brassy = 3.0f;
		return {"bb classical", 0, MUTE_NONE, p};
	case 1: // Bb trumpet, jazz / lead: shallow cup, bright, blares at ff, growl on tilt back
		p.cup = 0.55f;
		p.bell = 1.5f;
		p.brassy = 4.5f;
		p.lip_open = 0.12f;
		p.breath = 0.03f;
		p.growl = 0.6f;
		p.overshoot = 0.35f;
		p.fall = 9.0f;
		return {"bb jazz", 0, MUTE_NONE, p};
	case 2: // C trumpet: a little brighter and more focused
		p.cup = 0.75f;
		p.bell = 0.5f;
		p.brassy = 3.3f;
		return {"c trumpet", 1, MUTE_NONE, p};
	case 3: // piccolo trumpet in Bb: light, bright, small cup
		p.cup = 0.45f;
		p.bell = 0.5f;
		p.brassy = 2.4f;
		p.lip_mass = 0.7f;
		p.lip_q = 6.0f; // light lips: more damping, or they flutter on their own at ff
		p.vibrato = 0.25f;
		p.p_loud = 6.0f;
		return {"piccolo", 2, MUTE_NONE, p};
	case 4: // cornet: deep V cup, conical: mellow, sweet
		p.cup = 1.05f;
		p.bell = -1.5f;
		p.brassy = 2.0f;
		p.lip_q = 8.0f;
		p.breath = 0.025f;
		return {"cornet", 3, MUTE_NONE, p};
	case 5: // harmon mute, stem out (Miles): thin, buzzy, nasal; tilt = hand over the stem
		p.cup = 0.6f;
		p.bell = 1.0f;
		p.brassy = 3.5f;
		p.mute = 1.0f;
		p.wah = 0.8f;
		p.tilt_lip = 0.15f;
		p.breath = 0.035f;
		p.vibrato = 0.25f;
		return {"harmon", 0, MUTE_HARMON, p};
	case 6: // cup mute: dark, soft-edged, distant
		p.cup = 0.75f;
		p.brassy = 3.0f;
		p.mute = 1.0f;
		p.wah = 0.0f;
		return {"cup mute", 0, MUTE_CUP, p};
	case 7: // straight mute: bright, thin, piercing
		p.cup = 0.7f;
		p.bell = 0.5f;
		p.brassy = 3.3f;
		p.mute = 1.0f;
		p.wah = 0.0f;
		return {"straight", 0, MUTE_STRAIGHT, p};
	case 8: // plunger: tilt back closes the plunger (wah), growl
		p.cup = 0.6f;
		p.bell = 1.0f;
		p.brassy = 4.0f;
		p.mute = 1.0f;
		p.wah = 1.0f;
		p.tilt_lip = 0.0f;
		p.growl = 0.5f;
		return {"plunger", 0, MUTE_PLUNGER, p};
	}
}

[[nodiscard]] static inline const char* preset_name(int i) {
	static constexpr const char* names[NUM_PRESETS] = {"bb classical", "bb jazz", "c trumpet", "piccolo", "cornet",
	    "harmon", "cup mute", "straight", "plunger"};
	return (i >= 0 && i < NUM_PRESETS) ? names[i] : names[0];
}

#ifndef TRUMPET_PRESET
static constexpr int DEFAULT_PRESET = 0;
#else
static constexpr int DEFAULT_PRESET = TRUMPET_PRESET;
#endif

// level matching per preset (median active RMS, measured with the default performance)
static constexpr real PRESET_GAIN[NUM_PRESETS] = {0.81f, 0.685f, 0.776f, 1.06f, 1.31f, 0.0978f, 0.489f, 0.21f, 0.487f};

// ---------------------------------------------------------------------------------------------
// player and key state

struct Player {
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
	float h12[2 * H12L];
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
	real tn;           // tongue noise envelope
	int stop_blocks;   // re-tongue: blocks with the tongue against the lips
	real seed;         // tongued start from silence: seed strength, applied in the next block
	real bfloor;       // breath at a tongued start (from the strike), decaying into the key pressure
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
	real xamp;  // lip oscillation amplitude (m)
	// warm start (trumpet_seed.h): a tongued note from silence starts in its own steady oscillation
	int warm;     // this note was started from a seed
	real wdyn;    // the strike, then the breath the seed was chosen for (key pressure units)
	real rf, rf1; // radiated fade-in (two one-poles at block rate; 1 = full)
	real rf_tau;  // its time constant (s)
	real level; // smoothed output level (stealing)
};

struct KeyState {
	real pres_prev;
	real note_on; // note at the onset
	real vpk;     // peak strike while pending
	int pending;  // blocks waiting for the strike peak
	int held;
	int player; // player sounding this key, -1 none
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	int bore_i;
	int mute_type;
	Params p;  // as set
	Params ps; // smoothed (de-zippered live edits)
	real sr, dt, block_dt;
	uint32_t blocks;
	strisy::SvfCoef bc, tc; // breath and tilt smoothers (block rate)
	real shake_pk, shake;
	real combo_semis[bore::NCOMBO];
	real valve_len[bore::NCOMBO]; // one-way samples
	real open_midi;
	real leff; // one-way effective length of the open horn (samples) at the nominal pitch
	real learn[bore::NCOMBO][bore::NK];
	real fpitch[bore::NCOMBO][bore::NK]; // fingering chart (MIDI), -1000: not playable
	float fir_rev[bore::NFIR];           // bell reflection, time reversed (oldest sample first)
	real fcost[bore::NCOMBO][bore::NK];
	Player pl[NP];
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
	real quiet; // blocks without sound (state flush)
	float wtab[seed::NSIG][68]; // warm start: one period of each seeded signal
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

[[nodiscard]] static inline const bore::Bore& bore_of(const Engine& e) { return bore::BORES[e.bore_i]; }

#ifdef TRUMPET_DEBUG
static int dbg_nocal = 0;                                                    // 1: no calibration, 2: the values below
static float dbg_cal_lip = 0.0f, dbg_cal_trim = 0.0f, dbg_cal_pthr = 600.0f; // (calibration runs)
#endif

// the player's knowledge of the horn (trumpet_cal.h): lip setting, slide trim and the pressure where
// the note speaks, interpolated between the calibrated notes
struct CalPoint {
	real trim, lip, pthr;
};
[[nodiscard]] static inline CalPoint cal_at(const Engine& e, real note) {
	const cal::Cal& c = cal::CAL[e.preset];
	const real x = clampr(note - c.lo, 0.0f, (real)(cal::NCAL - 1) - 0.001f);
	const int i = (int)x;
	const real f = x - (real)i;
	CalPoint p{c.trim[i] + f * (c.trim[i + 1] - c.trim[i]), c.lip[i] + f * (c.lip[i + 1] - c.lip[i]),
	    c.pthr[i] + f * (c.pthr[i + 1] - c.pthr[i])};
#ifdef TRUMPET_DEBUG
	if (dbg_nocal == 1) p = {0.0f, 0.0f, 1200.0f * exp2s((note - 70.0f) * (1.0f / 18))};
	if (dbg_nocal == 2) p = {dbg_cal_trim, dbg_cal_lip, dbg_cal_pthr};
#endif
	return p;
}

// ---------------------------------------------------------------------------------------------
// setup

static inline void reset_player(Engine& e, int i) {
	Player& s = e.pl[i];
	const uint32_t rng = s.rng ? s.rng : 0x9E3779B9u * (uint32_t)(i + 1) + 12345u;
	const real vph = s.vib_ph;
	s = Player{};
	s.rng = rng;
	s.vib_ph = vph;
	s.owner = -1;
	s.k = 4;
	s.note = s.note_prev = 70.0f;
	s.df = s.dprev = s.dnl = bore_of(e).dpure;
	s.vmix = 1.0f;
	s.fmeas = 0.0f;
	s.rf = s.rf1 = 1.0f;
	for (int n = 0; n < CLEN; n++) cyl_f[i][n] = cyl_b[i][n] = 0.0f;
}

static inline void update_bore(Engine& e) {
	const bore::Bore& b = bore_of(e);
	static constexpr int combo_valves[bore::NCOMBO][3] = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}};
	static constexpr real nominal_semis[bore::NCOMBO] = {0, 1, 2, 3, 4, 5, 6};
	for (int c = 0; c < bore::NCOMBO; c++) {
		e.valve_len[c] = 0.0f;
		for (int v = 0; v < 3; v++)
			if (combo_valves[c][v]) e.valve_len[c] += b.valve_d[v];
		e.combo_semis[c] = nominal_semis[c];
	}
	e.open_midi = 69.0f + 12.0f * log2s(b.f_nom / 440.0f);
	for (int k = 0; k < bore::NFIR; k++) e.fir_rev[k] = b.fir[bore::NFIR - 1 - k];
	// the player's fingering chart: pitch of every resonance with every valve combination, and the
	// preference cost (more valves, the out-of-tune 7th/11th partials, the crowded top)
	static constexpr real pen_combo[bore::NCOMBO] = {0, 3, 4, 9, 22, 30, 40};
	for (int c = 0; c < bore::NCOMBO; c++) {
		for (int k = 0; k < bore::NK; k++) {
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
	e.leff = (real)bore::FS / (2.0f * b.f_nom);
	for (int c = 0; c < bore::NCOMBO; c++)
		for (int k = 0; k < bore::NK; k++) e.learn[c][k] = 0.0f; // learned on top of the calibration
}

static inline void update_derived(Engine& e) {
	e.bc = strisy::svf_from_g(strisy::tan0(PI / e.sr * BLOCK * rmin(e.p.resp_hz, 300.0f)), 0.5f);
}

static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	const Preset pr = preset_def(i);
	const int old_bore = e.bore_i;
	e.preset = i;
	e.p = pr.p;
	e.ps = e.p;
	e.bore_i = pr.bore;
	e.mute_type = pr.mute;
	if (pr.bore != old_bore || e.blocks == 0) {
		update_bore(e);
		// sounding players move to the new bore's lengths through a valve move
		for (int v = 0; v < NP; v++) {
			Player& s = e.pl[v];
			s.dprev = rmin(s.df, DMAXC);
			s.vmix = 0.0f;
			s.vrate = 1.0f / 16.0f;
		}
	}
	update_derived(e);
}

static inline void init(Engine& e, int sample_rate, int nvoices) {
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
	for (int n = 0; n < ROOML; n++) room_buf[n] = 0.0f;
	set_preset(e, DEFAULT_PRESET);
	for (int i = 0; i < NP; i++) {
		e.pl[i].rng = 0;
		e.pl[i].vib_ph = 0.23f * (real)i;
		reset_player(e, i);
	}
	e.blocks = 1;
}

[[nodiscard]] static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < NP; v++) n += e.pl[v].active;
	return n;
}

// ---------------------------------------------------------------------------------------------
// the player's choices: valves, partial, extension

struct Fingering {
	int combo, k, ext;
	real nominal;
};

static inline Fingering choose_fingering(const Engine& e, real note, const Player& s, bool keep) {
	// lowest normal note: partial 2 with 1-2-3; below it the horn grows a 4th (and 5th) valve loop
	const real lowest = e.open_midi + 12.0f - 6.0f - 0.6f;
	int ext = 0;
	if (note < lowest) ext = note < lowest - 6.0f ? 12 : 6;
	const real nn = note + (real)ext; // in the open horn's terms
	Fingering best = {0, 2, ext, e.open_midi + 12.0f - (real)ext};
	real best_cost = 1e9f;
	for (int c = 0; c < bore::NCOMBO; c++) {
		for (int k = 2; k < bore::NK; k++) {
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
[[nodiscard]] static inline real fold_note(const Engine& e, real n) {
	// the range where the notes speak (the piccolo's is narrower; the cornet's top a little lower)
	const bool picc = e.bore_i == 2;
	const real lo = e.open_midi + (picc ? -0.5f : -3.0f);
	const real hi = e.open_midi + (picc ? 38.5f : e.bore_i == 3 ? 40.5f :
	                                      e.bore_i == 1         ? 43.5f :
	                                                              44.5f);
	while (n < lo) n += 12.0f;
	while (n > hi) n -= 12.0f;
	return n;
}

// ---------------------------------------------------------------------------------------------
// one player, one block

static inline void start_note(Engine& e, int i, real hard, bool tongue, bool from_silence) {
	Player& s = e.pl[i];
	const Params& P = e.ps;
	s.warm = 0;
	if (tongue) {
		const real h = clampr(hard * P.attack, 0.0f, 1.5f);
		s.wdyn = h; // the strike, until the warm start picks the breath
		s.tau_on = 0.012f * exp2s(-3.3f * h); // 12 ms "d" .. 1.2 ms "t"
		s.ov = P.overshoot * h;
		s.bfloor = from_silence || s.gate < 0.3f ? clampr(SEED_BREATH * h, 0.0f, 0.9f) : 0.0f;
		s.tn = h;
		if (!from_silence && s.gate > 0.05f) s.stop_blocks = 6 + (int)(10.0f * (1.0f - rmin(h, 1.0f))); // re-tongue
		if (from_silence) {
			// the tongue releases air already under pressure: the full breath hits the lips at once,
			// which the tongue held tensed and ready to swing; the seed (applied in the first block,
			// when the fingering is known) starts the lips swinging at their frequency
			s.gate = clampr(0.7f + 0.25f * h, 0.0f, 0.97f);
			s.seed = clampr(0.4f + 0.6f * h, 0.0f, 1.0f);
		}
	}
	s.t_on = e.blocks;
	s.active = 1;
	s.good_blocks = s.wrong_blocks = 0;
	s.gest_len = 0.0f;
}

// the front-end junctions for one sample (N sections; N = 0: any number, n)
template <int N> static inline void ladder(Player& s, uint32_t w, const int* __restrict__ d, const real* __restrict__ k, int n = N) {
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

#ifdef TRUMPET_DEBUG
static float dbg_rb = LIP_IN_R, dbg_cb = LIP_IN_C, dbg_qb = 0.0f; // inward lip mode: ratio, cents, Q (0: lip_q)
static int dbg_norefl = 0;
static float dbg_seed = SEED_LIP;
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

struct WarmSeed {
	real trim, um;
	real dc[seed::NSIG];
	real re[seed::NSIG][seed::NH], im[seed::NSIG][seed::NH];
};

#ifdef TRUMPET_DEBUG
static const seed::State* dbg_seed_ovr = nullptr; // capture: seed from this state
static int dbg_nowarm = 0;
static float dbg_force_dyn = -1.0f, dbg_bright = -1.0f; // capture: the seed's breath and harmonics
#endif

// the seed for this note and breath (interpolated between the captured breaths); false: none
static inline bool warm_lookup(const Engine& e, real note, real dyn, const Fingering& fg, WarmSeed& w) {
	const seed::State* a = nullptr;
	const seed::State* c = nullptr;
	real u = 0.0f;
#ifdef TRUMPET_DEBUG
	if (dbg_nowarm) return false;
	if (dbg_seed_ovr) a = c = dbg_seed_ovr;
#endif
	if (a == nullptr) {
		const seed::Set& st = seed::SETS[e.preset];
		const int n = (int)(note + 0.5f) - st.lo;
		if (st.st == nullptr || n < 0 || n >= st.n) return false;
		const seed::State* row = &st.st[n * seed::NDYN];
		int j = 0;
		while (j < seed::NDYN - 2 && dyn > seed::DYN[j + 1]) j++;
		u = clampr((dyn - seed::DYN[j]) / (seed::DYN[j + 1] - seed::DYN[j]), 0.0f, 1.0f);
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
	for (int q = 0; q < seed::NSIG; q++) {
		w.dc[q] = a->dc[q] + u * (c->dc[q] - a->dc[q]);
		for (int k = 0; k < seed::NH; k++) {
			w.re[q][k] = a->re[q][k] + u * (c->re[q][k] - a->re[q][k]);
			w.im[q][k] = a->im[q][k] + u * (c->im[q][k] - a->im[q][k]);
		}
	}
	return true;
}

// one period table (64 points, linear interpolation), read at t samples from now
[[nodiscard]] static inline real wtab_at(const float* T, real t, real iper) {
	real u = t * iper;
	u -= (real)(int)u;
	if (u < 0.0f) u += 1.0f;
	u *= 64.0f;
	const int j = (int)u & 63;
	return T[j] + (u - (real)j) * (T[j + 1] - T[j]);
}

// fills one cylinder line backwards from the write index: L[w - n] = T(-n), n = 1..len
static inline void wtab_fill(float* __restrict__ L, uint32_t w, int len, const float* __restrict__ T, real per) {
	const real inc = 64.0f / per;
	real u = 0.0f;
	for (int n = 1; n <= len; n++) {
		u -= inc;
		if (u < 0.0f) u += 64.0f;
		const int j = (int)u & 63;
		L[(w - (uint32_t)n) & CMASK] = T[j] + (u - (real)j) * (T[j + 1] - T[j]);
	}
}

// writes the seed into player i: the cylinder lines, the front end (run in on the seeded cup pressure
// and the returning wave), the bell's multirate filter histories, the cup, both lip modes and the
// tracker. Signals (trumpet_seed.h): 0 the cylinder's forward input, 1 the bell's reflection, 2 the
// cup pressure, 3 and 4 the outward and inward lip modes.
// bright 0..1: the seeded harmonics (0: the first two, 1: all NH)
static inline void warm_fill(Engine& e, int i, const WarmSeed& w, real per, real df, real loss_b, real knl, real bright) {
	Player& s = e.pl[i];
	const bore::Bore& b = bore_of(e);
	constexpr int NH = seed::NH;
	real g[NH];
	for (int k = 0; k < NH; k++) g[k] = clampr(bright * (real)(NH - 2) + 2.0f - (real)k, 0.0f, 1.0f);
	for (int q = 0; q < seed::NSIG; q++) {
		float* T = e.wtab[q];
		for (int j = 0; j < 64; j++) {
			real v = w.dc[q];
			for (int k = 0; k < NH; k++) {
				const int ci = ((k + 1) * j) & 63;
				v += g[k] * (w.re[q][k] * seed::COS64[ci] - w.im[q][k] * seed::COS64[(ci + 48) & 63]);
			}
			T[j] = v;
		}
		T[64] = T[0];
	}
	const float* Tf = e.wtab[0];
	const float* Tr = e.wtab[1];
	const float* Tp = e.wtab[2];
	const float* Tx = e.wtab[3];
	const float* Ty = e.wtab[4];
	const real iper = 1.0f / per;
	const uint32_t w0 = s.w;
	constexpr int NPRE = WARM_NPRE;
	// cylinder: forward line as far back as the (steepened) reads reach, backward line for the reads
	// and the front-end run-in
	wtab_fill(cyl_f[i], w0, (int)rmin(df + 96.0f, (real)(CLEN - 2)), Tf, per);
	wtab_fill(cyl_b[i], w0, (int)rmin(df + (real)(NPRE + 4), (real)(CLEN - 2)), Tr, per);
	// front end: run the ladder in for NPRE samples on the seeded cup pressure and the returning wave
	const int nsec = b.nsec;
	int dl[MAXSEC];
	real kl[MAXSEC];
	for (int j = 0; j < MAXSEC; j++) {
		dl[j] = j < nsec ? b.sec_d[j] : 1;
		kl[j] = j < nsec ? b.sec_k[j] : 0.0f;
		for (int n = 0; n < SLEN; n++) s.ff[j][n] = s.fb[j][n] = 0.0f;
	}
	const float* CB = cyl_b[i];
	const int i0 = (int)df;
	const real fr = df - (real)i0;
	for (int t = -NPRE; t <= -1; t++) {
		const uint32_t ww = w0 + (uint32_t)t;
		const real b0 = s.fb[0][(ww - (uint32_t)b.sec_d[0]) & SMASK];
		s.ff[0][ww & SMASK] = wtab_at(Tp, (real)(t + 1), iper) - b0;
		ladder<0>(s, ww, dl, kl, nsec);
		const real c0 = CB[(ww - (uint32_t)i0) & CMASK], c1 = CB[(ww - (uint32_t)i0 - 1u) & CMASK];
		const real cbv = c0 + fr * (c1 - c0);
		const real fa = s.ff[nsec - 1][(ww - (uint32_t)b.sec_d[nsec - 1]) & SMASK];
		s.fb[nsec - 1][ww & SMASK] = cbv + b.sec_k[nsec - 1] * (fa - cbv);
	}
	// bell: decimator input (the wave arriving at the bell), 12 kHz history (the decimator delays
	// 23.5 samples), interpolator input (the reflection 23.5 samples ahead of its output)
	for (int m = 0; m < DHL; m++) {
		const int j = (s.dpos - 1 - m) & (DHL - 1);
		s.dh[j] = s.dh[j + DHL] = wtab_at(Tf, (real)(-1 - m) - df, iper);
	}
	const real td = (real)(-1 - s.phase);
	for (int m = 0; m < H12L; m++) {
		const int j = (s.hpos - 1 - m) & (H12L - 1);
		s.h12[j] = s.h12[j + H12L] = wtab_at(Tf, td - 8.0f * (real)m - 23.5f - df, iper);
	}
	const real il = 1.0f / rmax(loss_b, 0.3f);
	for (int m = 0; m < 8; m++) s.ih[m] = m < 6 ? wtab_at(Tr, td - 8.0f * (real)m + 23.5f, iper) * il : 0.0f;
	// cup, lips (x and v from the last two samples), tracker
	s.pc = Tp[0];
	s.x = Tx[0];
	s.v = (Tx[0] - wtab_at(Tx, -1.0f, iper)) * (real)bore::FS;
	s.y = Ty[0];
	s.vy = (Ty[0] - wtab_at(Ty, -1.0f, iper)) * (real)bore::FS;
	s.um = w.um;
	s.xlp = s.x + s.y;
	s.xhp = w.dc[3] + w.dc[4];
	s.xprev = s.xlp - s.xhp;
	s.tlast = -1e4f;
	s.pprev = per;
	const real o1r = w.re[3][0] + w.re[4][0], o1i = w.im[3][0] + w.im[4][0];
	s.xamp = g[0] * fsqrt(o1r * o1r + o1i * o1i);
	s.df = s.dprev = df;
	s.dnl = clampr(df - knl * wtab_at(Tf, -df, iper), 1.0f, DMAXC); // the steepened read, where it would be
	s.vmix = 1.0f;
	s.n1 = 0.0f;
}

static inline void process_player(Engine& e, int i) {
	Player& s = e.pl[i];
	const Params& P = e.ps;
	const bore::Bore& b = bore_of(e);
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
	WarmSeed wseed;
	bool wfill = false;
	real wbright = 0.0f;
	if (s.seed > 0.0f && held) {
		const real h = s.wdyn;
		real dyn = clampr(rmax(WARM_D0 + WARM_D1 * h, pres), 0.05f, 1.3f);
#ifdef TRUMPET_DEBUG
		if (dbg_force_dyn > 0.0f) dyn = dbg_force_dyn;
#endif
		if (warm_lookup(e, target, dyn, fg, wseed)) {
			wfill = true;
			s.warm = 1;
			s.wdyn = dyn;
			s.trim = wseed.trim;
			s.bfloor = WARM_BF_K * dyn;
			s.b1 = 0.0f;
			s.b2 = WARM_BF_K * dyn;
			s.gate = 1.0f;
			s.rf = s.rf1 = 0.0f;
			s.rf_tau = WARM_FADE_SOFT + (WARM_FADE_HARD - WARM_FADE_SOFT) * clampr(h, 0.0f, 1.0f);
			wbright = clampr(h + 1.5f * (dyn - 0.45f), 0.0f, 1.0f);
#ifdef TRUMPET_DEBUG
			if (dbg_bright >= 0.0f) wbright = dbg_bright;
#endif
		}
	}
	const real off_c = 100.0f * (target - s.nominal); // cents the slide/lips must add
	const real f_t = midi_hz(target);

	// cylinder length: nominal + valves + extension + slide (the slide does the tuning offset)
	const real ext_len = s.ext ? e.leff * (exp2s((real)s.ext * (1.0f / 12)) - 1.0f) * exp2s(0.0f) : 0.0f;
	const real leff_c = e.leff * exp2s(((real)s.ext + e.combo_semis[s.combo]) * (1.0f / 12));
	const real slide = -leff_c * (off_c + s.trim) * (0.693147f / 1200.0f);
	const real d_target = clampr(b.dpure + e.valve_len[s.combo] + ext_len + clampr(slide, 4.0f - b.dpure, 80.0f), 2.0f, DMAXC - 2.0f);

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
	const real f_lip = f_t * exp2s((P.lip_tune + cp.lip + s.lipc + s.trim + shake_c) * (1.0f / 1200) - 0.116f); // the lips follow the horn
	const real wl = 2.0f * PI * rmin(f_lip, 6000.0f);
	const real w2_t = wl * wl;
	const real q = clampr(P.lip_q * exp2s(0.3f * tilt * P.tilt_lip), 0.3f, 500.0f);
	const real reg = exp2s((target - 70.0f) * (1.0f / 12));               // register factor (1 at concert Bb4)
	const real mu = clampr(1.5f * P.lip_mass / fsqrt(reg), 0.05f, 20.0f); // kg/m^2 (lighter lips up high)
	const real h0 = clampr(P.lip_open * 0.001f * exp2s(-tilt * P.tilt_lip) / fsqrt(fsqrt(reg)), -0.001f, 0.002f);
	const real lw = clampr(P.lip_width * 0.001f, 0.002f, 0.02f);

	// breath: pressure -> mouth pressure (threshold grows with register)
	real bl;
	{
		real b1 = s.b1, b2 = s.b2, t;
		// during the attack the breath runs ahead of the key: a key takes ~10-30 ms to be pressed
		// fully, the player's air arrives at the tongue release (the pressure velocity tells how hard)
		real pin = pres;
		if (held && (real)(e.blocks - s.t_on) * e.block_dt < 0.06f) pin = rmin(pres + LEAD * rmax(cc.vpres, 0.0f), rmax(pres, 1.1f));
		SVF_TICK(pin, b1, b2, e.bc, t, bl);
		s.b1 = b1;
		s.b2 = b2;
		(void)t;
		// a tongued note starts on the breath the player built up behind the tongue: the strike tells
		// how hard (the key pressure itself takes ~10 ms to arrive); it hands over to the pressure
		bl = clampr(rmax(bl, s.bfloor), 0.0f, 1.5f);
		s.bfloor *= 1.0f - e.block_dt / (s.warm ? WARM_BF_T : BFLOOR_T);
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
	s.ov *= 1.0f - e.block_dt / 0.04f;
	s.tn *= 1.0f - e.block_dt / 0.006f;
	// growl: throat flutter, band-passed noise around 45 Hz on the mouth pressure
	const real growl = clampr(P.growl * (-yv - 0.4f) * 1.67f, 0.0f, 1.0f);
	s.gr1 += 0.06f * (noise(s.rng) - s.gr1);
	s.gr2 += 0.06f * (s.gr1 - s.gr2);
	s.n2 += 0.02f * (noise(s.rng) - s.n2); // slow wander
	const real pm_end = rmax(pm_t * s.gate * (1.0f + s.ov) * (1.0f + 0.012f * s.n2 + 3.0f * growl * (s.gr1 - s.gr2)), 0.0f);
	if (s.gate < 1e-6f && !sounding) {
		// silent: the bore rings out, then the player goes idle
	}

	// half valve: far bends (beyond the lips' comfortable range, re the slide the note normally takes)
	// lose resonance strength
	const real hv = clampr((fabsf(off_c + s.trim - cp.trim) - 30.0f) * (1.0f / 60.0f), 0.0f, 1.0f) * P.halfvalve;
	const real loss_b = clampr(1.0f - 0.25f * P.bore_loss - 0.3f * hv - 0.4f * (1.0f - s.vmix) * s.vmix * 4.0f * 0.5f, 0.3f, 1.0f);

	// steepening: time shift per Pa (Burgers), over ~the cylinder and leadpipe length
	const real lnl = (b.dpure + 60.0f) * (C0 / (real)bore::FS) + 0.3f;
	const real knl = clampr(P.brassy, 0.0f, 8.0f) * 1.2f * lnl / (RHO * C0 * C0 * C0) * (real)bore::FS / fsqrt(reg);

	if (wfill) {
		// this block: the seed instead of the sound (the next block starts in the oscillation)
		warm_fill(e, i, wseed, (real)bore::FS / f_t, d_target, loss_b, knl, wbright);
		s.seed = 0.0f;
		s.pm = pm_end;
		s.w2 = w2_t;
		s.peak = 0.0f;
		s.idle_blocks = 0;
#ifdef TRUMPET_DEBUG
		for (int n = 0; n < NS && dbg_pc && dbg_n < dbg_len; n++, dbg_n++) {
			dbg_pc[dbg_n] = 0.0f;
			if (dbg_u) dbg_u[dbg_n] = dbg_x[dbg_n] = 0.0f;
		}
#endif
		return;
	}
	if (s.seed > 0.0f) {
		// the lips start in their swing (phase: opening, through the rest position), with roughly the
		// amplitude they will settle at; the lip motion locks onto the horn within a few periods
#ifdef TRUMPET_DEBUG
		const real SEED_LIP = dbg_seed;
#endif
		const real amp = SEED_LIP * s.seed * (0.12f + 0.25f * fsqrt(bl)) * 0.001f / fsqrt(fsqrt(reg));
		s.x = h0;
		s.v = amp * fsqrt(w2_t);
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
#ifdef TRUMPET_DEBUG
	const real rb = dbg_rb, w2b_f = exp2s(dbg_cb * (1.0f / 600)), qb = dbg_qb > 0.0f ? dbg_qb : q;
#else
	const real rb = LIP_IN_R, w2b_f = exp2s(LIP_IN_C * (1.0f / 600)), qb = q;
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
	const real spump = SPUMP * lw * 0.002f; // swept area of the lip motion (m^2)
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
	float* __restrict__ CF = cyl_f[i];
	float* __restrict__ CB = cyl_b[i];
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
	if (s.rf < 1.0f) {
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
#ifdef TRUMPET_DEBUG
		um += 0.002f * (uj - um);
		real u0 = uj + br * (uj - um) * n1 + spump * v + tnl * n1;
		if (dbg_impulse) { u0 = (dbg_n == 0) ? 1e-6f * (real)bore::FS : 0.0f; }
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
		const real F = (pm - pc) * imu;
		const real hc = x + y;
		if (hc >= 0.0f) {
			v = (v + dt * (-w2 * (x - h0) + F)) * damp;
			vy = (vy + dt * (-w2 * w2b_f * y - rb * F)) * dampb;
		} else {
			v = (v + dt * (-w2 * (x - h0) - kc * hc + F)) * cdamp;
			vy = (vy + dt * (-w2 * w2b_f * y - rb * (kc * hc + F))) * cdampb;
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
			const real c0 = CB[(w - (uint32_t)i0) & CMASK], c1 = CB[(w - (uint32_t)i0 - 1u) & CMASK];
			cbv = c0 + fr * (c1 - c0);
			if (vmix < 1.0f) {
				const int j0 = (int)dprev;
				const real fq = dprev - (real)j0;
				const real e0 = CB[(w - (uint32_t)j0) & CMASK], e1 = CB[(w - (uint32_t)j0 - 1u) & CMASK];
				cbv = (e0 + fq * (e1 - e0)) + vmix * (cbv - (e0 + fq * (e1 - e0)));
			}
		}
		const real fa = s.ff[nsec - 1][(w - (uint32_t)b.sec_d[nsec - 1]) & SMASK];
		const real tj = b.sec_k[nsec - 1] * (fa - cbv);
		CF[w & CMASK] = fa + tj;
#ifdef TRUMPET_DEBUG
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
			const real a0 = CF[(w - (uint32_t)i0) & CMASK], a1 = CF[(w - (uint32_t)i0 - 1u) & CMASK];
			real ynom = a0 + fr * (a1 - a0);
			if (vmix < 1.0f) {
				const int j0 = (int)dprev;
				const real fq = dprev - (real)j0;
				const real e0 = CF[(w - (uint32_t)j0) & CMASK], e1 = CF[(w - (uint32_t)j0 - 1u) & CMASK];
				ynom = (e0 + fq * (e1 - e0)) + vmix * (ynom - (e0 + fq * (e1 - e0)));
			}
			real dn = df - knl * ynom;
			dn = clampr(dn, dnl - rl, dnl + ru);
			dnl = clampr(dn, 1.0f, DMAXC);
			yl = ynom;
			if (vmix < 1.0f) {
				yb = ynom;
			} else {
				const int k0 = (int)dnl;
				const real fk = dnl - (real)k0;
				const real s0 = CF[(w - (uint32_t)k0) & CMASK], s1 = CF[(w - (uint32_t)k0 - 1u) & CMASK];
				yb = s0 + fk * (s1 - s0);
			}
		}
		// bell reflection: decimate 8x, FIR at 12 kHz, interpolate 8x
		s.dh[dpos] = yl;
		s.dh[dpos + DHL] = yl;
		dpos = (dpos + 1) & (DHL - 1);
		if (++phase == bore::DEC) {
			phase = 0;
			// newest sample at dpos - 1: window dh[dpos + DHL - NDEC .. dpos + DHL - 1]
			const float* __restrict__ hd = &s.dh[dpos + DHL - bore::NDEC];
			real a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
			for (int k = 0; k < bore::NDEC; k += 4) {
				a0 += bore::HDEC[k] * hd[k];
				a1 += bore::HDEC[k + 1] * hd[k + 1];
				a2 += bore::HDEC[k + 2] * hd[k + 2];
				a3 += bore::HDEC[k + 3] * hd[k + 3];
			}
			const real acc = (a0 + a1) + (a2 + a3);
			// 12 kHz FIR
			s.h12[hpos] = acc;
			s.h12[hpos + H12L] = acc;
			hpos = (hpos + 1) & (H12L - 1);
			const float* __restrict__ hh = &s.h12[hpos + H12L - bore::NFIR];
			const float* __restrict__ fr = e.fir_rev;
			real r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;
			for (int k = 0; k < bore::NFIR; k += 4) {
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
			for (int m = 0; m < 6; m++) rfl += bore::HINT[8 * m + phase] * s.ih[m];
		}
#ifdef TRUMPET_DEBUG
		if (dbg_norefl) rfl = 0.0f;
#endif
		CB[w & CMASK] = rfl * loss_b;
#ifdef TRUMPET_DEBUG
		if (dbg_r && dbg_n - 1 < dbg_len) dbg_r[dbg_n - 1] = rfl * loss_b;
#endif
		w++;
		// radiated: the forward wave at the bell entry
		gr += dgr;
		mix[n] += gr * yb;
		peak = rmax(peak, fabsf(yb));
		// pitch tracker: lip displacement, low-passed, DC removed, upward zero crossings
		xlp += a_lp * (x + y - xlp);
		xhp += TRK_HP * (xlp - xhp);
		const real xc = xlp - xhp;
		xamp = rmax(xamp, fabsf(xc));
		if (xprev < 0.0f && xc >= 0.0f) {
			const real tc = (real)n - xc / (xc - xprev);
			if (tlast > -4000.0f) {
				const real per = tc - tlast;
				// count only periods consistent with the previous one (a clean, periodic regime)
				if (fabsf(per - pprev) < TRK_TOL * per) {
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
#ifdef TRUMPET_DEBUG
	if (dbg_nopll) ncross = 0;
#endif
	s.xamp = rmax(xamp, s.xamp + 0.1f * (xamp - s.xamp)); // fast up, slow down
	if (s.xamp < 4e-5f || shaking) ncross = 0;            // no oscillation (yet), or a shake: nothing to measure
	if (ncross > 0) {
		const real fm = (real)bore::FS * (real)ncross / rmax(per_sum, 1.0f);
		s.fmeas = fm;
		const real err = 1200.0f * log2s(fm / f_t);
		if (fabsf(err) < 160.0f && s.gate > 0.3f && s.stop_blocks == 0) {
			// settle the attack fast once the note is locked near its pitch; never jump (a big slide
			// move at once would knock the lips out of their regime)
			const bool early = (e.blocks - s.t_on) * e.block_dt < 0.05f;
			const real tc = early && fabsf(err) < 40.0f ? 0.015f : 0.05f;
			const real step = clampr(err * rmin(per_sum * dt / tc, 0.6f), -4.0f, 4.0f);
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
#ifdef TRUMPET_DEBUG
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

static inline int alloc_player(Engine& e) {
	const int np = (int)clampr(e.ps.voices, 1.0f, (real)NP);
	int best = -1;
	real bl = 1e30f;
	for (int i = 0; i < np; i++) {
		const Player& s = e.pl[i];
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

static inline void detach_key(Engine& e, int key) {
	for (int i = 0; i < NP; i++) {
		Player& s = e.pl[i];
		int m = 0;
		for (int j = 0; j < s.nstack; j++)
			if (s.stack[j] != key) s.stack[m++] = s.stack[j];
		s.nstack = m;
	}
}

static inline void key_on(Engine& e, int key, real strike) {
	KeyState& k = e.ks[key];
	const Params& P = e.ps;
	const int mode = (int)(P.legato + 0.5f);
	int slur_from = -1;
	if (mode >= 1) {
		// the most recent sounding player whose key is held
		uint32_t newest = 0;
		for (int i = 0; i < NP; i++) {
			const Player& s = e.pl[i];
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
		Player& s = e.pl[slur_from];
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
		for (int j = 0; j < NP; j++)
			if (e.pl[j].active && e.pl[j].t_on >= newest) {
				newest = e.pl[j].t_on;
				i = j;
			}
	}
	if (i < 0) i = alloc_player(e);
	Player& s = e.pl[i];
	if (s.owner >= 0) e.ks[s.owner].player = -1;
	const bool from_silence = !s.active || s.gate < 0.02f;
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
	start_note(e, i, rmax(strike, 0.15f), true, from_silence);
}

static inline void key_off(Engine& e, int key) {
	KeyState& k = e.ks[key];
	detach_key(e, key);
	const int i = k.player;
	k.player = -1;
	if (i < 0) return;
	Player& s = e.pl[i];
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

static inline void process_keys(Engine& e) {
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
[[nodiscard]] static inline Svf2 svf_at(const Engine& e, real hz, real q) {
	const real qq = clampr(q, 0.3f, 20.0f);
	return {strisy::svf_pade(PI / e.sr * clampr(hz, 20.0f, 0.45f * e.sr), qq), 1.0f / qq};
}

// soft clipper: x - 4/27 x^3 on |x| <= 1.5, +-1 beyond
[[nodiscard]] static inline real sat(real x) {
	x = clampr(x, -1.5f, 1.5f);
	return x * (1.0f - 0.148148148f * x * x);
}

static inline void process_output(Engine& e, real* __restrict__ out) {
	const bore::Bore& b = bore_of(e);
	const Params& P = e.ps;
	// 96 -> 48 kHz: 31 tap halfband (kaiser, -0.1 dB at 18 kHz)
	static constexpr real HB[8] = {-0.00012586f, 0.00106450f, -0.00377247f, 0.00980408f, -0.02159190f, 0.04398898f, -0.09309054f, 0.31373747f};
	// bell brightness: first order high shelf around 1.2 kHz, +-(bell * 2) dB
	const real gsh = exp2s(clampr(P.bell, -6.0f, 6.0f) * 2.0f * (1.0f / 6.02f)) - 1.0f;
	const real ash = clampr(2.0f * PI * 1200.0f / e.sr, 0.01f, 0.9f);
	const Svf2 fit_hp = svf_at(e, FIT_HP_HZ, 0.7f), fit_pk = svf_at(e, FIT_PK_HZ, FIT_PK_Q);
	const real fit_lo = FIT_LO, fit_pg = FIT_PK_G;
	const real adir = clampr(2.0f * PI * 2400.0f * (0.0615f / b.bell_r) / e.sr, 0.01f, 0.9f);
	// mutes (the radiated sound only: the mute's effect on the bore is the bore_loss / bell params)
	const real md = clampr(P.mute, 0.0f, 1.0f);
	const int mt = md > 0.0f ? e.mute_type : MUTE_NONE;
	// the plunger / hand: the tilt of the loudest held player
	{
		real best = -1.0f, y = 0.0f;
		for (int i = 0; i < NP; i++) {
			const Player& s = e.pl[i];
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
	const real g = 0.0107f * clampr(P.gain, 0.0f, 8.0f) * PRESET_GAIN[e.preset];
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
		room_buf[e.room_w & (ROOML - 1)] = y;
		real r = 0.0f;
		for (int k = 0; k < 4; k++) r += RG[k] * room_buf[(e.room_w - (uint32_t)RT[k]) & (ROOML - 1)];
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

static inline void process(Engine& e, real* __restrict__ output) {
	{
		const real* __restrict__ pt = reinterpret_cast<const real*>(&e.p);
		real* __restrict__ ps = reinterpret_cast<real*>(&e.ps);
		for (int i = 0; i < NPARAMS; i++) {
			const real d = pt[i] - ps[i];
			ps[i] = fabsf(d) < 1e-5f ? pt[i] : ps[i] + 0.05f * d;
		}
	}
	e.blocks++;
	const real sh = clampr(e.g.acc_abs - 1.1f, 0.0f, 1.0f);
	e.shake_pk = rmax(sh, e.shake_pk * (1.0f - e.block_dt * 2.5f));
	e.shake += 0.03f * (e.shake_pk - e.shake);
	process_keys(e);
	for (int n = 0; n < NS; n++) e.mix[n] = 0.0f;
	for (int i = 0; i < NP; i++)
		if (e.pl[i].active) process_player(e, i);
	process_output(e, output);
}

// ---------------------------------------------------------------------------------------------
// live parameters

struct ParamEntry {
	strisy::ParamDesc d;
	real Params::*m;
};

static constexpr strisy::ParamKind C_ = strisy::ParamKind::continuous;
static constexpr strisy::ParamKind S_ = strisy::ParamKind::steps;

static constexpr ParamEntry param_table[] = {
    {{"gain", "level", 0.1f, 4.0f, C_}, &Params::gain},
    {{"p_soft", "level", 0.3f, 8.0f, C_}, &Params::p_soft},     // pp mouth pressure re threshold
    {{"p_loud", "level", 2.0f, 20.0f, C_}, &Params::p_loud},    // ff mouth pressure (kPa at Bb4)
    {{"pres_ff", "level", 0.2f, 1.5f, C_}, &Params::pres_ff},   // key pressure that plays ff
    {{"resp_hz", "level", 3.0f, 100.0f, C_}, &Params::resp_hz}, // breath response
    {{"release", "level", 0.005f, 0.5f, C_}, &Params::release},
    {{"lip_tune", "lips", -300.0f, 300.0f, C_}, &Params::lip_tune}, // lip tension trim (cents)
    {{"lip_q", "lips", 0.5f, 400.0f, C_}, &Params::lip_q},
    {{"lip_mass", "lips", 0.3f, 3.0f, C_}, &Params::lip_mass},
    {{"lip_open", "lips", -0.6f, 0.6f, C_}, &Params::lip_open}, // rest opening (mm)
    {{"lip_width", "lips", 4.0f, 14.0f, C_}, &Params::lip_width},
    {{"tilt_lip", "lips", -1.0f, 1.0f, C_}, &Params::tilt_lip}, // tilt -> embouchure
    {{"cup", "bore", 0.3f, 2.0f, C_}, &Params::cup},            // cup volume (cm^3)
    {{"bore_loss", "bore", 0.0f, 1.0f, C_}, &Params::bore_loss},
    {{"bell", "bore", -6.0f, 6.0f, C_}, &Params::bell},    // brightness dB/oct above 1 kHz
    {{"brassy", "bore", 0.0f, 8.0f, C_}, &Params::brassy}, // wave steepening (1 = physical)
    {{"breath", "air", 0.0f, 1.0f, C_}, &Params::breath},
    {{"t_noise", "air", 0.0f, 1.0f, C_}, &Params::t_noise},
    {{"attack", "attack", 0.0f, 2.0f, C_}, &Params::attack}, // tonguing hardness
    {{"overshoot", "attack", 0.0f, 1.0f, C_}, &Params::overshoot},
    {{"vibrato", "mod", 0.0f, 1.0f, C_}, &Params::vibrato}, // pedal vibrato (semitones)
    {{"vib_hz", "mod", 3.0f, 8.0f, C_}, &Params::vib_hz},
    {{"shake", "mod", 0.0f, 2.0f, C_}, &Params::shake},
    {{"growl", "mod", 0.0f, 1.0f, C_}, &Params::growl},
    {{"fall", "bend", 0.0f, 12.0f, C_}, &Params::fall},
    {{"doit", "bend", 0.0f, 12.0f, C_}, &Params::doit},
    {{"halfvalve", "bend", 0.0f, 1.0f, C_}, &Params::halfvalve},
    {{"mute", "mute", 0.0f, 1.0f, C_}, &Params::mute},
    {{"wah", "mute", -1.0f, 1.0f, C_}, &Params::wah},
    {{"legato", "play", 0.0f, 2.0f, S_}, &Params::legato}, // 0 poly, 1 auto, 2 mono
    {{"slur_thr", "play", 0.0f, 1.0f, C_}, &Params::slur_thr},
    {{"valve_ms", "play", 3.0f, 60.0f, C_}, &Params::valve_ms},
    {{"voices", "play", 1.0f, (float)NP, S_}, &Params::voices},
    {{"room", "room", 0.0f, 1.0f, C_}, &Params::room},
};
static constexpr int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

} // namespace trumpet

namespace strisy {
template <> struct EngineParams<trumpet::Engine> {
	static constexpr int count = trumpet::NUM_LIVE_PARAMS;
	[[nodiscard]] static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &trumpet::param_table[i].d : nullptr; }
	[[nodiscard]] static float* ptr(trumpet::Engine& e, int i) {
		return (i >= 0 && i < count) ? &(e.p.*(trumpet::param_table[i].m)) : nullptr;
	}
	static void changed(trumpet::Engine& e, int) { trumpet::update_derived(e); }
};
} // namespace strisy

#endif // VOICE_TRUMPET_H_
