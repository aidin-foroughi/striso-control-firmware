/**
 * trumpet: a physical model of the trumpet family (Bb, C, piccolo, cornet; mutes), built for
 * realism first. Build with `VOICE=trumpet`; presets with set_preset().
 *
 * The model (lips, bore, bell, player) is the shared brass core, brass_core.h; this header is the
 * trumpet family's data: bores (trumpet_bore.h), lip calibration (trumpet_cal.h), warm-start seeds
 * (trumpet_seed.h), presets and parameters.
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
 *   strike      tonguing: hard = "t" (fast, overshoot, tongue noise), soft = "d"; a tongued note from
 *               silence starts in the horn's own steady oscillation for that note and breath (warm
 *               start, trumpet_seed.h; the "warm" parameter: seed amplitude, 0 = the natural start)
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
 * instruction); a warm start costs about one player block (~34k, instead of that player's block; at
 * most one per block, the others wait a block) plus ~2k in each of the next ~15 blocks (the
 * cylinder lines are seeded ahead of their reads). Memory: cylinder lines 32 KB + room 4 KB in ram1,
 * Engine ~20 KB in DTCM, seed table (trumpet_seed.h) ~170 KB of flash.
 */
#ifndef VOICE_TRUMPET_H_
#define VOICE_TRUMPET_H_

#if defined(TRUMPET_DEBUG) && !defined(BRASS_DEBUG)
#define BRASS_DEBUG
#endif
#include "brass_core.h"
#include "trumpet_bore.h"
#include "trumpet_cal.h"
#include "trumpet_seed.h"

