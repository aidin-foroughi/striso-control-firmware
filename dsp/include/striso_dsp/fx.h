/**
 * Master effects after the voice engines: mono in, stereo out.
 *
 * API, in namespace fx:
 *   typedef float real; BLOCK (= 32)
 *   struct Engine (plain data, lives in DTCM; the big buffers are the host's FastMemory and
 *   LargeMemory, which init() takes over: Engine::fast, Engine::large set before)
 *   void init(Engine&, int sample_rate);
 *   void process(Engine&, const real* in, real* out_l, real* out_r);   // BLOCK samples each
 *   void set_bypass(Engine&, bool);   // all stages fade out (~20 ms) and stop; parameters are kept
 *   live parameters through strisy::EngineParams<fx::Engine> (target 1 of the parameter protocol)
 *
 * Chain (mono -> stereo):
 *   1. drive   gentle cubic saturation with a little asymmetry (DC-blocked), and a tilt EQ
 *   2. phaser  classic 4-stage allpass phaser with feedback, exponential sine sweep (mono)
 *   3. chorus  3-tap ensemble (taps 120 degrees apart, panned L / centre / R), slow sine plus an
 *              optional fast "ensemble" shimmer, cubic interpolated, wet path high-passed
 *   4. trem    stereo tremolo / auto-pan like a Rhodes Suitcase (stereo = phase offset L/R)
 *   5. delay   ping-pong echo (one line, taps at T and 2T), filtered feedback with a soft limit,
 *              time changes glide (small) or crossfade (large)
 *   6. reverb  Dattorro plate (size 0.87): pre-delay, low cut, 4 input diffusers, figure-eight
 *              tank whose allpasses and long delays are modulated by two slow LFOs (smooth tail,
 *              persistent modes within ~1.5 dB of ideal diffuse noise), damping, RT60 decay
 *   out        stereo width (M/S), level, look-ahead soft-knee limiter (knee 3.6, peaks <= 4.4,
 *              16 samples = 0.33 ms latency for the whole output), final clamp at 4.5
 * Delay and reverb run at half rate (24414 Hz) between 6-coefficient polyphase IIR half-band
 * filters (passband to 10.2 kHz, 74 dB image/alias rejection): half the CPU and memory.
 * The delay feeds the reverb (echoes are reverberated), both are added to the dry stereo signal.
 *
 * Every stage has a mix/depth parameter; at 0 the stage fades out and is skipped entirely (its
 * delay memory is then cleared in the background, so turning it on again never replays old audio).
 * A (re)started chorus/delay/reverb fades its input in, so its wet path never starts with a jump.
 * Parameter writes are clamped and smoothed per block (~20 ms) and ramped per sample; delay times
 * glide with a slew limit or crossfade, so writes at any rate never click, blow up or produce NaN.
 * All feedback paths are passive (allpasses, one-pole filters, gains < 1) or soft limited; a
 * per-block guard resets everything if a non-finite value ever appears.
 * Defaults: chorus 0.25 (a touch of width) and a light plate (mix 0.15, 2 s); unity level.
 *
 * Cost (Cortex-M7, estimated from the -O3 disassembly, ~1.2 cycles per instruction): all stages on
 * ~28k cycles per block (~11% CPU), defaults ~19k (~7%), bypassed ~2k. Tested on the host with
 * tools/harness/fx_suite.sh.
 * Memory: Engine 5.6 KB (DTCM, includes the chorus line); FastMemory: delay line 64 KB (int16) +
 * pre-delay 16 KB (the board: AHB SRAM1/2); LargeMemory: reverb ring 64 KB (the board: AXI SRAM).
 * No libm, no per-sample divisions or transcendentals: LFOs are polynomials evaluated per block or
 * per 8 samples and interpolated.
 */
#ifndef FX_H_
#define FX_H_

#include <stdint.h>
#include <math.h>
#include <type_traits>
#include "controls.h"
#include "dspmath.h"
#include "params.h"

#define FX_NOINLINE __attribute__((noinline))

