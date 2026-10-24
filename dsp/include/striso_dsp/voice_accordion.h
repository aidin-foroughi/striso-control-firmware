/**
 * Accordion: physically modelled free reeds with registers, musette tuning, reed chambers,
 * cassotto and a shared bellows (make VOICE=accordion, -DACCORDION_PRESET=n on the host).
 * API: see the comment at the top of synth_strisy.cpp; live parameters: strisy::EngineParams.
 *
 * Presets (level matched):
 *   0 clarinet   one 8' reed
 *   1 bassoon    one 16' reed (an octave down), in the cassotto
 *   2 piccolo    one 4' reed (an octave up)
 *   3 violin     two 8' reeds, the second slightly sharp: a light, slow tremolo
 *   4 musette    three 8' reeds (in tune, sharp, flat): the classic wet French musette
 *   5 organ      16' + 4'
 *   6 master     16' + 8' + 8' + 8' + 4', 16' and the first 8' in the cassotto
 *   7 bandoneon  16' + 8', dry, larger reeds on long plates: darker and rounder, slower to speak
 *   8 harmonium  8' + 4' reed organ: no reed chambers, a soft slow speech, one wind supply
 *
 * The free reed (Millot & Baumann 2007, Ricot, Causse & Misdariis 2005, Cottingham 2011): a
 * tongue vibrating through a slot in a plate, blown closed (the pressure pushes it into the slot).
 * Its pitch is its own (no air column): the reed is a lightly damped oscillator, nearly
 * sinusoidal, slightly flatter at high pressure. The sound comes from the air it lets through:
 *   x    = xe + A sin(phi)    tip position in plate thicknesses (the slot spans [-1, 0]); the
 *                             rest set is above the plate, the pressure pushes xe into the slot
 *   S(x) = r(x) + kd r(-1-x)  useful section: open above the plate (side and tip gaps, linear in
 *                             x), shut while the tongue is inside the slot (only the clearance
 *                             gap, which rounds the corners: r is a quadratic-then-linear ramp),
 *                             open again below the plate
 *   u    = vj sat(S / Sc) + kp o A cos(phi)
 *                             through-flow: the jet velocity vj = sqrt(dp) times the section,
 *                             saturating at the series constriction (pallet, chamber outlet);
 *                             plus the flow the tongue pumps (Millot & Baumann's u_p)
 * Small amplitudes stay above the slot (a nearly sinusoidal flow: soft is dark); larger ones
 * enter and cross the slot, cutting the flow into pulses (loud is bright, harmonic rich). The
 * radiated sound is du/dt (a monopole), summed per reed bank, then the reed chamber resonance
 * (one per bank: the 16', the first 8' (cassotto), the other 8's (musette), the 4'), the
 * optional cassotto (a second chamber: lowpass, darker and smoother), and the grille/body.
 *
 * The reed amplitude A is not solved sample by sample: it follows the self-excited
 * oscillator's amplitude equation per block (logistic growth above the threshold pressure at a
 * rate that is slower for big low reeds, decay at the reed's damping below it, a lower sustain
 * threshold than onset threshold: the free reed's inverse bifurcation), seeded by the static
 * deflection a fast pressure rise gives the tongue. The phase is a rotating phasor at the reed's
 * frequency: exact tuning, bends and any microtonal note, minus the pressure flattening
 * (stable at low pressure, a few cents flat when pushed hard; more on the 16') and the pallet
 * bend (a strong bellows on a barely open pallet bends the note down, as accordionists do).
 *
 * Bellows: the Striso has a pressure per key, the accordion one bellows. Each key opens its
 * pallet (throttling the reed's pressure while barely open); the pressure behind the pallets is
 *   (1 - bellows) * own pressure + bellows * shared bellows pressure (the strongest key, smoothed)
 * so 0 is per-note expression and 1 a real accordion (one bellows, keys only open pallets).
 * Bellows reversal: on a pedal press (and on a vigorous shake: the bellows shake) the pressure
 * dips through zero and the other reed of each pair (pull instead of push: its own tuning
 * error) starts speaking.
 *
 * Musette: the second 8' reed is sharp and the third flat by `beat` Hz at A4, with
 * beat_track 0 the same beat rate across the range (as tuners do), 1 constant cents.
 *
 * How to play:
 *   pres   bellows/pallet: below the threshold only air; then the reeds speak, growing louder
 *          and brighter and a little flatter with pressure. With `bellows` up, the strongest
 *          key sets the pressure for the whole chord and light keys just open their pallets.
 *   but_y  tilt: the reed voicing, forward a little brighter, back softer.
 *   note   pitch and bends (reeds follow exactly).
 *   pedal  press: bellows reversal. Shaking the Striso: bellows shake.
 *   vpres  unused (the attack follows the pressure rise itself).
 * No libm; no divisions or square roots in the sample loops.
 */
#ifndef VOICE_ACCORDION_H_
#define VOICE_ACCORDION_H_

#include <stdint.h>
#include <math.h>
#include "strisy.h"