namespace trumpet {

using real = float;
static constexpr int BLOCK = brasspm::BLOCK;
static constexpr int MAX_VOICES = brasspm::MAX_VOICES; // key voices (controls)
static constexpr int NP = 4;                           // players: physical trumpets (CPU: ~33k instructions per block each)
static constexpr int CLEN = 1024;                      // cylinder delay lines (one way, 96 kHz)

// cylinder delay lines: AHB SRAM1/2 (ram1), 32 KB; early reflections 4 KB
static float cyl_f_buf[NP][CLEN] __attribute__((aligned(32), section(".ram1_clear")));
static float cyl_b_buf[NP][CLEN] __attribute__((aligned(32), section(".ram1_clear")));
static float room_buf_[brasspm::ROOML] __attribute__((aligned(32), section(".ram1_clear")));

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
	real warm;      // warm start: a tongued note from silence starts in its steady oscillation (seed amplitude, 0: off)
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

using brasspm::MUTE_CUP;
using brasspm::MUTE_HARMON;
using brasspm::MUTE_NONE;
using brasspm::MUTE_PLUNGER;
using brasspm::MUTE_STRAIGHT;
using Preset = brasspm::Preset<Params>;

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
	p.warm = 1.0f;
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
// the trumpet family for the shared core (brass_core.h)

struct Family {
	using Params = trumpet::Params;
	using Bore = bore::Bore;
	using Cal = cal::Cal;
	using SeedState = seed::State;
	using SeedSet = seed::Set;
	static constexpr bool HAS_SLIDE = false;
	static constexpr int NP = trumpet::NP;
	static constexpr int CLEN = trumpet::CLEN;
	static constexpr uint32_t CMASK = CLEN - 1;
	static constexpr real DMAXC = (real)(CLEN - 4);
	static constexpr int H12L = 128; // 12 kHz history (double buffered)
	static constexpr int FS = bore::FS, DEC = bore::DEC, NDEC = bore::NDEC, NFIR = bore::NFIR, NK = bore::NK;
	static constexpr int NCOMBO = bore::NCOMBO, MAXSEC = bore::MAXSEC;
	static constexpr const float (&HDEC)[NDEC] = bore::HDEC;
	static constexpr const float (&HINT)[NDEC] = bore::HINT;
	static constexpr const Bore (&BORES)[bore::NBORE] = bore::BORES;
	static constexpr int NCAL = cal::NCAL;
	static constexpr const Cal (&CAL)[trumpet::NUM_PRESETS] = cal::CAL;
	static constexpr int NH = seed::NH, NDYN = seed::NDYN, NSIG = seed::NSIG;
	static constexpr const float (&DYN)[NDYN] = seed::DYN;
	static constexpr const float (&COS64)[64] = seed::COS64;
	static constexpr const SeedSet (&SEEDS)[trumpet::NUM_PRESETS] = seed::SETS;
	static constexpr float (&cyl_f)[NP][CLEN] = cyl_f_buf;
	static constexpr float (&cyl_b)[NP][CLEN] = cyl_b_buf;
	static constexpr float (&room_buf)[brasspm::ROOML] = room_buf_;
	static constexpr int NPARAMS = trumpet::NPARAMS;
	static constexpr int NUM_PRESETS = trumpet::NUM_PRESETS;
	static constexpr int DEFAULT_PRESET = trumpet::DEFAULT_PRESET;
	static constexpr const real (&PRESET_GAIN)[trumpet::NUM_PRESETS] = trumpet::PRESET_GAIN;
	[[nodiscard]] static Preset preset_def(int i) { return trumpet::preset_def(i); }
#ifdef TRUMPET_SPUMP
	static constexpr real SPUMP = TRUMPET_SPUMP;
#else
	static constexpr real SPUMP = 0.6f;
#endif
	// the lips' second (inward striking) mode: its response to the pressure difference re the outward
	// mode's, and its tuning above the outward mode (cents); see the LIPS comment in process_player
	static constexpr real LIP_IN_R = 1.0f, LIP_IN_C = 150.0f;
	static constexpr real SEED_LIP = 1.0f; // tongued starts: the lips released already swinging (x their settling amplitude)
	                                       // warm start (trumpet_seed.h): the breath a tongued note is seeded for (key pressure units:
	                                       // WARM_D0 + WARM_D1 x strike, or the pressure if more; a seed below the breath grows, one above it
	                                       // sounds as an accent), the floor that breath holds (x WARM_BF_K, decaying over WARM_BF_T), the
	                                       // radiated fade-in time constant (s, per stage: hard .. soft strike), the overshoot's rise
#ifndef TRUMPET_WARM_D0
	static constexpr real WARM_D0 = 0.2f, WARM_D1 = 0.5f, WARM_BF_K = 0.7f, WARM_BF_T = 0.03f;
#else
	static constexpr real WARM_D0 = TRUMPET_WARM_D0, WARM_D1 = TRUMPET_WARM_D1, WARM_BF_K = TRUMPET_WARM_BFK, WARM_BF_T = TRUMPET_WARM_BFT;
#endif
	static constexpr real WARM_FADE_HARD = 0.001f, WARM_FADE_SOFT = 0.004f, WARM_OV_T = 0.004f;
	static constexpr int WARM_PER_BLOCK = 1;
	static constexpr real WARM_TAIL_GATE = 0.15f; // a released player this quiet can be warm started
	// pitch tracker: DC removal of the lip motion (per internal sample), period consistency
	static constexpr real TRK_HP = 0.004f, TRK_TOL = 0.05f;
	static constexpr brasspm::Attack ATK_DEF = {0.0f, 0.0f, 0.012f, 0.787f, 0.00531f, 0.0186f, 0.273f, 0.00655f, 0.0147f, 4.52f, 0.623f, 0.04f, 0.582f, 0.173f, 0.0165f, 0.268f, 0.78f, 0.0197f, 0.0f, 0.02f, 4.0f, 0.743f};
	static constexpr brasspm::WarmAtk WATK_DEF = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.01f};
	// output EQ fitted to the Iowa MIS Bb trumpet recordings (cmpref.py)
#ifndef TRUMPET_FIT
	static constexpr real FIT_HP_HZ = 450.0f, FIT_LO = 0.12f, FIT_PK_HZ = 1250.0f, FIT_PK_Q = 0.8f, FIT_PK_G = 1.6f;
#else
	static constexpr real FIT_HP_HZ = TRUMPET_FIT_HP, FIT_LO = TRUMPET_FIT_LO, FIT_PK_HZ = TRUMPET_FIT_PK, FIT_PK_Q = TRUMPET_FIT_Q, FIT_PK_G = TRUMPET_FIT_G;
#endif
	// the range where the notes speak, re the open horn (the piccolo's is narrower; the cornet's top a little lower)
	[[nodiscard]] static constexpr real range_lo(int bi) { return bi == 2 ? -0.5f : -3.0f; }
	[[nodiscard]] static constexpr real range_hi(int bi) { return bi == 2 ? 38.5f : bi == 3 ? 40.5f :
		    bi == 1                                                                         ? 43.5f :
		                                                                                      44.5f; }
};

using Engine = brasspm::Engine<Family>;
using Player = brasspm::Player<Family>;
using brasspm::KeyState;

static inline void init(Engine& e, int sample_rate, int nvoices) { brasspm::init(e, sample_rate, nvoices); }
static inline void process(Engine& e, real* __restrict__ output) { brasspm::process(e, output); }
static inline void set_preset(Engine& e, int i) { brasspm::set_preset(e, i); }
static inline void update_derived(Engine& e) { brasspm::update_derived(e); }
[[nodiscard]] static inline int active_voices(const Engine& e) { return brasspm::active_voices(e); }
[[nodiscard]] static inline real fold_note(const Engine& e, real n) { return brasspm::fold_note(e, n); }

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
    {{"warm", "attack", 0.0f, 1.0f, C_}, &Params::warm},    // warm start (seed amplitude; 0: the natural start)
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