namespace fx {

using real = float;
static constexpr int BLOCK = strisy::BLOCK;
static constexpr int HB = BLOCK / 2; // half-rate samples per block
static constexpr int LSEG = 16;      // limiter segment = look-ahead
static constexpr int PH_CTRL = 8;    // phaser coefficient update interval

using strisy::rmax;
using strisy::rmin;

// ---------------------------------------------------------------------------------------------
// Buffers

static constexpr int CH_LEN = 1024, CH_MASK = CH_LEN - 1;  // chorus, full rate (in Engine)
static constexpr int DL_LEN = 32768, DL_MASK = DL_LEN - 1; // delay, half rate, int16
static constexpr int PD_LEN = 4096, PD_MASK = PD_LEN - 1;  // reverb pre-delay, half rate
static constexpr int RV_LEN = 16384, RV_MASK = RV_LEN - 1; // reverb ring, half rate
static constexpr float DL_SCALE = 4096.0f;                 // int16 full scale = +-8

// the host gives the effects their memory (engine contract, engines/fx.h): the delay line and the
// reverb's pre-delay (80 KB; the board: AHB SRAM1/2), the reverb ring (64 KB; the board: AXI SRAM)
struct FastMemory {
	alignas(32) int16_t dl[DL_LEN];
	alignas(32) float pd[PD_LEN];
};
struct LargeMemory {
	alignas(32) float rv[RV_LEN];
};

// ---------------------------------------------------------------------------------------------
// Parameters

enum {
	P_DRIVE,
	P_TILT,
	P_PH_MIX,
	P_PH_RATE,
	P_PH_DEPTH,
	P_PH_FB,
	P_PH_FREQ,
	P_CH_MIX,
	P_CH_RATE,
	P_CH_DEPTH,
	P_CH_ENS,
	P_TR_DEPTH,
	P_TR_RATE,
	P_TR_STEREO,
	P_DL_MIX,
	P_DL_TIME,
	P_DL_FB,
	P_DL_TONE,
	P_DL_PP,
	P_RV_MIX,
	P_RV_PRE,
	P_RV_DECAY,
	P_RV_DAMP,
	P_RV_LOWCUT,
	P_RV_MOD,
	P_LEVEL,
	P_WIDTH,
	P_BYPASS,
	NPARAM
};

struct ParamInfo {
	strisy::ParamDesc d;
	float def;      // default
	float off;      // value forced while bypassed (NO_OFF: keep)
	uint8_t smooth; // 1: smoothed per block, 0: used directly (rates, times, switches)
};
static constexpr float NO_OFF = -1e9f;

static constexpr ParamInfo PARAMS[NPARAM] = {
    {{"drive", "drive", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.0f, 0.0f, 1},            // saturation amount
    {{"tilt", "drive", -1.0f, 1.0f, strisy::ParamKind::continuous}, 0.0f, 0.0f, 1},            // tilt EQ around 700 Hz, +-6 dB
    {{"phmix", "phaser", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.0f, 0.0f, 1},           // 1 = classic 50/50 (deepest notches)
    {{"phrate", "phaser", 0.05f, 8.0f, strisy::ParamKind::continuous}, 0.5f, NO_OFF, 0},       // Hz
    {{"phdepth", "phaser", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.7f, NO_OFF, 1},       // sweep +-1.5 octaves at 1
    {{"phfb", "phaser", 0.0f, 0.85f, strisy::ParamKind::continuous}, 0.3f, NO_OFF, 1},         // feedback (resonance)
    {{"phfreq", "phaser", 100.0f, 4000.0f, strisy::ParamKind::continuous}, 600.0f, NO_OFF, 1}, // sweep centre, Hz
    {{"chmix", "chorus", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.25f, 0.0f, 1},          // wet amount
    {{"chrate", "chorus", 0.05f, 5.0f, strisy::ParamKind::continuous}, 0.3f, NO_OFF, 0},       // Hz
    {{"chdepth", "chorus", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.25f, NO_OFF, 1},      // 0..3 ms modulation
    {{"chens", "chorus", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.0f, NO_OFF, 1},         // fast 6 Hz ensemble shimmer
    {{"trdepth", "trem", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.0f, 0.0f, 1},
    {{"trrate", "trem", 0.5f, 12.0f, strisy::ParamKind::continuous}, 4.5f, NO_OFF, 0},         // Hz
    {{"trstereo", "trem", 0.0f, 1.0f, strisy::ParamKind::continuous}, 1.0f, NO_OFF, 1},        // 0 tremolo, 1 auto-pan
    {{"dlmix", "delay", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.0f, 0.0f, 1},            // echo level
    {{"dltime", "delay", 20.0f, 650.0f, strisy::ParamKind::continuous}, 330.0f, NO_OFF, 0},    // ms per repeat
    {{"dlfb", "delay", 0.0f, 0.95f, strisy::ParamKind::continuous}, 0.35f, NO_OFF, 1},         // feedback per repeat
    {{"dltone", "delay", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.5f, NO_OFF, 1},         // feedback low-pass 1..12 kHz
    {{"dlpp", "delay", 0.0f, 1.0f, strisy::ParamKind::continuous}, 1.0f, NO_OFF, 1},           // 0 mono echo, 1 ping-pong
    {{"rvmix", "reverb", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.15f, 0.0f, 1},          // reverb level
    {{"rvpre", "reverb", 0.0f, 150.0f, strisy::ParamKind::continuous}, 12.0f, NO_OFF, 0},      // pre-delay, ms
    {{"rvdecay", "reverb", 0.2f, 15.0f, strisy::ParamKind::continuous}, 2.0f, NO_OFF, 1},      // RT60, s
    {{"rvdamp", "reverb", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.45f, NO_OFF, 1},       // tank damping 12 kHz..375 Hz
    {{"rvlowcut", "reverb", 20.0f, 600.0f, strisy::ParamKind::continuous}, 150.0f, NO_OFF, 1}, // input low cut, Hz
    {{"rvmod", "reverb", 0.0f, 1.0f, strisy::ParamKind::continuous}, 0.5f, NO_OFF, 1},         // tank modulation
    {{"level", "out", 0.0f, 2.0f, strisy::ParamKind::continuous}, 1.0f, 1.0f, 1},              // output gain
    {{"width", "out", 0.0f, 2.0f, strisy::ParamKind::continuous}, 1.0f, 1.0f, 1},              // stereo width
    {{"bypass", "out", 0.0f, 1.0f, strisy::ParamKind::toggle}, 0.0f, NO_OFF, 0},
};

// ---------------------------------------------------------------------------------------------
// Math helpers (no libm)

[[nodiscard]] static inline float clampf(float x, float lo, float hi) { return rmin(rmax(x, lo), hi); } // NaN -> lo

[[nodiscard]] static inline float floorf_(float x) {
	float t = (float)(int)x;
	return t > x ? t - 1.0f : t;
}

// 2^x, |x| < 100, relative error ~1e-5
[[nodiscard]] static inline float exp2_(float x) {
	x = clampf(x, -100.0f, 100.0f);
	const float fl = floorf_(x);
	const float f = x - fl;
	const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
	union {
		float f;
		int32_t i;
	} u;
	u.i = ((int32_t)fl + 127) << 23;
	return p * u.f;
}

// sin(2 pi x) for any |x| < 2^22, error < 4e-6
[[nodiscard]] static inline float sin2pi(float x) {
	x -= floorf_(x + 0.5f); // [-0.5, 0.5)
	const float ax = fabsf(x);
	float u = ax > 0.25f ? 0.5f - ax : ax; // sin(pi - a) = sin(a)
	u = x < 0.0f ? -u : u;
	const float u2 = u * u;
	return u * (6.2831853f + u2 * (-41.341702f + u2 * (81.605249f + u2 * (-76.705860f + u2 * 42.058694f))));
}

// sqrt(x) for 0 <= x < 1e18 (control rate): bit-level estimate + 3 Newton steps, rel. error < 1e-6
[[nodiscard]] static inline float sqrt_(float x) {
	x = rmax(x, 0.0f);
	union {
		float f;
		int32_t i;
	} u;
	u.f = x;
	u.i = 0x1fbd1df5 + (u.i >> 1);
	float y = u.f;
	for (int k = 0; k < 3; k++) y = 0.5f * (y + x / rmax(y, 1e-30f));
	return x > 0.0f ? y : 0.0f;
}

// one-pole low-pass coefficient for cutoff fc at rate sr: 1 - exp(-2 pi fc / sr)
[[nodiscard]] static inline float onepole(float fc, float sr) {
	return 1.0f - exp2_(-9.0647202f * fc / sr); // 2 pi / ln 2
}

// 4-point cubic Hermite interpolation between y1 (f = 0) and y2 (f = 1)
[[nodiscard]] static inline float hermite(float y0, float y1, float y2, float y3, float f) {
	const float c1 = 0.5f * (y2 - y0);
	const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
	const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
	return ((c3 * f + c2) * f + c1) * f + y1;
}

// linear below K, smooth quadratic knee above, flat at K + C (C1 continuous, branch free)
[[nodiscard]] static inline float softlimit(float x, float K, float C) {
	const float xp = rmax(x - K, 0.0f), xn = rmin(x + K, 0.0f);
	const float up = rmin(xp, 2.0f * C), un = rmax(xn, -2.0f * C);
	const float q = 0.25f / C;
	return (x - xp - xn) + up - up * up * q + un + un * un * q;
}

// ---------------------------------------------------------------------------------------------
// Polyphase IIR half-band filters (two allpass paths, 3 first-order sections each, coefficients
// from the HIIR design method for 6 coefficients, transition 0.04): passband 0..10.25 kHz,
// stopband from 14.6 kHz at -74.5 dB.

static constexpr float HB_C[6] = {0.0682040760f, 0.2402703580f, 0.4486762359f, 0.6411223671f, 0.7999975637f, 0.9344822355f};
struct HbState {
	float x[6], y[6];
};

// 2:1 decimation of BLOCK samples into HB samples
static inline void hb_down(HbState& h, const float* __restrict__ in, float* __restrict__ out) {
	float x0 = h.x[0], x1 = h.x[1], x2 = h.x[2], x3 = h.x[3], x4 = h.x[4], x5 = h.x[5];
	float y0 = h.y[0], y1 = h.y[1], y2 = h.y[2], y3 = h.y[3], y4 = h.y[4], y5 = h.y[5];
	for (int j = 0; j < HB; j++) {
		float s0 = in[2 * j + 1], s1 = in[2 * j], t;
		t = x0;
		x0 = s0;
		s0 = (s0 - y0) * HB_C[0] + t;
		y0 = s0;
		t = x1;
		x1 = s1;
		s1 = (s1 - y1) * HB_C[1] + t;
		y1 = s1;
		t = x2;
		x2 = s0;
		s0 = (s0 - y2) * HB_C[2] + t;
		y2 = s0;
		t = x3;
		x3 = s1;
		s1 = (s1 - y3) * HB_C[3] + t;
		y3 = s1;
		t = x4;
		x4 = s0;
		s0 = (s0 - y4) * HB_C[4] + t;
		y4 = s0;
		t = x5;
		x5 = s1;
		s1 = (s1 - y5) * HB_C[5] + t;
		y5 = s1;
		out[j] = 0.5f * (s0 + s1);
	}
	h.x[0] = x0;
	h.x[1] = x1;
	h.x[2] = x2;
	h.x[3] = x3;
	h.x[4] = x4;
	h.x[5] = x5;
	h.y[0] = y0;
	h.y[1] = y1;
	h.y[2] = y2;
	h.y[3] = y3;
	h.y[4] = y4;
	h.y[5] = y5;
}

// 1:2 interpolation of HB samples, added into BLOCK samples
static inline void hb_up_add(HbState& h, const float* __restrict__ in, float* __restrict__ out) {
	float x0 = h.x[0], x1 = h.x[1], x2 = h.x[2], x3 = h.x[3], x4 = h.x[4], x5 = h.x[5];
	float y0 = h.y[0], y1 = h.y[1], y2 = h.y[2], y3 = h.y[3], y4 = h.y[4], y5 = h.y[5];
	for (int j = 0; j < HB; j++) {
		float s0 = in[j], s1 = in[j], t;
		t = x0;
		x0 = s0;
		s0 = (s0 - y0) * HB_C[0] + t;
		y0 = s0;
		t = x1;
		x1 = s1;
		s1 = (s1 - y1) * HB_C[1] + t;
		y1 = s1;
		t = x2;
		x2 = s0;
		s0 = (s0 - y2) * HB_C[2] + t;
		y2 = s0;
		t = x3;
		x3 = s1;
		s1 = (s1 - y3) * HB_C[3] + t;
		y3 = s1;
		t = x4;
		x4 = s0;
		s0 = (s0 - y4) * HB_C[4] + t;
		y4 = s0;
		t = x5;
		x5 = s1;
		s1 = (s1 - y5) * HB_C[5] + t;
		y5 = s1;
		out[2 * j] += s0;
		out[2 * j + 1] += s1;
	}
	h.x[0] = x0;
	h.x[1] = x1;
	h.x[2] = x2;
	h.x[3] = x3;
	h.x[4] = x4;
	h.x[5] = x5;
	h.y[0] = y0;
	h.y[1] = y1;
	h.y[2] = y2;
	h.y[3] = y3;
	h.y[4] = y4;
	h.y[5] = y5;
}

// ---------------------------------------------------------------------------------------------
// Reverb geometry: Dattorro's plate (J. Dattorro, "Effect Design Part 1", JAES 1997), lengths
// given at 29761 Hz, scaled to the half rate and RV_SIZE. All lines share one ring buffer with a
// single decrementing pointer: line k is written at ring[p + off[k]] and read at delay d from
// ring[p + off[k] + d].

static constexpr float RV_SIZE = 0.87f;
static constexpr int RV_EXC = 13;      // max modulation excursion of the tank allpasses (samples)
static constexpr float RV_DEXC = 0.6f; // relative excursion of the long delays
enum { RV_AP1,
	RV_AP2,
	RV_AP3,
	RV_AP4,
	RV_LAP1,
	RV_LD1,
	RV_LAP2,
	RV_LD2,
	RV_RAP1,
	RV_RD1,
	RV_RAP2,
	RV_RD2,
	RV_NLINES };
static constexpr int16_t RV_LEN0[RV_NLINES] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
// output taps: line, position at 29761 Hz, sign (the sign pattern is hardcoded in stage_reverb)
static constexpr int RV_NTAPS = 7;
static constexpr int16_t RV_TAPS_L[RV_NTAPS][3] = {
    {RV_RD1, 266, 1}, {RV_RD1, 2974, 1}, {RV_RAP2, 1913, -1}, {RV_RD2, 1996, 1},
    {RV_LD1, 1990, -1}, {RV_LAP2, 187, -1}, {RV_LD2, 1066, -1}};
static constexpr int16_t RV_TAPS_R[RV_NTAPS][3] = {
    {RV_LD1, 353, 1}, {RV_LD1, 3627, 1}, {RV_LAP2, 1228, -1}, {RV_LD2, 2673, 1},
    {RV_RD1, 2111, -1}, {RV_RAP2, 335, -1}, {RV_RD2, 121, -1}};
// wet gain: Dattorro's 0.6 per tap, scaled so an undamped 2 s decay has an impulse response energy of ~1
static constexpr float RV_OUT = 0.6f * 1.2f;

// the reverb loop's ring accesses (reads, W: writes; M: the modulated Hermite reads)
enum {
	RA_AP0,
	RA_AP0W,
	RA_AP1,
	RA_AP1W,
	RA_AP2,
	RA_AP2W,
	RA_AP3,
	RA_AP3W,
	RA_FROML,
	RA_FROMR,
	RA_LAM,
	RA_LAW,
	RA_LDM,
	RA_LDW,
	RA_LB,
	RA_LBW,
	RA_LEW,
	RA_RAM,
	RA_RAW,
	RA_RDM,
	RA_RDW,
	RA_RB,
	RA_RBW,
	RA_REW,
	RA_TL,
	RA_TR = RA_TL + RV_NTAPS,
	RA_N = RA_TR + RV_NTAPS
};

// ---------------------------------------------------------------------------------------------

struct Engine {
	FastMemory* fast;   // the host's (init)
	LargeMemory* large; // the host's (init)
	float p[NPARAM];       // live parameters (written between blocks)
	float s[NPARAM];       // smoothed parameters (this block)
	float sp[NPARAM];      // smoothed parameters of the previous block
	float sr, sr2, inv_sr; // sample rate, half rate, 1/sr
	float smooth_a;        // per block smoothing coefficient

	// stage state: on = running this block
	int on_drive, on_tilt, on_phaser, on_chorus, on_trem, on_delay, on_reverb, on_half;
	int clr_dl, clr_pd, clr_rv; // background clear cursor (-1: clean)
	uint32_t guard_trips;

	float M[BLOCK];                         // mono working buffer
	float L[LSEG + BLOCK], R[LSEG + BLOCK]; // stereo working buffer behind the limiter look-ahead

	// drive / tilt
	float dr_k, dr_mk, dr_b, dr_yb, dr_w; // block-end values (ramped from these)
	float dr_dcx, dr_dcy, dr_dc_r;
	float tl_lp, tl_c, tl_gl, tl_gh;

	// phaser
	float ph_phase, ph_a, ph_y, ph_s[4];

	// chorus
	uint32_t ch_w;
	float ch_ps, ch_pf, ch_d[3], ch_hp, ch_hp_c, ch_in;
	float ch_buf[CH_LEN];

	// tremolo
	float tr_phase, tr_gl, tr_gr;

	// half rate
	HbState hb_dn, hb_upl, hb_upr;
	float h[HB], wl[HB], wr[HB];

	// delay
	uint32_t dl_w;
	float dl_T, dl_Told, dl_xf, dl_in, dl_tc;
	float dl_hp, dl_lp, dl_hp_c;
	float dl_a, dl_b;

	// reverb
	uint32_t rv_p, pd_w;
	int rv_off[RV_NLINES], rv_len[RV_NLINES];
	int rv_tl[RV_NTAPS], rv_tr[RV_NTAPS]; // tap ring offsets (off + position)
	float pd_T, rv_lc, rv_dl, rv_dr, rv_in;
	float rv_k[7]; // tank coefficients of the previous block (ramped per sample)
	float rv_ph[2], rv_m[4];
	float rv_exc;
	int rv_ao[RA_N], rv_lo[RA_N], rv_sp[RA_N]; // ring accesses: offset + rlo, rlo, rhi - rlo (rv_accesses)

	// limiter
	float lim_g0, lim_B, lim_rel;
};

static inline void set_bypass(Engine& e, bool on) { e.p[P_BYPASS] = on ? 1.0f : 0.0f; }

// ---------------------------------------------------------------------------------------------
// Stages

static FX_NOINLINE void stage_drive(Engine& e) {
	float* __restrict__ M = e.M;
	const float r = 1.0f / BLOCK;
	if (e.on_drive) {
		const float d = e.s[P_DRIVE];
		const float w1 = rmin(d * 5.0f, 1.0f);   // fade in the saturated path
		const float k1 = 0.2f * exp2_(3.3f * d); // knee (|u| = 1.5) at x = 7.5 .. 0.76
		const float b1 = 0.3f * d;               // asymmetry: even harmonics
		const float yb1 = b1 - 0.14814815f * b1 * b1 * b1;
		const float mk1 = (1.0f + 0.3f * d) / k1; // makeup
		float k = e.dr_k, mk = e.dr_mk, b = e.dr_b, yb = e.dr_yb, w = e.dr_w;
		const float dk = (k1 - k) * r, dmk = (mk1 - mk) * r, db = (b1 - b) * r, dyb = (yb1 - yb) * r, dw = (w1 - w) * r;
		float dcx = e.dr_dcx, dcy = e.dr_dcy;
		const float R = e.dr_dc_r;
		for (int n = 0; n < BLOCK; n++) {
			k += dk;
			mk += dmk;
			b += db;
			yb += dyb;
			w += dw;
			const float x = M[n];
			const float u = clampf(k * x + b, -1.5f, 1.5f);
			const float y = ((u - 0.14814815f * u * u * u) - yb) * mk;
			// DC blocker (8 Hz) on the difference, so fading the stage out leaves the dry signal exact
			const float dlt = y - x;
			dcy = dlt - dcx + R * dcy;
			dcx = dlt;
			M[n] = x + w * dcy;
		}
		e.dr_k = k1;
		e.dr_mk = mk1;
		e.dr_b = b1;
		e.dr_yb = yb1;
		e.dr_w = w1;
		e.dr_dcx = dcx;
		e.dr_dcy = dcy;
	}
	if (e.on_tilt) {
		const float t = e.s[P_TILT];
		const float gl1 = exp2_(-t), gh1 = exp2_(t);
		float gl = e.tl_gl, gh = e.tl_gh, lp = e.tl_lp;
		const float dgl = (gl1 - gl) * r, dgh = (gh1 - gh) * r, c = e.tl_c;
		for (int n = 0; n < BLOCK; n++) {
			gl += dgl;
			gh += dgh;
			const float x = M[n];
			lp += c * (x - lp);
			M[n] = gh * x + (gl - gh) * lp;
		}
		e.tl_gl = gl1;
		e.tl_gh = gh1;
		e.tl_lp = lp;
	}
}

[[nodiscard]] static inline float phaser_coef(const Engine& e, float phase) {
	float f = e.s[P_PH_FREQ] * exp2_(1.5f * e.s[P_PH_DEPTH] * sin2pi(phase));
	f = clampf(f, 20.0f, 7000.0f);
	const float t = strisy::tan0(3.14159265f * f * e.inv_sr);
	return (t - 1.0f) / (t + 1.0f);
}

static FX_NOINLINE void stage_phaser(Engine& e) {
	float* __restrict__ M = e.M;
	// out = mk * ((1 - m/2) x + (m/2) y): m = 1 is the classic 50/50 sum; mk makes up part of the
	// energy the notches remove. The feedback loop's average power gain is 1/(1 - fb^2): gin undoes it.
	const float m0 = e.sp[P_PH_MIX], m1 = e.s[P_PH_MIX];
	const float dm = (m1 - m0) * (1.0f / BLOCK);
	float m = m0;
	const float fb0 = e.sp[P_PH_FB], fb1 = e.s[P_PH_FB];
	const float gin0 = sqrt_(1.0f - fb0 * fb0), gin1 = sqrt_(1.0f - fb1 * fb1);
	float fb = fb0, gin = gin0;
	const float dfb = (fb1 - fb0) * (1.0f / BLOCK), dgin = (gin1 - gin0) * (1.0f / BLOCK);
	const float inc = e.s[P_PH_RATE] * PH_CTRL * e.inv_sr;
	float ph = e.ph_phase, a = e.ph_a, y = e.ph_y;
	float s0 = e.ph_s[0], s1 = e.ph_s[1], s2 = e.ph_s[2], s3 = e.ph_s[3];
	for (int k = 0; k < BLOCK / PH_CTRL; k++) {
		ph += inc;
		ph -= (float)(int)ph;
		const float a1 = phaser_coef(e, ph);
		const float da = (a1 - a) * (1.0f / PH_CTRL);
		for (int j = 0; j < PH_CTRL; j++) {
			a += da;
			m += dm;
			fb += dfb;
			gin += dgin;
			const int n = k * PH_CTRL + j;
			const float x = M[n];
			const float u = gin * x + fb * y;
			float t;
			t = a * u + s0;
			s0 = u - a * t;
			y = a * t + s1;
			s1 = t - a * y;
			t = a * y + s2;
			s2 = y - a * t;
			y = a * t + s3;
			s3 = t - a * y;
			M[n] = (1.0f + 0.35f * m) * (x + 0.5f * m * (y - x));
		}
		a = a1;
	}
	e.ph_phase = ph;
	e.ph_a = a;
	e.ph_y = y;
	e.ph_s[0] = s0;
	e.ph_s[1] = s1;
	e.ph_s[2] = s2;
	e.ph_s[3] = s3;
}

static inline void chorus_delays(const Engine& e, float* d) {
	const float base = 0.010f * e.sr;
	const float as = e.s[P_CH_DEPTH] * 0.003f * e.sr;
	const float af = e.s[P_CH_ENS] * 0.0004f * e.sr;
	for (int k = 0; k < 3; k++)
		d[k] = base + as * sin2pi(e.ch_ps + 0.33333333f * k) + af * sin2pi(e.ch_pf + 0.33333333f * k);
}

// a (re)started chorus, delay or reverb fades its input in over 32 blocks (21 ms)
static constexpr float FADE_STEP = 1.0f / 32.0f;

static FX_NOINLINE void stage_chorus(Engine& e, float* __restrict__ L, float* __restrict__ R) {
	const float* __restrict__ M = e.M;
	if (!e.on_chorus) {
		for (int n = 0; n < BLOCK; n++) L[n] = R[n] = M[n];
		return;
	}
	float* __restrict__ buf = e.ch_buf;
	const float r = 1.0f / BLOCK;
	e.ch_ps += e.s[P_CH_RATE] * BLOCK * e.inv_sr;
	e.ch_ps -= (float)(int)e.ch_ps;
	e.ch_pf += 6.1f * BLOCK * e.inv_sr;
	e.ch_pf -= (float)(int)e.ch_pf;
	float d1[3];
	chorus_delays(e, d1);
	float da = e.ch_d[0], db = e.ch_d[1], dc = e.ch_d[2];
	const float dda = (d1[0] - da) * r, ddb = (d1[1] - db) * r, ddc = (d1[2] - dc) * r;
	// mix: dry 1 -> 0.8, each side gets one tap plus the centre tap at -3 dB
	const float m0 = e.sp[P_CH_MIX], m1 = e.s[P_CH_MIX];
	float gd = 1.0f - 0.2f * m0, gw = 0.55f * m0;
	const float dgd = -0.2f * (m1 - m0) * r, dgw = 0.55f * (m1 - m0) * r;
	float hp = e.ch_hp;
	const float hc = e.ch_hp_c;
	float fin = e.ch_in;
	const float fin1 = rmin(fin + FADE_STEP, 1.0f), dfin = (fin1 - fin) * r;
	e.ch_in = fin1;
	uint32_t w = e.ch_w;
	// The slots: the write at w & CH_MASK, tap k's Hermite reads at (w - di + 1 .. w - di - 2) &
	// CH_MASK, w rising by one per sample. If none of them wraps around the buffer's end in this
	// block (most blocks), the loop addresses the same slots as plain indices (identical results).
	// A tap's integer delay stays within one of its block's start and end delays (linear ramp).
	const int c0 = (int)(w & CH_MASK);
	int adj[3];
	bool fast = c0 + BLOCK <= CH_LEN;
	{
		const float d0[3] = {da, db, dc};
		for (int k = 0; k < 3; k++) {
			// (also false for a NaN or out of range delay: the masked path then)
			const bool ok = rmin(d0[k], d1[k]) >= 4.0f && rmax(d0[k], d1[k]) <= (float)(CH_LEN - BLOCK - 8);
			const int dlo = ok ? (int)rmin(d0[k], d1[k]) - 1 : 0, dhi = ok ? (int)rmax(d0[k], d1[k]) + 1 : 0;
			const int y = (int)((w - (uint32_t)(dhi + 2)) & CH_MASK); // lowest slot (first sample)
			fast &= ok & (y + (BLOCK - 1) + (dhi - dlo) + 3 < CH_LEN);
			adj[k] = y + dhi + 2 - c0; // fast: slot = c0 + n + adj - di + t
		}
	}
	auto run = [&](auto fast_tag) {
		constexpr bool FAST = decltype(fast_tag)::value;
		for (int n = 0; n < BLOCK; n++) {
			da += dda;
			db += ddb;
			dc += ddc;
			gd += dgd;
			gw += dgw;
			fin += dfin;
			const float x = M[n];
			hp += hc * (x - hp);
			if constexpr (FAST) buf[c0 + n] = fin * (x - hp);
			else buf[w & CH_MASK] = fin * (x - hp);
			float tap[3];
			const float dd[3] = {da, db, dc};
			for (int k = 0; k < 3; k++) {
				const int di = (int)dd[k];
				const float f = dd[k] - (float)di;
				if constexpr (FAST) {
					const float* t = buf + (c0 + n + adj[k] - di);
					tap[k] = hermite(t[1], t[0], t[-1], t[-2], f);
				} else {
					const uint32_t b = w - (uint32_t)di;
					tap[k] = hermite(buf[(b + 1) & CH_MASK], buf[b & CH_MASK], buf[(b - 1) & CH_MASK], buf[(b - 2) & CH_MASK], f);
				}
			}
			const float c = 0.70710678f * tap[1];
			L[n] = gd * x + gw * (tap[0] + c);
			R[n] = gd * x + gw * (tap[2] + c);
			w++;
		}
	};
	if (fast) run(std::true_type{});
	else run(std::false_type{});
	e.ch_w = w;
	e.ch_hp = hp;
	e.ch_d[0] = d1[0];
	e.ch_d[1] = d1[1];
	e.ch_d[2] = d1[2];
}

// gain = sqrt(1 - depth (1 - lfo) / 2): the power follows the LFO, so with stereo = 1 the sum of
// both channels' power is constant (equal power auto-pan) and a tremolo loses less level
static inline void trem_gains(const Engine& e, float& gl, float& gr) {
	const float d = 0.5f * e.s[P_TR_DEPTH];
	gl = sqrt_(1.0f - d * (1.0f - sin2pi(e.tr_phase)));
	gr = sqrt_(1.0f - d * (1.0f - sin2pi(e.tr_phase + 0.5f * e.s[P_TR_STEREO])));
}

static FX_NOINLINE void stage_trem(Engine& e, float* __restrict__ L, float* __restrict__ R) {
	e.tr_phase += e.s[P_TR_RATE] * BLOCK * e.inv_sr;
	e.tr_phase -= (float)(int)e.tr_phase;
	float gl1, gr1;
	trem_gains(e, gl1, gr1);
	float gl = e.tr_gl, gr = e.tr_gr;
	const float dgl = (gl1 - gl) * (1.0f / BLOCK), dgr = (gr1 - gr) * (1.0f / BLOCK);
	for (int n = 0; n < BLOCK; n++) {
		gl += dgl;
		gr += dgr;
		L[n] *= gl;
		R[n] *= gr;
	}
	e.tr_gl = gl1;
	e.tr_gr = gr1;
}

// delay line read at fractional delay d (>= 2) behind write index w (sample at w = newest)
[[nodiscard]] static inline float dl_read(const int16_t* __restrict__ buf, uint32_t w, float d) {
	const int di = (int)d;
	const float f = d - (float)di;
	const uint32_t b = w - (uint32_t)di;
	return hermite((float)buf[(b + 1) & DL_MASK], (float)buf[b & DL_MASK], (float)buf[(b - 1) & DL_MASK], (float)buf[(b - 2) & DL_MASK], f);
}

static constexpr float DL_XF_STEP = 1.0f / 48.0f; // crossfade over 48 blocks (31 ms)

// feedback low-pass coefficient: 1 kHz .. 12 kHz, fully open at 1
[[nodiscard]] static inline float dl_tone_coef(const Engine& e, float tone) {
	return tone >= 0.999f ? 1.0f : onepole(1000.0f * exp2_(3.585f * tone), e.sr2);
}

// echo: input e.h (half rate mono), output added to e.wl / e.wr (already scaled by the mix)
static FX_NOINLINE void stage_delay(Engine& e) {
	int16_t* __restrict__ buf = e.fast->dl;
	const float* __restrict__ h = e.h;
	const float r = 1.0f / HB;
	const float Tmax = 0.5f * DL_LEN - 8.0f;
	const float Tt = clampf(e.s[P_DL_TIME] * 0.001f * e.sr2, 8.0f, Tmax);
	const float m0 = e.sp[P_DL_MIX], m1 = e.s[P_DL_MIX];
	float m = m0 * (1.0f / DL_SCALE);
	const float dm = (m1 - m0) * r * (1.0f / DL_SCALE);
	const float fb0 = e.sp[P_DL_FB], fb1 = e.s[P_DL_FB];
	float fb = fb0, fb2 = fb0 * fb0;
	const float dfb = (fb1 - fb0) * r, dfb2 = (fb1 * fb1 - fb0 * fb0) * r;
	const float tc1 = dl_tone_coef(e, e.s[P_DL_TONE]);
	float tc = e.dl_tc;
	const float dtc = (tc1 - tc) * r;
	e.dl_tc = tc1;
	float fin = e.dl_in * DL_SCALE;
	const float fin1 = rmin(e.dl_in + FADE_STEP, 1.0f), dfin = (fin1 - e.dl_in) * r * DL_SCALE;
	e.dl_in = fin1;
	// ping-pong pan: a, b = sin, cos of (1 + pp) * pi / 4 (equal power; pp = 0: both -3 dB)
	const float th = 0.125f * (1.0f + e.s[P_DL_PP]);
	const float a1 = sin2pi(th), b1 = sin2pi(0.25f - th);
	float a = e.dl_a, b = e.dl_b;
	const float dA = (a1 - a) * r, dB = (b1 - b) * r;
	e.dl_a = a1;
	e.dl_b = b1;
	float hp = e.dl_hp, lp = e.dl_lp;
	const float hc = e.dl_hp_c;
	uint32_t w = e.dl_w;
	float* __restrict__ wl = e.wl;
	float* __restrict__ wr = e.wr;
	if (e.dl_xf >= 1.0f && fabsf(Tt - e.dl_T) > 256.0f) {
		// big jump: crossfade from the old read position to the new one
		e.dl_Told = e.dl_T;
		e.dl_T = Tt;
		e.dl_xf = 0.0f;
	}
	if (e.dl_xf < 1.0f) {
		const float x0 = e.dl_xf, x1 = rmin(e.dl_xf + DL_XF_STEP, 1.0f);
		float x = x0;
		const float dx = (x1 - x0) * r;
		const float Tn = e.dl_T, To = e.dl_Told;
		for (int j = 0; j < HB; j++) {
			x += dx;
			m += dm;
			a += dA;
			b += dB;
			fb += dfb;
			fb2 += dfb2;
			tc += dtc;
			fin += dfin;
			const float ao = dl_read(buf, w, To), bo = dl_read(buf, w, 2.0f * To);
			const float an = dl_read(buf, w, Tn), bn = dl_read(buf, w, 2.0f * Tn);
			const float tA = ao + x * (an - ao), tB = bo + x * (bn - bo);
			hp += hc * (tB - hp);
			lp += tc * ((tB - hp) - lp);
			const float in = softlimit(h[j] * fin + fb2 * lp, 4.0f * DL_SCALE, 3.5f * DL_SCALE);
			buf[(w + 1) & DL_MASK] = (int16_t)(int32_t)in;
			w++;
			const float tBf = fb * tB;
			wl[j] += m * (a * tA + b * tBf);
			wr[j] += m * (b * tA + a * tBf);
		}
		e.dl_xf = x1;
	} else {
		// small changes glide (slew limited to 0.375 samples per sample: tape-like pitch bend)
		const float T0 = e.dl_T;
		const float T1 = fabsf(Tt - T0) < 0.01f ? Tt : T0 + clampf((Tt - T0) * 0.15f, -6.0f, 6.0f);
		float T = T0;
		const float dT = (T1 - T0) * r;
		for (int j = 0; j < HB; j++) {
			T += dT;
			m += dm;
			a += dA;
			b += dB;
			fb += dfb;
			fb2 += dfb2;
			tc += dtc;
			fin += dfin;
			const float tA = dl_read(buf, w, T), tB = dl_read(buf, w, 2.0f * T);
			hp += hc * (tB - hp);
			lp += tc * ((tB - hp) - lp);
			const float in = softlimit(h[j] * fin + fb2 * lp, 4.0f * DL_SCALE, 3.5f * DL_SCALE);
			buf[(w + 1) & DL_MASK] = (int16_t)(int32_t)in;
			w++;
			const float tBf = fb * tB;
			wl[j] += m * (a * tA + b * tBf);
			wr[j] += m * (b * tA + a * tBf);
		}
		e.dl_T = T1;
	}
	e.dl_hp = hp;
	e.dl_lp = lp;
	e.dl_w = w;
}

// decay: gain per sample for the RT60, applied per tank segment; decay diffusion 2 as Dattorro;
// damping; input low cut
static inline void rv_coefs(const Engine& e, float* k) {
	const float sr2 = e.sr2;
	const int* len = e.rv_len;
	const float lg = -9.9657843f / (e.s[P_RV_DECAY] * sr2); // log2 of the gain per sample
	k[0] = exp2_(lg * (float)(len[RV_LAP1] + len[RV_LD1]));
	k[1] = exp2_(lg * (float)(len[RV_LAP2] + len[RV_LD2]));
	k[2] = exp2_(lg * (float)(len[RV_RAP1] + len[RV_RD1]));
	k[3] = exp2_(lg * (float)(len[RV_RAP2] + len[RV_RD2]));
	k[4] = clampf(0.5f * (k[0] + k[2]) + 0.15f, 0.25f, 0.5f);
	k[5] = exp2_(-9.0647202f * 12000.0f * exp2_(-5.0f * e.s[P_RV_DAMP]) / sr2);
	k[6] = onepole(e.s[P_RV_LOWCUT], sr2);
}

// the reverb loop's ring accesses (stage_reverb) from the geometry
static inline void rv_accesses(Engine& e) {
	int off[RA_N], rlo[RA_N], rhi[RA_N];
	const int* len = e.rv_len;
	off[RA_AP0] = e.rv_off[RV_AP1] + len[RV_AP1];
	off[RA_AP0W] = e.rv_off[RV_AP1];
	off[RA_AP1] = e.rv_off[RV_AP2] + len[RV_AP2];
	off[RA_AP1W] = e.rv_off[RV_AP2];
	off[RA_AP2] = e.rv_off[RV_AP3] + len[RV_AP3];
	off[RA_AP2W] = e.rv_off[RV_AP3];
	off[RA_AP3] = e.rv_off[RV_AP4] + len[RV_AP4];
	off[RA_AP3W] = e.rv_off[RV_AP4];
	off[RA_FROML] = e.rv_off[RV_LD2] + len[RV_LD2];
	off[RA_FROMR] = e.rv_off[RV_RD2] + len[RV_RD2];
	off[RA_LAM] = off[RA_LAW] = e.rv_off[RV_LAP1];
	off[RA_LDM] = off[RA_LDW] = e.rv_off[RV_LD1];
	off[RA_LB] = e.rv_off[RV_LAP2] + len[RV_LAP2];
	off[RA_LBW] = e.rv_off[RV_LAP2];
	off[RA_LEW] = e.rv_off[RV_LD2];
	off[RA_RAM] = off[RA_RAW] = e.rv_off[RV_RAP1];
	off[RA_RDM] = off[RA_RDW] = e.rv_off[RV_RD1];
	off[RA_RB] = e.rv_off[RV_RAP2] + len[RV_RAP2];
	off[RA_RBW] = e.rv_off[RV_RAP2];
	off[RA_REW] = e.rv_off[RV_RD2];
	for (int k = 0; k < RV_NTAPS; k++) {
		off[RA_TL + k] = e.rv_tl[k];
		off[RA_TR + k] = e.rv_tr[k];
	}
	for (int k = 0; k < RA_N; k++) rlo[k] = rhi[k] = 0;
	// modulated reads: Hermite taps -1 .. +2 around the integer delay, which stays within
	// RV_EXC (+ rounding) of the line length (as the ring layout's extra room assumes)
	{
		const int mods[4] = {RA_LAM, RA_LDM, RA_RAM, RA_RDM}, lines[4] = {RV_LAP1, RV_LD1, RV_RAP1, RV_RD1};
		for (int i = 0; i < 4; i++) {
			rlo[mods[i]] = len[lines[i]] - RV_EXC - 4;
			rhi[mods[i]] = len[lines[i]] + RV_EXC + 4;
		}
	}
	for (int k = 0; k < RA_N; k++) {
		e.rv_ao[k] = off[k] + rlo[k];
		e.rv_lo[k] = rlo[k];
		e.rv_sp[k] = rhi[k] - rlo[k];
	}
}

// reverb: input e.h + echo, output added to e.wl / e.wr (scaled by the mix)
static FX_NOINLINE void stage_reverb(Engine& e) {
	float* __restrict__ ring = e.large->rv;
	float* __restrict__ pd = e.fast->pd;
	const float r = 1.0f / HB;
	const float sr2 = e.sr2;
	// pre-delay: glide, slew limited to 0.375 samples per sample
	const float Pt = clampf(e.s[P_RV_PRE] * 0.001f * sr2, 0.0f, (float)(PD_LEN - 4));
	const float P0 = e.pd_T;
	const float P1 = fabsf(Pt - P0) < 0.01f ? Pt : P0 + clampf((Pt - P0) * 0.2f, -6.0f, 6.0f);
	float P = P0;
	const float dP = (P1 - P0) * r;
	e.pd_T = P1;
	// tank coefficients, ramped per sample from the previous block's
	const int* len = e.rv_len;
	float k1[7];
	rv_coefs(e, k1);
	float gL1 = e.rv_k[0], gL2 = e.rv_k[1], gR1 = e.rv_k[2], gR2 = e.rv_k[3], dd2 = e.rv_k[4], damp = e.rv_k[5], lc_c = e.rv_k[6];
	const float dgL1 = (k1[0] - gL1) * r, dgL2 = (k1[1] - gL2) * r, dgR1 = (k1[2] - gR1) * r, dgR2 = (k1[3] - gR2) * r;
	const float ddd2 = (k1[4] - dd2) * r, ddamp = (k1[5] - damp) * r, dlc = (k1[6] - lc_c) * r;
	for (int k = 0; k < 7; k++) e.rv_k[k] = k1[k];
	float fin = e.rv_in;
	const float fin1 = rmin(fin + FADE_STEP, 1.0f), dfin = (fin1 - fin) * r;
	e.rv_in = fin1;
	// modulation LFOs (0.62 and 0.77 Hz), interpolated over the block
	e.rv_ph[0] += 0.62f * HB / sr2;
	e.rv_ph[0] -= (float)(int)e.rv_ph[0];
	e.rv_ph[1] += 0.77f * HB / sr2;
	e.rv_ph[1] -= (float)(int)e.rv_ph[1];
	const float exc = e.s[P_RV_MOD] * (float)RV_EXC;
	const float mL1 = (float)len[RV_LAP1] + exc * sin2pi(e.rv_ph[0]);
	const float mR1 = (float)len[RV_RAP1] + exc * sin2pi(e.rv_ph[1] + 0.25f);
	// the long delays get a smaller, slower modulation too: smears the remaining tank modes
	const float nL1 = (float)len[RV_LD1] + RV_DEXC * exc * sin2pi(e.rv_ph[1] + 0.6f);
	const float nR1 = (float)len[RV_RD1] + RV_DEXC * exc * sin2pi(e.rv_ph[0] + 0.85f);
	float mL = e.rv_m[0], mR = e.rv_m[1], nL = e.rv_m[2], nR = e.rv_m[3];
	const float dmL = (mL1 - mL) * r, dmR = (mR1 - mR) * r, dnL = (nL1 - nL) * r, dnR = (nR1 - nR) * r;
	e.rv_m[0] = mL1;
	e.rv_m[1] = mR1;
	e.rv_m[2] = nL1;
	e.rv_m[3] = nR1;
	const float g0 = e.sp[P_RV_MIX] * RV_OUT, g1 = e.s[P_RV_MIX] * RV_OUT;
	float g = g0;
	const float dg = (g1 - g0) * r;

	// The ring accesses of the loop, each at (p + offset [+ rel]) & RV_MASK with p decrementing by
	// one per sample (rel: the modulated reads' variable part, within [rlo, rhi] of the access).
	// In most blocks (~97%) no access's slots wrap around the end of the ring: the loop then
	// addresses the same slots as plain indices from the block's first one, without the masks
	// (the same reads and writes in the same order: identical results).
	uint32_t p = e.rv_p, pw = e.pd_w;
	const int q0 = (int)(p & RV_MASK);
	int off[RA_N];
	bool fast = true;
	for (int k = 0; k < RA_N; k++) {
		// lowest slot at the block's start; over the block the slots run down HB - 1 below it
		const int y = (int)((p + (uint32_t)e.rv_ao[k]) & RV_MASK);
		fast &= (y >= HB - 1) & (y + e.rv_sp[k] < RV_LEN);
		off[k] = y - e.rv_lo[k] - q0;
	}
	// (the modulated delays stay within RV_EXC of their line lengths; anything else, NaN included,
	// takes the masked path, which can never index outside the ring)
	{
		const float m = (float)(RV_EXC + 1);
		const float a = (float)len[RV_LAP1], b = (float)len[RV_RAP1], c = (float)len[RV_LD1], d = (float)len[RV_RD1];
		fast &= (fabsf(mL - a) <= m) & (fabsf(mL1 - a) <= m) & (fabsf(mR - b) <= m) & (fabsf(mR1 - b) <= m);
		fast &= (fabsf(nL - c) <= m) & (fabsf(nL1 - c) <= m) & (fabsf(nR - d) <= m) & (fabsf(nR1 - d) <= m);
	}
	if (!fast)
		for (int k = 0; k < RA_N; k++) off[k] = e.rv_ao[k] - e.rv_lo[k];
	float lc = e.rv_lc, dl = e.rv_dl, dr = e.rv_dr;
	const float* __restrict__ h = e.h;
	float* __restrict__ wl = e.wl;
	float* __restrict__ wr = e.wr;
	auto run = [&](auto fast_tag) {
		constexpr bool FAST = decltype(fast_tag)::value;
		int q = q0; // fast: index of p's slot
		// the slot of access k (+ rel) at this sample
		auto at = [&](int k, int rel) -> float& {
			if constexpr (FAST) return ring[q + off[k] + rel];
			else return ring[(p + (uint32_t)(off[k] + rel)) & RV_MASK];
		};
		for (int j = 0; j < HB; j++) {
			P += dP;
			mL += dmL;
			mR += dmR;
			nL += dnL;
			nR += dnR;
			g += dg;
			fin += dfin;
			gL1 += dgL1;
			gL2 += dgL2;
			gR1 += dgR1;
			gR2 += dgR2;
			dd2 += ddd2;
			damp += ddamp;
			lc_c += dlc;
			// input: mono + echo, pre-delay (linear interpolation), low cut
			pd[pw & PD_MASK] = fin * (h[j] + 0.5f * (wl[j] + wr[j]));
			const int pi = (int)P;
			const float pf = P - (float)pi;
			const float p0 = pd[(pw - (uint32_t)pi) & PD_MASK], p1 = pd[(pw - (uint32_t)pi - 1) & PD_MASK];
			pw++;
			float x = p0 + pf * (p1 - p0);
			lc += lc_c * (x - lc);
			x -= lc;
			// input diffusion
			float z, v;
			z = at(RA_AP0, 0);
			v = x - 0.75f * z;
			at(RA_AP0W, 0) = v;
			x = z + 0.75f * v;
			z = at(RA_AP1, 0);
			v = x - 0.75f * z;
			at(RA_AP1W, 0) = v;
			x = z + 0.75f * v;
			z = at(RA_AP2, 0);
			v = x - 0.625f * z;
			at(RA_AP2W, 0) = v;
			x = z + 0.625f * v;
			z = at(RA_AP3, 0);
			v = x - 0.625f * z;
			at(RA_AP3W, 0) = v;
			x = z + 0.625f * v;
			// tank outputs from the previous trip around
			const float fromL = at(RA_FROML, 0), fromR = at(RA_FROMR, 0);
			// left half: modulated allpass, delay, damping, decay, allpass, delay
			{
				float a = x + gR2 * fromR;
				const int di = (int)mL;
				const float f = mL - (float)di;
				z = hermite(at(RA_LAM, di - 1), at(RA_LAM, di), at(RA_LAM, di + 1), at(RA_LAM, di + 2), f);
				v = a + 0.7f * z;
				at(RA_LAW, 0) = v;
				a = z - 0.7f * v;
				at(RA_LDW, 0) = a;
				const int ni = (int)nL;
				const float nf = nL - (float)ni;
				const float c = hermite(at(RA_LDM, ni - 1), at(RA_LDM, ni), at(RA_LDM, ni + 1), at(RA_LDM, ni + 2), nf);
				dl = c + damp * (dl - c);
				a = gL1 * dl;
				z = at(RA_LB, 0);
				v = a - dd2 * z;
				at(RA_LBW, 0) = v;
				a = z + dd2 * v;
				at(RA_LEW, 0) = a;
			}
			// right half
			{
				float a = x + gL2 * fromL;
				const int di = (int)mR;
				const float f = mR - (float)di;
				z = hermite(at(RA_RAM, di - 1), at(RA_RAM, di), at(RA_RAM, di + 1), at(RA_RAM, di + 2), f);
				v = a + 0.7f * z;
				at(RA_RAW, 0) = v;
				a = z - 0.7f * v;
				at(RA_RDW, 0) = a;
				const int ni = (int)nR;
				const float nf = nR - (float)ni;
				const float c = hermite(at(RA_RDM, ni - 1), at(RA_RDM, ni), at(RA_RDM, ni + 1), at(RA_RDM, ni + 2), nf);
				dr = c + damp * (dr - c);
				a = gR1 * dr;
				z = at(RA_RB, 0);
				v = a - dd2 * z;
				at(RA_RBW, 0) = v;
				a = z + dd2 * v;
				at(RA_REW, 0) = a;
			}
			// output taps, signs + + - + - - - (both channels)
			const float yl = (at(RA_TL, 0) + at(RA_TL + 1, 0) + at(RA_TL + 3, 0)) - (at(RA_TL + 2, 0) + at(RA_TL + 4, 0) + at(RA_TL + 5, 0) + at(RA_TL + 6, 0));
			const float yr = (at(RA_TR, 0) + at(RA_TR + 1, 0) + at(RA_TR + 3, 0)) - (at(RA_TR + 2, 0) + at(RA_TR + 4, 0) + at(RA_TR + 5, 0) + at(RA_TR + 6, 0));
			p--;
			q--;
			wl[j] += g * yl;
			wr[j] += g * yr;
		}
	};
	if (fast) run(std::true_type{});
	else run(std::false_type{});
	e.rv_p = p;
	e.pd_w = pw;
	e.rv_lc = lc;
	e.rv_dl = dl;
	e.rv_dr = dr;
}

static FX_NOINLINE void stage_half(Engine& e, float* __restrict__ L, float* __restrict__ R) {
	float mono[BLOCK];
	for (int n = 0; n < BLOCK; n++) mono[n] = 0.5f * (L[n] + R[n]);
	hb_down(e.hb_dn, mono, e.h);
	for (int j = 0; j < HB; j++) e.wl[j] = e.wr[j] = 0.0f;
	if (e.on_delay) stage_delay(e);
	if (e.on_reverb) stage_reverb(e);
	hb_up_add(e.hb_upl, e.wl, L);
	hb_up_add(e.hb_upr, e.wr, R);
}

[[nodiscard]] static inline float limiter_gain(float peak) {
	// soft knee from 3.6, output peak never above 4.4
	const float K = 3.6f, C = 0.8f;
	if (peak <= K) return 1.0f;
	const float u = rmin(peak - K, 2.0f * C);
	return (K + u - u * u * (0.25f / C)) / peak;
}

// width, level, look-ahead limiter; returns false if a non-finite value was found
static FX_NOINLINE bool stage_out(Engine& e, real* __restrict__ out_l, real* __restrict__ out_r) {
	float* __restrict__ L = e.L + LSEG;
	float* __restrict__ R = e.R + LSEG;
	const float r = 1.0f / BLOCK;
	const float w0 = e.sp[P_WIDTH], w1 = e.s[P_WIDTH];
	const float v0 = e.sp[P_LEVEL], v1 = e.s[P_LEVEL];
	if (w0 != 1.0f || w1 != 1.0f || v0 != 1.0f || v1 != 1.0f) {
		// L' = v (M + w S), R' = v (M - w S): gains for L, R
		float w = w0, v = v0;
		const float dw = (w1 - w0) * r, dv = (v1 - v0) * r;
		for (int n = 0; n < BLOCK; n++) {
			w += dw;
			v += dv;
			const float a = 0.5f * v * (1.0f + w), b = 0.5f * v * (1.0f - w);
			const float l = L[n], rr = R[n];
			L[n] = a * l + b * rr;
			R[n] = b * l + a * rr;
		}
	}
	// segment peaks of the new block (two segments)
	float pk[2];
	float sum = 0.0f;
	for (int s = 0; s < 2; s++) {
		float m = 0.0f;
		for (int n = s * LSEG; n < (s + 1) * LSEG; n++) {
			const float al = fabsf(L[n]), ar = fabsf(R[n]);
			sum += al + ar;
			m = rmax(m, rmax(al, ar));
		}
		pk[s] = m;
	}
	if (!(sum < 1e7f)) return false;
	const float g1 = limiter_gain(pk[0]), g2 = limiter_gain(pk[1]);
	const float rel = e.lim_rel;
	const float B0 = e.lim_B;
	const float B1 = rmin(rmin(e.lim_g0, g1), B0 + (1.0f - B0) * rel);
	const float B2 = rmin(rmin(g1, g2), B1 + (1.0f - B1) * rel);
	// output the pending segment (from the previous block) and the first new one
	const float* __restrict__ Lb = e.L;
	const float* __restrict__ Rb = e.R;
	float gn = B0;
	float dgn = (B1 - B0) * (1.0f / LSEG);
	for (int n = 0; n < BLOCK; n++) {
		if (n == LSEG) {
			gn = B1;
			dgn = (B2 - B1) * (1.0f / LSEG);
		}
		gn += dgn;
		out_l[n] = clampf(gn * Lb[n], -4.5f, 4.5f);
		out_r[n] = clampf(gn * Rb[n], -4.5f, 4.5f);
	}
	e.lim_g0 = g2;
	e.lim_B = B2;
	for (int n = 0; n < LSEG; n++) {
		e.L[n] = e.L[BLOCK + n];
		e.R[n] = e.R[BLOCK + n];
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// State management

static inline void clear_buffers_now(Engine& e) {
	for (int i = 0; i < DL_LEN; i++) e.fast->dl[i] = 0;
	for (int i = 0; i < PD_LEN; i++) e.fast->pd[i] = 0.0f;
	for (int i = 0; i < RV_LEN; i++) e.large->rv[i] = 0.0f;
}

static inline void reset_states(Engine& e) {
	e.on_drive = e.on_tilt = e.on_phaser = e.on_chorus = e.on_trem = e.on_delay = e.on_reverb = e.on_half = 0;
	e.clr_dl = e.clr_pd = e.clr_rv = -1;
	for (int n = 0; n < BLOCK; n++) e.M[n] = 0.0f;
	for (int n = 0; n < LSEG + BLOCK; n++) e.L[n] = e.R[n] = 0.0f;
	for (int i = 0; i < CH_LEN; i++) e.ch_buf[i] = 0.0f;
	e.lim_g0 = e.lim_B = 1.0f;
	e.ph_phase = 0.0f;
	e.ch_ps = 0.0f;
	e.ch_pf = 0.0f;
	e.tr_phase = 0.0f;
	e.rv_ph[0] = 0.0f;
	e.rv_ph[1] = 0.5f;
	e.ch_w = 0;
	e.dl_w = 0;
	e.rv_p = 0;
	e.pd_w = 0;
}

// a stage is turned on: start from a clean state at the current parameter values (no ramps)
static inline void start_drive(Engine& e) {
	e.dr_k = 0.2f;
	e.dr_mk = 5.0f;
	e.dr_b = 0.0f;
	e.dr_yb = 0.0f;
	e.dr_w = 0.0f;
	e.dr_dcx = e.dr_dcy = 0.0f;
}
static inline void start_tilt(Engine& e) {
	e.tl_gl = e.tl_gh = 1.0f;
	e.tl_lp = 0.0f;
}
static inline void start_phaser(Engine& e) {
	e.ph_a = phaser_coef(e, e.ph_phase);
	e.ph_y = 0.0f;
	for (int k = 0; k < 4; k++) e.ph_s[k] = 0.0f;
}
static inline void start_chorus(Engine& e) {
	for (int i = 0; i < CH_LEN; i++) e.ch_buf[i] = 0.0f;
	chorus_delays(e, e.ch_d);
	e.ch_hp = 0.0f;
	e.ch_in = 0.0f;
}
static inline void start_trem(Engine& e) { e.tr_gl = e.tr_gr = 1.0f; }
static inline void start_delay(Engine& e) {
	e.dl_T = e.dl_Told = clampf(e.s[P_DL_TIME] * 0.001f * e.sr2, 8.0f, 0.5f * DL_LEN - 8.0f);
	e.dl_xf = 1.0f;
	e.dl_hp = e.dl_lp = 0.0f;
	e.dl_in = 0.0f;
	e.dl_tc = dl_tone_coef(e, e.s[P_DL_TONE]);
	const float th = 0.125f * (1.0f + e.s[P_DL_PP]);
	e.dl_a = sin2pi(th);
	e.dl_b = sin2pi(0.25f - th);
	e.clr_dl = -1;
}
static inline void start_reverb(Engine& e) {
	e.pd_T = clampf(e.s[P_RV_PRE] * 0.001f * e.sr2, 0.0f, (float)(PD_LEN - 4));
	e.rv_lc = e.rv_dl = e.rv_dr = 0.0f;
	e.rv_in = 0.0f;
	rv_coefs(e, e.rv_k);
	e.rv_m[0] = (float)e.rv_len[RV_LAP1];
	e.rv_m[1] = (float)e.rv_len[RV_RAP1];
	e.rv_m[2] = (float)e.rv_len[RV_LD1];
	e.rv_m[3] = (float)e.rv_len[RV_RD1];
	e.clr_pd = e.clr_rv = -1;
}
static inline void start_half(Engine& e) {
	e.hb_dn = HbState();
	e.hb_upl = HbState();
	e.hb_upr = HbState();
}

// background clearing of a stopped stage's memory, a chunk per block
static inline void clear_step(Engine& e) {
	const int CHUNK = 1024;
	if (e.clr_dl >= 0) {
		uint32_t* b = (uint32_t*)e.fast->dl; // two samples per word
		const int end = e.clr_dl + CHUNK < DL_LEN / 2 ? e.clr_dl + CHUNK : DL_LEN / 2;
		for (int i = e.clr_dl; i < end; i++) b[i] = 0;
		e.clr_dl = end < DL_LEN / 2 ? end : -1;
	}
	if (e.clr_rv >= 0) {
		const int end = e.clr_rv + CHUNK < RV_LEN ? e.clr_rv + CHUNK : RV_LEN;
		for (int i = e.clr_rv; i < end; i++) e.large->rv[i] = 0.0f;
		e.clr_rv = end < RV_LEN ? end : -1;
	} else if (e.clr_pd >= 0) {
		const int end = e.clr_pd + CHUNK < PD_LEN ? e.clr_pd + CHUNK : PD_LEN;
		for (int i = e.clr_pd; i < end; i++) e.fast->pd[i] = 0.0f;
		e.clr_pd = end < PD_LEN ? end : -1;
	}
}

// clamp the live parameters (NaN -> min), smooth them toward their targets
static inline void update_controls(Engine& e) {
	const bool byp = e.p[P_BYPASS] >= 0.5f;
	const float a = e.smooth_a;
	for (int i = 0; i < NPARAM; i++) {
		const ParamInfo& pi = PARAMS[i];
		const float v = clampf(e.p[i], pi.d.min, pi.d.max);
		e.p[i] = v;
		const float t = (byp && pi.off != NO_OFF) ? pi.off : v;
		const float s0 = e.s[i];
		e.sp[i] = s0;
		float s = pi.smooth ? s0 + (t - s0) * a : t;
		if (fabsf(t - s) < 1e-4f * (pi.d.max - pi.d.min)) s = t;
		e.s[i] = s;
	}
}

static inline void update_stages(Engine& e) {
	const int dr = e.s[P_DRIVE] > 0.0f, tl = e.s[P_TILT] != 0.0f;
	const int ph = e.s[P_PH_MIX] > 0.0f, ch = e.s[P_CH_MIX] > 0.0f;
	const int tr = e.s[P_TR_DEPTH] > 0.0f, dl = e.s[P_DL_MIX] > 0.0f, rv = e.s[P_RV_MIX] > 0.0f;
	if (dr && !e.on_drive) start_drive(e);
	if (tl && !e.on_tilt) start_tilt(e);
	if (ph && !e.on_phaser) start_phaser(e);
	if (ch && !e.on_chorus) start_chorus(e);
	if (tr && !e.on_trem) start_trem(e);
	if ((dl || rv) && !e.on_half) start_half(e);
	if (dl && !e.on_delay) start_delay(e);
	if (rv && !e.on_reverb) start_reverb(e);
	if (!dl && e.on_delay) e.clr_dl = 0;
	if (!rv && e.on_reverb) {
		e.clr_rv = 0;
		e.clr_pd = 0;
	}
	e.on_drive = dr;
	e.on_tilt = tl;
	e.on_phaser = ph;
	e.on_chorus = ch;
	e.on_trem = tr;
	e.on_delay = dl;
	e.on_reverb = rv;
	e.on_half = dl || rv;
}

// ---------------------------------------------------------------------------------------------
// API

static inline void init(Engine& e, int sample_rate) {
	const float sr = (float)sample_rate;
	e.sr = sr;
	e.sr2 = 0.5f * sr;
	e.inv_sr = 1.0f / sr;
	e.smooth_a = 1.0f - exp2_(-1.4426950f * BLOCK / (0.02f * sr)); // 20 ms
	e.lim_rel = 1.0f - exp2_(-1.4426950f * LSEG / (0.15f * sr));   // 150 ms release
	e.dr_dc_r = 1.0f - 6.2831853f * 8.0f / sr;
	e.tl_c = onepole(700.0f, sr);
	e.ch_hp_c = onepole(120.0f, sr);
	e.dl_hp_c = onepole(100.0f, e.sr2);
	for (int i = 0; i < NPARAM; i++) e.p[i] = e.s[i] = e.sp[i] = PARAMS[i].def;
	e.guard_trips = 0;
	// reverb geometry: lengths at the half rate; shrink if the ring is too small for this rate
	float f = e.sr2 / 29761.0f * RV_SIZE;
	for (int iter = 0; iter < 20; iter++) {
		int total = 0;
		for (int k = 0; k < RV_NLINES; k++) {
			e.rv_len[k] = (int)(RV_LEN0[k] * f + 0.5f);
			e.rv_off[k] = total;
			const int extra = (k == RV_LAP1 || k == RV_RAP1 || k == RV_LD1 || k == RV_RD1) ? RV_EXC + 4 : 1;
			total += e.rv_len[k] + extra;
		}
		if (total <= RV_LEN) break;
		f *= 0.95f;
	}
	for (int k = 0; k < RV_NTAPS; k++) {
		e.rv_tl[k] = e.rv_off[RV_TAPS_L[k][0]] + (int)(RV_TAPS_L[k][1] * f + 0.5f);
		e.rv_tr[k] = e.rv_off[RV_TAPS_R[k][0]] + (int)(RV_TAPS_R[k][1] * f + 0.5f);
	}
	rv_accesses(e);
	reset_states(e);
	clear_buffers_now(e);
	update_stages(e);
}

static inline void process(Engine& e, const real* __restrict__ in, real* __restrict__ out_l, real* __restrict__ out_r) {
	update_controls(e);
	update_stages(e);
	float* L = e.L + LSEG;
	float* R = e.R + LSEG;
	for (int n = 0; n < BLOCK; n++) e.M[n] = clampf(in[n], -16.0f, 16.0f); // also maps NaN to a number
	if (e.on_drive || e.on_tilt) stage_drive(e);
	if (e.on_phaser) stage_phaser(e);
	stage_chorus(e, L, R);
	if (e.on_trem) stage_trem(e, L, R);
	if (e.on_half) stage_half(e, L, R);
	if (!stage_out(e, out_l, out_r)) {
		// guard: something went non-finite. Start over from silence (clears all memory at once).
		e.guard_trips++;
		reset_states(e);
		clear_buffers_now(e);
		for (int n = 0; n < BLOCK; n++) out_l[n] = out_r[n] = 0.0f;
		return;
	}
	clear_step(e);
}

} // namespace fx

namespace strisy {
template <> struct EngineParams<fx::Engine> {
	static constexpr int count = fx::NPARAM;
	static const ParamDesc* desc(int i) { return (i >= 0 && i < count) ? &fx::PARAMS[i].d : nullptr; }
	static float* ptr(fx::Engine& e, int i) { return (i >= 0 && i < count) ? &e.p[i] : nullptr; }
	static void changed(fx::Engine&, int) {} // process() clamps and smooths every block
};
}

#endif // FX_H_