namespace accordion {

using real = float;
static constexpr int BLOCK = strisy::BLOCK;
static constexpr int MAX_VOICES = 10;
static constexpr int NRANK = 5; // 16', 8' (1st, cassotto), 8' sharp, 8' flat, 4'
static constexpr int NBUS = 4;  // 16', 8' first, 8' musette, 4'
static constexpr int FADE_BLOCKS = 4;
static constexpr int rank_bus[NRANK] = {0, 1, 2, 2, 3};
static constexpr real rank_oct[NRANK] = {0.5f, 1.0f, 1.0f, 1.0f, 2.0f};

using strisy::rmax;
using strisy::rmin;
[[nodiscard]] static inline real clampr(real x, real lo, real hi) { return rmin(rmax(x, lo), hi); }

[[nodiscard]] static inline real fsqrt(real x) {
#if defined(__arm__) && defined(__ARM_FP)
	real r;
	__asm__("vsqrt.f32 %0, %1"
	        : "=t"(r)
	        : "t"(x));
	return r;
#else
	return __builtin_sqrtf(x);
#endif
}

// 2^p for |p| < 60, relative error ~1e-7 (block rate)
[[nodiscard]] static inline real pow2(real p) {
	p = clampr(p, -60.0f, 60.0f);
	int w = (int)p;
	if ((real)w > p) w--;
	const real z = p - (real)w;
	const real f = 1.0f + z * (0.693146839f + z * (0.240231090f + z * (0.0554789288f + z * (0.00968616940f + z * (0.00123824704f + z * 0.000218713408f)))));
	union {
		uint32_t i;
		float f;
	} e = {(uint32_t)(w + 127) << 23};
	return f * e.f;
}
[[nodiscard]] static inline real exp_neg(real x) { return pow2(-1.442695f * x); } // e^-x
// log2(x) for x > 0, error ~1e-4 (block rate)
[[nodiscard]] static inline real log2_fast(real x) {
	union {
		float f;
		uint32_t i;
	} u = {rmax(x, 1e-30f)};
	const real ex = (real)((int)((u.i >> 23) & 0xffu) - 127);
	u.i = (u.i & 0x007fffffu) | 0x3f800000u; // mantissa in [1, 2)
	const real m = u.f - 1.0f;
	return ex + m * (1.4425449f + m * (-0.7181452f + m * (0.4575485f + m * (-0.2779042f + m * (0.1217970f + m * -0.0258411f)))));
}
// log2(f / 440)
[[nodiscard]] static inline real fast_log2_ratio(real f) { return log2_fast(f * (1.0f / 440.0f)); }
[[nodiscard]] static inline real cos_poly(real x) {
	const real x2 = x * x;
	return 1.0f + x2 * (-0.5f + x2 * (1.0f / 24 + x2 * (-1.0f / 720 + x2 * (1.0f / 40320))));
}
[[nodiscard]] static inline real sin_poly(real x) {
	const real x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
[[nodiscard]] static inline real db2lin(real db) { return pow2(db * 0.16609640f); }
[[nodiscard]] static inline uint32_t hash32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
// uniform in [-1, 1)
[[nodiscard]] static inline real hash_unit(uint32_t x) { return (real)(int32_t)hash32(x) * (1.0f / 2147483648.0f); }

// ---------------------------------------------------------------------------------------------
// Sound design parameters (all live, see param_table)

struct Params {
	// registers
	real level;      // output level
	real r16;        // 16' (bassoon) reed level, 0 = off
	real r8a;        // 8' (clarinet) reed
	real r8b;        // 8' sharp (violin/musette)
	real r8c;        // 8' flat (musette)
	real r4;         // 4' (piccolo)
	real beat;       // musette beat rate at A4, Hz (8' sharp reed +beat, flat reed -beat * beat_flat)
	real beat_flat;  // flat reed's beat relative to the sharp one's
	real beat_track; // 0 the same beat in Hz across the range .. 1 the same cents
	real cassotto;   // 16' and first 8' through the tone chamber (on/off)
	// bellows
	real bellows;    // 0 per-note pressure .. 1 one shared bellows
	real bel_ms;     // bellows smoothing, ms
	real valve;      // key pressure that opens the pallet fully
	real throttle;   // pallet throttling (reed section / full pallet section)
	real chamber_ms; // pressure build-up behind an opening pallet, ms
	real release_ms; // pallet closing, ms
	// reed
	real speak_ms;  // time for an 8' A4 reed to speak at mezzo forte, ms
	real low_slow;  // onset slower for lower reeds: exponent of the period
	real thresh;    // threshold pressure (oscillation onset)
	real amp;       // tip amplitude at full pressure, plate thicknesses
	real rest;      // rest set of the tip above the plate, plate thicknesses
	real push;      // tip pushed into the slot at full pressure
	real gap;       // clearance gap (rounds the cut-off)
	real exit;      // exit below the plate: quadratic part (the tongue leaves tip first)
	real down;      // opening below the plate relative to above
	real constrict; // series constriction: the section where the flow saturates
	real pump;      // flow pumped by the tongue itself
	real flat;      // pitch drop at full pressure, cents (8'; 16' x1.3, 4' x0.6)
	real bend;      // pallet bend: cents down at full bellows on a barely open pallet
	real spread;    // reed tuning tolerance, +- cents
	// tone
	real ch_hz;     // reed chamber resonance, Hz (8')
	real ch_db;     // its peak
	real ch_q;      // its Q
	real cas_hz;    // cassotto lowpass, Hz
	real cas_leak;  // highs passing the cassotto
	real grille_hz; // grille / body lowpass, Hz
	real tilt;      // reed voicing with tilt y
	// articulation
	real rev_pedal; // bellows reversal on pedal press (on/off)
	real rev_shake; // bellows reversal on shaking (on/off)
	real rev_ms;    // reversal gap, ms
};
static constexpr int NPARAMS = sizeof(Params) / sizeof(real);

static constexpr int NUM_PRESETS = 9;
static constexpr const char* preset_names[NUM_PRESETS] = {
    "clarinet", "bassoon", "piccolo", "violin", "musette", "organ", "master", "bandoneon", "harmonium"};

[[nodiscard]] static inline const char* preset_name(int i) {
	return (i >= 0 && i < NUM_PRESETS) ? preset_names[i] : preset_names[0];
}

[[nodiscard]] static inline Params base_params() {
	Params p;
	p.level = 1.0f;
	p.r16 = 0.0f;
	p.r8a = 1.0f;
	p.r8b = 0.0f;
	p.r8c = 0.0f;
	p.r4 = 0.0f;
	p.beat = 1.3f;
	p.beat_flat = 1.0f;
	p.beat_track = 0.0f;
	p.cassotto = 0.0f;
	p.bellows = 0.5f;
	p.bel_ms = 40.0f;
	p.valve = 0.25f;
	p.throttle = 0.35f;
	p.chamber_ms = 10.0f;
	p.release_ms = 18.0f;
	p.speak_ms = 35.0f;
	p.low_slow = 0.35f;
	p.thresh = 0.025f;
	p.amp = 3.5f;
	p.rest = 0.9f;
	p.push = 0.3f;
	p.gap = 0.12f;
	p.exit = 0.6f;
	p.down = 1.5f;
	p.constrict = 1.6f;
	p.pump = 0.5f;
	p.flat = 3.0f;
	p.bend = 30.0f;
	p.spread = 0.5f;
	p.ch_hz = 2500.0f;
	p.ch_db = 13.0f;
	p.ch_q = 1.4f;
	p.cas_hz = 1600.0f;
	p.cas_leak = 0.45f;
	p.grille_hz = 7500.0f;
	p.tilt = 0.3f;
	p.rev_pedal = 1.0f;
	p.rev_shake = 1.0f;
	p.rev_ms = 80.0f;
	return p;
}

[[nodiscard]] static inline Params preset_params(int i) {
	Params p = base_params();
	switch (i) {
	default:
	case 0: // clarinet
		p.level = 0.95f;
		break;
	case 1: // bassoon
		p.level = 2.1f;
		p.r8a = 0.0f;
		p.r16 = 1.0f;
		p.cassotto = 1.0f;
		break;
	case 2: // piccolo
		p.level = 0.88f;
		p.r8a = 0.0f;
		p.r4 = 1.0f;
		break;
	case 3: // violin: two 8', a light tremolo
		p.level = 0.75f;
		p.r8b = 1.0f;
		p.beat = 1.3f;
		break;
	case 4: // musette: three 8', wet French tuning
		p.level = 0.62f;
		p.r8b = 1.0f;
		p.r8c = 1.0f;
		p.beat = 5.2f;
		break;
	case 5: // organ: 16' + 4'
		p.level = 0.82f;
		p.r8a = 0.0f;
		p.r16 = 1.0f;
		p.r4 = 0.8f;
		break;
	case 6: // master
		p.level = 0.5f;
		p.r16 = 1.0f;
		p.r8b = 0.9f;
		p.r8c = 0.9f;
		p.r4 = 0.8f;
		p.beat = 2.6f;
		p.cassotto = 1.0f;
		break;
	case 7: // bandoneon: 16' + 8' in octaves, dry; big reeds on long zinc plates
		p.level = 0.85f;
		p.r16 = 1.0f;
		p.r8a = 0.9f;
		p.speak_ms = 50.0f;
		p.rest = 1.1f;
		p.amp = 2.6f;
		p.constrict = 1.2f;
		p.down = 1.1f;
		p.pump = 1.0f;
		p.ch_hz = 1300.0f;
		p.ch_db = 9.0f;
		p.ch_q = 1.2f;
		p.grille_hz = 4500.0f;
		p.flat = 5.0f;
		break;
	case 8: // harmonium: 8' + 4' reed organ, no reed chambers, one wind supply
		p.level = 1.65f;
		p.r4 = 0.5f;
		p.bellows = 1.0f;
		p.bel_ms = 90.0f;
		p.speak_ms = 70.0f;
		p.chamber_ms = 25.0f;
		p.release_ms = 30.0f;
		p.rest = 1.0f;
		p.amp = 2.4f;
		p.ch_hz = 1500.0f;
		p.ch_db = 3.0f;
		p.ch_q = 0.8f;
		p.grille_hz = 4200.0f;
		p.rev_pedal = 0.0f;
		p.rev_shake = 0.0f;
		break;
	}
	return p;
}

// ---------------------------------------------------------------------------------------------
// State

struct Reed {
	real c, s;   // phasor: displacement sin, velocity cos
	real A;      // tip amplitude (plate thicknesses)
	real xe;     // equilibrium tip position at the end of the last block
	real vj, pg; // through-flow and pump gains at the end of the last block
	real tune;   // tuning error of this reed (cents), per bellows direction
	int on;      // processed (sounding or starting)
};

struct VoiceState {
	int active;
	int quiet;
	int fade;      // blocks left of a fade-out before the note change
	real note;     // note the reeds play (held during a fade)
	real o;        // pallet opening 0..1
	real dp;       // pressure at the reeds (behind the pallet)
	real dp_blk;   // dp at the previous block (onset kick)
	real fg;       // fade gain at the end of the last block
	uint32_t seed; // note-on counter for the reed tuning errors and phases
	Reed r[NRANK];
};

struct Engine {
	strisy::GlobalControls g;
	strisy::VoiceControls vc[MAX_VOICES];
	int nvoices;
	int preset;
	real sr, inv_sr;
	Params p;  // as set (presets, live edits)
	Params ps; // smoothed copy used by the DSP
	// bellows
	real bel;       // shared bellows pressure
	real rev_g;     // reversal envelope 1 .. 0
	real rev_t;     // time into the reversal (s), < 0: none
	int dir;        // bellows direction (push 0 / pull 1)
	int pedal_dn;   // pedal pressed (hysteresis)
	int shake_arm;  // shake re-armed
	real shake_t;   // time since the last shake reversal (s)
	uint32_t rng;   // phases
	uint32_t nseed; // note-on counter
	// output stage (per bus: radiation difference, chamber, cassotto)
	real up[NBUS];
	strisy::SvfState chs[NBUS], cas[NBUS];
	strisy::SvfState gr1, gr2;
	real casmix; // cassotto amount, smoothed
	VoiceState vs[MAX_VOICES];
	real bus[NBUS][BLOCK];
};

static inline void reset_voice(VoiceState& s) {
	const uint32_t seed = s.seed;
	const real note = s.note;
	s = VoiceState();
	s.seed = seed;
	s.note = note;
	s.fg = 1.0f;
}

// Select a preset (between blocks, any time); sounding notes continue with the new registers.
static inline void set_preset(Engine& e, int i) {
	if (i < 0 || i >= NUM_PRESETS) i = 0;
	e.preset = i;
	e.p = preset_params(i);
	e.ps = e.p;
	e.casmix = e.p.cassotto > 0.5f ? 1.0f : 0.0f;
}

// preset after init() (host renders: -DACCORDION_PRESET=n)
#ifdef ACCORDION_PRESET
static constexpr int DEFAULT_PRESET = ACCORDION_PRESET;
#else
static constexpr int DEFAULT_PRESET = 0;
#endif

static inline void init(Engine& e, int sample_rate, int nvoices) {
	e.sr = (real)sample_rate;
	e.inv_sr = 1.0f / e.sr;
	e.nvoices = nvoices < MAX_VOICES ? nvoices : MAX_VOICES;
	e.g = {1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 3.0f / 16};
	e.bel = 0.0f;
	e.rev_g = 1.0f;
	e.rev_t = -1.0f;
	e.dir = 0;
	e.pedal_dn = 0;
	e.shake_arm = 1;
	e.shake_t = 1.0f;
	e.rng = 0x2545F491u;
	e.nseed = 0;
	for (int b = 0; b < NBUS; b++) {
		e.up[b] = 0.0f;
		e.chs[b] = e.cas[b] = strisy::SvfState();
		for (int n = 0; n < BLOCK; n++) e.bus[b][n] = 0.0f;
	}
	e.gr1 = e.gr2 = strisy::SvfState();
	for (int v = 0; v < MAX_VOICES; v++) {
		e.vc[v] = {69.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		e.vs[v].seed = 0;
		e.vs[v].note = 69.0f;
		reset_voice(e.vs[v]);
	}
	set_preset(e, DEFAULT_PRESET);
}

[[nodiscard]] static inline const real* rank_level(const Params& q) { return &q.r16; }

// start the reeds of a voice for a new note (new key: new reeds, random phases, tuning errors)
static inline void start_reeds(Engine& e, VoiceState& s) {
	s.seed = ++e.nseed;
	for (int r = 0; r < NRANK; r++) {
		Reed& d = s.r[r];
		e.rng = e.rng * 1664525u + 1013904223u;
		const real ph = hash_unit(e.rng) * 1.5707963f; // +-90 degrees, then a random half turn
		const real sg = (e.rng & 0x100u) ? 1.0f : -1.0f;
		d.c = sg * cos_poly(ph);
		d.s = sg * sin_poly(ph);
		d.A = 0.0f;
		d.xe = e.ps.rest;
		d.vj = d.pg = 0.0f;
		d.on = 0;
	}
}

// tuning error of reed r of a note: fixed per key and bellows direction (each direction has its
// own reed); the note is rounded at note-on, so bends don't change it
static inline void tune_reeds(const Engine& e, VoiceState& s) {
	const uint32_t key = (uint32_t)(int)(s.note + 0.5f);
	for (int r = 0; r < NRANK; r++)
		s.r[r].tune = e.ps.spread * hash_unit(key * 2654435761u + (uint32_t)r * 40503u + (uint32_t)e.dir * 977u + 12345u);
}

// The free reed's flow for a tip position x: section above and below the plate, saturating at
// the series constriction (see the top of the file)
[[nodiscard]] static inline real reed_flow(real x, real du, real hu, real dd, real hd, real kd, real isc) {
	// above the plate: linear opening, rounded by the clearance gap
	const real m1 = rmax(x, 0.0f);
	const real c1 = rmin(m1, du);
	const real r1 = m1 - c1 + c1 * c1 * hu;
	// below the plate: the tongue leaves tip first (quadratic, then linear)
	const real m2 = rmax(-1.0f - x, 0.0f);
	const real c2 = rmin(m2, dd);
	const real r2 = m2 - c2 + c2 * c2 * hd;
	// series constriction: smooth saturation (cubic, flat at q = 1.5)
	const real q = rmin((r1 + kd * r2) * isc, 1.5f);
	return q - 0.148148148f * q * q * q;
}

// One reed over the block: phasor, section, saturation, pump; adds its flow to bus.
// Linear ramps of A, xe, vj, pg from the previous block's end values (no steps).
// Two samples per iteration, both rotated from the same phasor value: two independent chains
// for the in-order FPU.
static inline void run_reed(Reed& d, real* __restrict__ bus, real cw, real sw, real A1, real xe1, real vj1, real pg1,
    real du, real dd, real kd, real isc) {
	constexpr real inv = 1.0f / BLOCK;
	const real cw2 = cw * cw - sw * sw, sw2 = 2.0f * sw * cw;
	real cc = d.c, ss = d.s;
	real A = d.A, xe = d.xe, vj = d.vj, pg = d.pg;
	const real dA = (A1 - A) * inv, dxe = (xe1 - xe) * inv, dvj = (vj1 - vj) * inv, dpg = (pg1 - pg) * inv;
	const real hu = 0.5f / du, hd = 0.5f / dd;
	for (int n = 0; n < BLOCK; n += 2) {
		const real ca = cc * cw - ss * sw, sa = cc * sw + ss * cw;
		const real cb = cc * cw2 - ss * sw2, sb = cc * sw2 + ss * cw2;
		cc = cb;
		ss = sb;
		const real Aa = A + dA, Ab = A + 2.0f * dA;
		const real xa = (xe + dxe) + Aa * sa, xb = (xe + 2.0f * dxe) + Ab * sb;
		const real ya = reed_flow(xa, du, hu, dd, hd, kd, isc);
		const real yb = reed_flow(xb, du, hu, dd, hd, kd, isc);
		bus[n] += (vj + dvj) * ya + (pg + dpg) * Aa * ca;
		bus[n + 1] += (vj + 2.0f * dvj) * yb + (pg + 2.0f * dpg) * Ab * cb;
		A = Ab;
		xe += 2.0f * dxe;
		vj += 2.0f * dvj;
		pg += 2.0f * dpg;
	}
	// renormalize the phasor against rounding drift
	const real gn = 1.5f - 0.5f * (cc * cc + ss * ss);
	d.c = cc * gn;
	d.s = ss * gn;
	d.A = A1;
	d.xe = xe1;
	d.vj = vj1;
	d.pg = pg1;
}

static inline void process_voice(Engine& e, int v) {
	const strisy::VoiceControls c = e.vc[v];
	VoiceState& s = e.vs[v];
	const Params& q = e.ps;
	const real pres = (c.pres == c.pres) ? clampr(c.pres, 0.0f, 1.5f) : 0.0f;
	const real note_in = (c.note == c.note) ? clampr(c.note, 0.0f, 140.0f) : 69.0f;
	const real dt = (real)BLOCK * e.inv_sr;

	if (!s.active) {
		if (pres <= 0.0f) {
			s.note = note_in;
			return;
		}
		reset_voice(s);
		s.note = note_in;
		s.active = 1;
		start_reeds(e, s);
		tune_reeds(e, s);
	}
	// a jump of a semitone or more: the voice went to another key. Fade the old reeds out over
	// a few blocks (their pallet closes), then start the new key's reeds.
	// (fade FADE_BLOCKS + 1 .. 2 ramps the gain down to 0, at 1 the new reeds start)
	if (s.fade == 0 && fabsf(note_in - s.note) >= 0.99f) s.fade = FADE_BLOCKS + 1;
	if (s.fade == 1) {
		s.fade = 0;
		s.note = note_in;
		s.o = 0.0f;
		s.dp = s.dp_blk = 0.0f;
		start_reeds(e, s);
		tune_reeds(e, s);
	} else if (s.fade > 1) {
		s.fade--;
	} else {
		s.note = note_in;
	}
	const real fg = s.fade > 0 ? (real)(s.fade - 1) * (1.0f / FADE_BLOCKS) : 1.0f;

	// pallet and pressure behind it
	const real kopen = 1.0f - exp_neg(dt * (1000.0f / 2.0f));
	const real kclose = 1.0f - exp_neg(dt * 1000.0f / clampr(q.release_ms, 2.0f, 500.0f));
	const real ot = clampr(pres / clampr(q.valve, 0.02f, 1.5f), 0.0f, 1.0f);
	s.o += (ot - s.o) * (ot > s.o ? kopen : kclose);
	const real kap = clampr(q.throttle, 0.05f, 2.0f);
	const real o2 = s.o * s.o;
	const real tau = o2 * (1.0f + kap * kap) / (o2 + kap * kap);
	const real bl = clampr(q.bellows, 0.0f, 1.0f);
	const real src = bl * e.bel + (1.0f - bl) * pres;
	const real dpt = src * tau;
	const real kch = 1.0f - exp_neg(dt * 1000.0f / clampr(q.chamber_ms, 1.0f, 200.0f));
	s.dp += (dpt - s.dp) * (dpt > s.dp ? kch : kclose);
	if (!(s.dp == s.dp)) s.dp = 0.0f;
	// bellows reversal: the pressure at the reeds goes through zero (exactly 0 at the turn)
	const real rg = e.rev_g;
	const real dp = rmax(s.dp, 0.0f) * rg * rg;
	const real vj = fsqrt(rmax(s.dp, 0.0f)) * rg * fg;
	const real rise = rmax(dp - s.dp_blk, 0.0f);
	s.dp_blk = dp;

	// pitch: pressure flattening (stable at low pressure) and the pallet bend
	const real hp = rmax(dp - 0.4f, 0.0f);
	const real flat_c = -q.flat * hp * hp * (1.0f / 0.36f);
	const real bend_c = -q.bend * bl * e.bel * rg * rg * (1.0f - tau) * (s.o > 0.01f ? 1.0f : 0.0f);
	const real f8 = 440.0f * pow2((s.note - 69.0f) * (1.0f / 12.0f) + (bend_c) * (1.0f / 1200.0f));
	const real beat_a4 = clampr(q.beat, 0.0f, 20.0f);
	const real btr = clampr(q.beat_track, 0.0f, 1.0f);
	const real beat8 = beat_a4 * pow2(btr * fast_log2_ratio(f8));

	// tilt: voicing
	const real ty = clampr(c.but_y, -1.0f, 1.0f) * q.tilt;
	const real isc0 = pow2(0.8f * ty) / clampr(q.constrict, 0.2f, 6.0f);

	const real* lv = rank_level(q);
	const real thr = clampr(q.thresh, 0.002f, 0.5f);
	const real speak = clampr(q.speak_ms, 3.0f, 500.0f) * 0.001f;
	const real sig0 = 6.8f / speak; // logistic 1% -> 90% in speak at A4
	real peak = 0.0f;
	int sounding = 0;
	for (int r = 0; r < NRANK; r++) {
		Reed& d = s.r[r];
		const real lvl = clampr(lv[r], 0.0f, 2.0f);
		const int b = rank_bus[r];
		const real fm = r == 0 ? 1.3f : (r == 4 ? 0.6f : 1.0f);
		real f = f8 * rank_oct[r] * pow2((flat_c * fm + d.tune) * (1.0f / 1200.0f));
		if (r == 2) f += beat8;
		if (r == 3) f -= beat8 * clampr(q.beat_flat, 0.0f, 3.0f);
		// the registers' compass: fade out below 25 Hz and above 5 kHz
		const real comp = clampr((f - 18.0f) * (1.0f / 7.0f), 0.0f, 1.0f) * clampr((9000.0f - f) * (1.0f / 4000.0f), 0.0f, 1.0f);
		const real g = lvl * comp;
		if (g <= 1e-4f && d.vj < 1e-5f && d.pg < 1e-5f) {
			d.on = 0;
			d.A = 0.0f;
			d.vj = d.pg = 0.0f;
			continue;
		}
		const real w = clampr(6.2831853f * f * e.inv_sr, 0.0f, 1.45f);
		const real cw = cos_poly(w), sw = sin_poly(w);
		// amplitude equation: threshold (lower once speaking), growth, saturation, decay
		const real th = d.A > 0.05f ? 0.6f * thr : thr;
		const real z = rmax(dp - th, 0.0f);
		const real At = clampr(q.amp, 0.3f, 6.0f) * z * 1.6f / (z + 0.6f);
		real A = d.A;
		if (At > A && z > 0.0f) {
			// seed: turbulence floor and the static deflection of a fast pressure rise
			const real kick = clampr(q.push, 0.0f, 1.5f) * rise * rmin(1.0f, 0.5f / (f * dt));
			A = rmax(A + kick, 0.004f * At);
			// logistic growth, exact over the block; slower for long reeds, faster when pushed
			const real sig = sig0 * pow2(clampr(q.low_slow, 0.0f, 1.0f) * fast_log2_ratio(f)) * fsqrt(rmin(z * 2.0f, 4.0f));
			const real ex = exp_neg(rmin(sig * dt, 20.0f));
			A = At / (1.0f + (At / A - 1.0f) * ex);
		} else {
			// decay at the reed's damping (Q ~ 60, at least 1/0.12 s)
			const real sd = rmax(3.1415927f * f * (1.0f / 60.0f), 8.0f);
			A = At + (A - At) * exp_neg(rmin(sd * dt, 20.0f));
		}
		A = clampr(A, 0.0f, 8.0f);
		const real xe1 = clampr(q.rest, -1.0f, 3.0f) - clampr(q.push, 0.0f, 1.5f) * dp;
		// corners at least 1.5 samples wide (band limiting), else the clearance / exit shapes
		const real aa = 1.5f * A * w;
		const real du = rmax(clampr(q.gap, 0.01f, 1.0f), aa);
		const real dd = rmax(clampr(q.exit, 0.01f, 2.0f), aa);
		const real vj1 = vj * g;
		const real pg1 = clampr(q.pump, 0.0f, 2.0f) * s.o * rg * fg * g;
		run_reed(d, e.bus[b], cw, sw, A, xe1, vj1, pg1, du, dd, clampr(q.down, 0.0f, 3.0f), isc0);
		d.on = 1;
		sounding = 1;
		peak = rmax(peak, A);
	}
	(void)sounding;

	// safety: reset a voice whose state went non-finite or absurd
	real chk = s.dp + s.o;
	for (int r = 0; r < NRANK; r++) chk += fabsf(s.r[r].c) + fabsf(s.r[r].s) + s.r[r].A;
	if (!(chk < 1000.0f)) {
		reset_voice(s);
		s.note = note_in;
		return;
	}
	if (pres <= 0.0f && s.dp < 1e-9f && s.o < 1e-5f && s.fade == 0) {
		if (++s.quiet > 3) {
			s.active = 0;
			for (int r = 0; r < NRANK; r++) s.r[r].A = s.r[r].vj = s.r[r].pg = 0.0f;
		}
	} else {
		s.quiet = 0;
	}
	(void)peak;
}

[[nodiscard]] static inline int active_voices(const Engine& e) {
	int n = 0;
	for (int v = 0; v < e.nvoices; v++) n += e.vs[v].active;
	return n;
}

[[nodiscard]] static inline strisy::SvfCoef svf_hz(real hz, real sr, real Q) {
	return strisy::svf_from_g(strisy::tan0(clampr(hz, 20.0f, 0.42f * sr) * 3.1415927f / sr), Q);
}

// bellows: the shared pressure, reversal triggers and the reversal's pressure dip
static inline void bellows(Engine& e, real dt) {
	const Params& q = e.ps;
	real mx = 0.0f;
	for (int v = 0; v < e.nvoices; v++) {
		const real p = e.vc[v].pres;
		if (p == p && p > mx) mx = p;
	}
	mx = rmin(mx, 1.5f);
	e.bel += (mx - e.bel) * (1.0f - exp_neg(dt * 1000.0f / clampr(q.bel_ms, 2.0f, 500.0f)));
	if (!(e.bel == e.bel)) e.bel = 0.0f;

	bool trig = false;
	const real ped = e.g.pedal;
	if (!e.pedal_dn && ped > 0.6f) {
		e.pedal_dn = 1;
		trig = trig || q.rev_pedal > 0.5f;
	} else if (e.pedal_dn && ped < 0.4f) {
		e.pedal_dn = 0;
	}
	const real a = fabsf(e.g.acc_abs - 1.0f);
	e.shake_t += dt;
	if (e.shake_arm && a > 0.8f && e.shake_t > 0.08f) {
		e.shake_arm = 0;
		e.shake_t = 0.0f;
		trig = trig || q.rev_shake > 0.5f;
	} else if (a < 0.4f) {
		e.shake_arm = 1;
	}
	const real T = clampr(q.rev_ms, 10.0f, 400.0f) * 0.001f;
	if (trig && (e.rev_t < 0.0f || e.rev_t > 0.5f * T)) e.rev_t = 0.0f;
	if (e.rev_t >= 0.0f) {
		const real t0 = e.rev_t;
		e.rev_t += dt;
		// the turn: the other reed of each pair takes over, nearly at rest
		if (t0 < 0.5f * T && e.rev_t >= 0.5f * T) {
			e.dir ^= 1;
			for (int v = 0; v < e.nvoices; v++) {
				VoiceState& s = e.vs[v];
				if (!s.active) continue;
				for (int r = 0; r < NRANK; r++) s.r[r].A *= 0.1f;
				tune_reeds(e, s);
			}
		}
		if (e.rev_t >= T) {
			e.rev_t = -1.0f;
			e.rev_g = 1.0f;
		} else {
			// raised cosine dip through zero
			const real x = e.rev_t / T;                  // 0..1
			const real ph = 6.2831853f * x - 3.1415927f; // -pi..pi
			const real h = ph < -1.5707963f ? -3.1415927f - ph : (ph > 1.5707963f ? 3.1415927f - ph : ph);
			const real cph = ph < -1.5707963f || ph > 1.5707963f ? -cos_poly(h) : cos_poly(h); // cos(ph)
			e.rev_g = 0.5f - 0.5f * cph;
		}
	}
}

static inline void process(Engine& e, real* __restrict__ output) {
	// smooth the live parameters towards the written values (~55 ms)
	{
		real* a = &e.ps.level;
		const real* t = &e.p.level;
		for (int i = 0; i < NPARAMS; i++) {
			const real d = t[i] - a[i];
			a[i] = (d == d) ? a[i] + 0.012f * d : t[i];
		}
		// a rank switched off fades out, then stops
		real* lv = &e.ps.r16;
		const real* lt = &e.p.r16;
		for (int r = 0; r < NRANK; r++)
			if (lt[r] <= 0.0f && lv[r] < 2e-3f) lv[r] = 0.0f;
	}
	const Params& q = e.ps;
	const real sr = e.sr;
	const real dt = (real)BLOCK * e.inv_sr;
	bellows(e, dt);
	e.casmix += ((q.cassotto > 0.5f ? 1.0f : 0.0f) - e.casmix) * 0.03f;

	for (int b = 0; b < NBUS; b++)
		for (int n = 0; n < BLOCK; n++) e.bus[b][n] = 0.0f;
	for (int v = 0; v < e.nvoices; v++) process_voice(e, v);

	// per bank: radiation (du/dt), reed chamber resonance, cassotto (16' and first 8')
	static constexpr real ch_f[NBUS] = {0.88f, 1.0f, 1.0f, 0.97f};
	static constexpr real ch_qm[NBUS] = {0.8f, 1.0f, 1.0f, 2.5f};
	static constexpr real ch_dbo[NBUS] = {0.0f, 0.0f, 0.0f, -7.0f};
	const real rad = sr * (1.0f / (6.2831853f * 1000.0f)); // du/dt normalized at 1 kHz
	const strisy::SvfCoef cc = svf_hz(q.cas_hz, sr, 0.8f);
	const real leak = clampr(q.cas_leak, 0.0f, 1.0f);
	real mix[BLOCK];
	for (int n = 0; n < BLOCK; n++) mix[n] = 0.0f;
	for (int b = 0; b < NBUS; b++) {
		const strisy::SvfCoef ch = svf_hz(q.ch_hz * ch_f[b], sr, clampr(q.ch_q * ch_qm[b], 0.3f, 10.0f));
		const real gch = (db2lin(clampr(q.ch_db + ch_dbo[b], -12.0f, 24.0f)) - 1.0f) * ch.k;
		const real cm = b < 2 ? e.casmix : 0.0f;
		real u0 = e.up[b];
		real a1 = e.chs[b].ic1, a2 = e.chs[b].ic2, k1 = e.cas[b].ic1, k2 = e.cas[b].ic2;
		const real* in = e.bus[b];
		for (int n = 0; n < BLOCK; n++) {
			const real u = fabsf(in[n]) < 1e5f ? in[n] : 0.0f; // also catches NaN
			const real d = (u - u0) * rad;
			u0 = u;
			real v1, v2;
			SVF_TICK(d, a1, a2, ch, v1, v2);
			(void)v2;
			real y = d + gch * v1;
			if (cm > 0.0f) {
				real w1, w2;
				SVF_TICK(y, k1, k2, cc, w1, w2);
				(void)w1;
				const real yc = 1.2f * (w2 * (1.0f - leak) + leak * y);
				y += cm * (yc - y);
			}
			mix[n] += y;
		}
		if (!(fabsf(a1) + fabsf(a2) + fabsf(k1) + fabsf(k2) + fabsf(u0) < 1e6f)) a1 = a2 = k1 = k2 = u0 = 0.0f;
		e.up[b] = u0;
		e.chs[b] = {a1, a2};
		e.cas[b] = {k1, k2};
	}

	// grille and body: resonant lowpass, then a gentle second one; level, soft limit above 4
	const strisy::SvfCoef g1 = svf_hz(q.grille_hz, sr, 1.6f);
	const strisy::SvfCoef g2 = svf_hz(q.grille_hz * 1.6f, sr, 0.55f);
	const real lvl = clampr(q.level, 0.0f, 4.0f) * 0.21f;
	real b1 = e.gr1.ic1, b2 = e.gr1.ic2, c1 = e.gr2.ic1, c2 = e.gr2.ic2;
	for (int n = 0; n < BLOCK; n++) {
		real v1, v2, w1, w2;
		SVF_TICK(mix[n], b1, b2, g1, v1, v2);
		SVF_TICK(v2, c1, c2, g2, w1, w2);
		(void)v1;
		(void)w1;
		const real y = w2 * lvl;
		const real ay = fabsf(y);
		const real o = ay > 4.0f ? 4.0f + (ay - 4.0f) / (1.0f + 0.25f * (ay - 4.0f)) : ay;
		output[n] = clampr(y < 0.0f ? -o : o, -8.0f, 8.0f);
	}
	if (!(fabsf(b1) + fabsf(b2) + fabsf(c1) + fabsf(c2) < 1e6f)) b1 = b2 = c1 = c2 = 0.0f;
	if (fabsf(b1) + fabsf(b2) + fabsf(c1) + fabsf(c2) < 1e-12f) b1 = b2 = c1 = c2 = 0.0f;
	e.gr1 = {b1, b2};
	e.gr2 = {c1, c2};
}

// ---------------------------------------------------------------------------------------------
// Live parameters

struct ParamEntry {
	strisy::ParamDesc d;
	real Params::*m;
};

static constexpr ParamEntry param_table[] = {
    {{"level", "register", 0.0f, 4.0f, strisy::ParamKind::continuous}, &Params::level},
    {{"r16", "register", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::r16},      // 16' bassoon
    {{"r8", "register", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::r8a},       // 8' clarinet
    {{"r8_sharp", "register", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::r8b}, // 8' sharp (violin, musette)
    {{"r8_flat", "register", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::r8c},  // 8' flat (musette)
    {{"r4", "register", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::r4},        // 4' piccolo
    {{"beat", "musette", 0.0f, 12.0f, strisy::ParamKind::continuous}, &Params::beat},    // Hz at A4
    {{"beat_flat", "musette", 0.0f, 2.0f, strisy::ParamKind::continuous}, &Params::beat_flat},
    {{"beat_track", "musette", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::beat_track}, // 0 Hz .. 1 cents
    {{"cassotto", "register", 0.0f, 1.0f, strisy::ParamKind::toggle}, &Params::cassotto},
    {{"bellows", "bellows", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::bellows}, // 0 per note .. 1 one bellows
    {{"bel_ms", "bellows", 5.0f, 300.0f, strisy::ParamKind::continuous}, &Params::bel_ms},
    {{"valve", "bellows", 0.05f, 1.0f, strisy::ParamKind::continuous}, &Params::valve}, // pressure for a full pallet
    {{"throttle", "bellows", 0.05f, 1.5f, strisy::ParamKind::continuous}, &Params::throttle},
    {{"chamber_ms", "bellows", 1.0f, 100.0f, strisy::ParamKind::continuous}, &Params::chamber_ms},
    {{"release_ms", "bellows", 3.0f, 200.0f, strisy::ParamKind::continuous}, &Params::release_ms},
    {{"speak_ms", "reed", 5.0f, 300.0f, strisy::ParamKind::continuous}, &Params::speak_ms},
    {{"low_slow", "reed", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::low_slow},
    {{"thresh", "reed", 0.005f, 0.3f, strisy::ParamKind::continuous}, &Params::thresh},
    {{"amp", "reed", 0.5f, 5.0f, strisy::ParamKind::continuous}, &Params::amp},
    {{"rest", "reed", 0.0f, 2.0f, strisy::ParamKind::continuous}, &Params::rest},
    {{"push", "reed", 0.0f, 1.2f, strisy::ParamKind::continuous}, &Params::push},
    {{"gap", "reed", 0.02f, 0.6f, strisy::ParamKind::continuous}, &Params::gap},
    {{"exit", "reed", 0.05f, 1.5f, strisy::ParamKind::continuous}, &Params::exit},
    {{"down", "reed", 0.0f, 2.5f, strisy::ParamKind::continuous}, &Params::down},
    {{"constrict", "reed", 0.3f, 4.0f, strisy::ParamKind::continuous}, &Params::constrict},
    {{"pump", "reed", 0.0f, 1.5f, strisy::ParamKind::continuous}, &Params::pump},
    {{"flat", "reed", 0.0f, 30.0f, strisy::ParamKind::continuous}, &Params::flat},    // cents at full pressure
    {{"bend", "reed", 0.0f, 100.0f, strisy::ParamKind::continuous}, &Params::bend},   // pallet bend, cents
    {{"spread", "reed", 0.0f, 3.0f, strisy::ParamKind::continuous}, &Params::spread}, // tuning tolerance, cents
    {{"ch_hz", "tone", 500.0f, 5000.0f, strisy::ParamKind::continuous}, &Params::ch_hz},
    {{"ch_db", "tone", 0.0f, 20.0f, strisy::ParamKind::continuous}, &Params::ch_db},
    {{"ch_q", "tone", 0.4f, 5.0f, strisy::ParamKind::continuous}, &Params::ch_q},
    {{"cas_hz", "tone", 400.0f, 4000.0f, strisy::ParamKind::continuous}, &Params::cas_hz},
    {{"cas_leak", "tone", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::cas_leak},
    {{"grille_hz", "tone", 2000.0f, 12000.0f, strisy::ParamKind::continuous}, &Params::grille_hz},
    {{"tilt", "expr", 0.0f, 1.0f, strisy::ParamKind::continuous}, &Params::tilt},
    {{"rev_pedal", "expr", 0.0f, 1.0f, strisy::ParamKind::toggle}, &Params::rev_pedal},
    {{"rev_shake", "expr", 0.0f, 1.0f, strisy::ParamKind::toggle}, &Params::rev_shake},
    {{"rev_ms", "expr", 20.0f, 300.0f, strisy::ParamKind::continuous}, &Params::rev_ms},
};
static constexpr int NUM_LIVE_PARAMS = sizeof(param_table) / sizeof(param_table[0]);
static_assert(NUM_LIVE_PARAMS == NPARAMS, "every Params field is exposed once");

} // namespace accordion

namespace strisy {
template <> struct EngineParams<accordion::Engine> {
	static constexpr int count = accordion::NUM_LIVE_PARAMS;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &accordion::param_table[i].d : nullptr; }
	static float* ptr(accordion::Engine& e, int i) { return (i >= 0 && i < count) ? &(e.p.*(accordion::param_table[i].m)) : nullptr; }
	static void changed(accordion::Engine&, int) {} // the DSP follows e.p through its smoothed copy
};
} // namespace strisy

#endif // VOICE_ACCORDION_H_
