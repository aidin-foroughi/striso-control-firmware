// Stress performance for the va voice (same interface as performance.h), 17.5 s:
//  A 0.0-2.6  single note 69 (440 Hz), pressure swell 0.2..1.2, flat tilt (pitch check 0.6-2.4 s)
//  B 2.6-5.0  hard accents: 2 ms strikes cycling over all 6 voices, notes 40..100, short holds
//  C 5.0-7.5  full resonance (tilt -1), 3 low voices, fast pressure swells up to 1.4, pedal 1 (long tails)
//  D 7.5-10.5 6-voice chord at high pressure, fast tilt sweeps -1..1, shaking and gyro rotation
//  E 10.5-13  6 held voices jumping by large intervals every 120 ms, tilt varying
//  F 13.0-14.6 high note 103 alone at steady pressure, flat tilt (aliasing check 13.3-14.5 s)
//  G 14.6-15.8 note 96 with tilt +1 (maximum sync) (sync aliasing check 14.9-15.7 s)
//  H 15.8-17.0 everything at once: notes 20..127, pressure 1.5, hard strikes, tilt +-1, pedal, shaking
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48828
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

struct Ctl { float note, pres, vpres, but_x, but_y; };

static double gate(double t, double t0, double t1, double att, double rel) {
	if (t < t0) return 0;
	double a = pmin((t - t0) / att, 1.0);
	if (t < t1) return a;
	double r = 1.0 - (t - t1) / rel;
	return r > 0 ? a * r : 0;
}

static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	auto set = [&](int i, double note, double pres, double bx, double by) {
		if (i >= nv) return;
		v[i].pres = (float)pres;
		v[i].but_x = (float)bx;
		v[i].but_y = (float)by;
		v[i].note = (float)(note + 0.5 * bx * bx * bx);
		last_note[i] = v[i].note;
	};
	// A
	{
		double e = gate(t, 0.05, 2.5, 0.05, 0.1);
		if (e > 0) set(0, 69, e * (0.7 - 0.5 * cos(2 * M_PI * t / 2.4)), 0, 0);
	}
	// B: strikes every 0.1 s
	if (t >= 2.6 && t < 5.0) {
		int k = (int)((t - 2.6) / 0.1);
		double t0 = 2.6 + 0.1 * k;
		static const int nn[8] = {40, 64, 52, 88, 71, 100, 47, 76};
		double e = gate(t, t0, t0 + 0.04 + 0.02 * (k % 3), 0.002, 0.01);
		if (e > 0) set(k % 6, nn[k % 8], e * (0.3 + 0.7 * ((k * 37) % 10) / 9.0), 0, 0);
	}
	// C: full resonance, pedal 1 for long tails
	if (t >= 5.0 && t < 7.5) g[7] = 1.0;
	for (int i = 0; i < 3; i++) {
		double e = gate(t, 5.0 + 0.1 * i, 6.8, 0.03, 0.05);
		if (e > 0) set(i, 36 + 12 * i + (i == 2 ? 7 : 0), e * (0.75 + 0.65 * sin(2 * M_PI * 3.0 * t + i)), 0, -1.0);
	}
	// D: chord with tilt sweeps, shaking, rotation
	for (int i = 0; i < 6; i++) {
		static const int ch[6] = {43, 50, 55, 59, 62, 67};
		double e = gate(t, 7.5 + 0.02 * i, 10.3, 0.01, 0.2);
		if (e > 0) set(i, ch[i], e * (1.2 + 0.2 * sin(2 * M_PI * 1.1 * t + i)), 0.3 * sin(2 * M_PI * 5.5 * t + i),
		               sin(2 * M_PI * 1.5 * t + 1.1 * i));
	}
	if (t > 8.5 && t < 10.5) {
		double s = sin(2 * M_PI * 4 * t);
		g[0] = 1 + 2.5 * s * s * (s > 0);
		g[4] = 0.9 * sin(2 * M_PI * 0.9 * t);
		g[5] = 0.9 * sin(2 * M_PI * 1.3 * t + 1);
		g[6] = 0.9 * sin(2 * M_PI * 1.7 * t + 2);
	}
	// E: note jumps on held voices
	for (int i = 0; i < 6; i++) {
		double e = gate(t, 10.5, 12.9, 0.01, 0.1);
		if (e <= 0) continue;
		int k = (int)((t - 10.5) / 0.12) + 3 * i;
		static const int jj[7] = {0, 19, -12, 7, 24, -17, 5};
		set(i, 48 + 5 * i + jj[k % 7], e * (0.5 + 0.5 * ((k * 13) % 7) / 6.0), 0, sin(2 * M_PI * 0.8 * t + i));
	}
	// F: high note, steady
	{
		double e = gate(t, 13.0, 14.5, 0.01, 0.05);
		if (e > 0) set(0, 103, 0.9 * e, 0, 0);
	}
	// G: maximum sync
	{
		double e = gate(t, 14.6, 15.7, 0.01, 0.05);
		if (e > 0) set(1, 96, 1.0 * e, 0, 1.0);
	}
	// H: everything at once
	if (t >= 15.8 && t < 17.0) {
		static const int ex[6] = {20, 127, 24, 115, 120, 60};
		for (int i = 0; i < 6; i++) {
			int k = (int)((t - 15.8) / (0.07 + 0.02 * i));
			double t0 = 15.8 + (0.07 + 0.02 * i) * k;
			double e = gate(t, t0, t0 + 0.05, 0.001, 0.005);
			set(i, ex[(i + k) % 6], 1.5 * pmax(e, 0.2), 1.0 * sin(2 * M_PI * 7 * t + i), (k + i) % 2 ? 1.0 : -1.0);
		}
		g[7] = 1.0;
		double s = sin(2 * M_PI * 5 * t);
		g[0] = 1 + 3 * s * s;
		g[4] = g[5] = g[6] = (float)sin(2 * M_PI * 2 * t);
	}
}

#ifdef VA_FUZZ
// -DVA_FUZZ: random abrupt controls every block (some out of range) to check numerical safety
static void fuzz_controls(int b, Ctl* v, int nv, float* g) {
	static uint32_t r = 1;
	static float hold[NVOICES_MAX][5];
	auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (float)(r >> 8) * (1.0f / 16777216.0f); };
	for (int i = 0; i < nv; i++) {
		if (b == 0 || rnd() < 0.05f) {   // new random state for this voice
			float m = rnd();
			hold[i][0] = m < 0.05f ? -20.0f + 300.0f * rnd() : 127.0f * rnd();
			hold[i][1] = rnd() < 0.3f ? 0.0f : (rnd() < 0.05f ? 3.0f * rnd() : 1.6f * rnd());
			hold[i][3] = 2.0f * rnd() - 1.0f;
			hold[i][4] = rnd() < 0.1f ? 6.0f * rnd() - 3.0f : 2.0f * rnd() - 1.0f;
		}
		hold[i][0] += 0.3f * (rnd() - 0.5f);   // continuous wobble
		v[i] = {hold[i][0], hold[i][1] * (rnd() < 0.02f ? 0.0f : 1.0f), 0, hold[i][3], hold[i][4]};
	}
	g[0] = rnd() < 0.3f ? 4.0f * rnd() : 1.0f;
	g[1] = 2 * rnd() - 1; g[2] = 2 * rnd() - 1; g[3] = 2 * rnd() - 1;
	g[4] = rnd() < 0.5f ? 0.0f : 2 * rnd() - 1; g[5] = 2 * rnd() - 1; g[6] = 2 * rnd() - 1;
	g[7] = rnd();
}
#endif

#ifdef VA_DEMO
// -DVA_DEMO [-DVA_DEMO_TRANSPOSE=n]: a short musical phrase (12.5 s, then silence) for listening
#ifndef VA_DEMO_TRANSPOSE
#define VA_DEMO_TRANSPOSE 0
#endif
struct DNote { int voice; double t0, t1, att, rel, note, pres, tilt0, tilt1, vib; };
static const DNote demo[] = {
	// melody (D minor), mixed strikes and soft entries, vibrato on the long notes
	{0, 0.00, 0.40, 0.004, 0.08, 62, 0.75, 0, 0, 0},
	{1, 0.45, 0.70, 0.008, 0.06, 65, 0.65, 0, 0, 0},
	{0, 0.75, 1.05, 0.004, 0.08, 69, 0.85, 0, 0, 0},
	{1, 1.10, 2.40, 0.060, 0.25, 72, 0.80, 0, 0, 0.25},
	{0, 2.45, 2.70, 0.004, 0.06, 70, 0.70, 0, 0, 0},
	{1, 2.75, 3.60, 0.030, 0.35, 69, 0.75, 0, 0, 0.2},
	// chords with pressure swells and release tails
	{0, 3.80, 5.00, 0.150, 0.40, 50, 0.60, 0, 0, 0},
	{1, 3.85, 5.00, 0.150, 0.40, 57, 0.55, 0, 0, 0},
	{2, 3.90, 5.00, 0.150, 0.40, 62, 0.55, 0, 0, 0},
	{3, 3.95, 5.00, 0.150, 0.40, 65, 0.60, 0, 0, 0},
	{4, 5.10, 6.30, 0.020, 0.60, 46, 0.70, 0, 0, 0},
	{5, 5.10, 6.30, 0.020, 0.60, 53, 0.60, 0, 0, 0},
	{0, 5.12, 6.30, 0.020, 0.60, 58, 0.60, 0, 0, 0},
	{1, 5.14, 6.30, 0.020, 0.60, 62, 0.65, 0, 0, 0},
	// long note: tilt sweep back -> forward -> centre under a pressure swell
	{2, 6.80, 9.20, 0.300, 0.40, 57, 0.85, -1, 1, 0.15},
	// staccato riff low, strikes
	{3, 9.40, 9.52, 0.002, 0.03, 38, 0.80, 0, 0, 0},
	{4, 9.60, 9.70, 0.003, 0.03, 38, 0.50, 0, 0, 0},
	{5, 9.75, 9.87, 0.002, 0.03, 50, 0.90, 0, 0, 0},
	{3, 9.95, 10.05, 0.003, 0.03, 38, 0.50, 0, 0, 0},
	{4, 10.10, 10.25, 0.002, 0.03, 41, 0.85, 0, 0, 0},
	{5, 10.30, 10.40, 0.003, 0.03, 43, 0.60, 0, 0, 0},
	{3, 10.45, 10.75, 0.002, 0.05, 45, 0.95, 0, 0, 0},
	// final chord, struck, held and released
	{0, 11.00, 12.00, 0.003, 0.05, 50, 0.70, 0.2, 0.2, 0},
	{1, 11.00, 12.00, 0.003, 0.05, 57, 0.65, 0.2, 0.2, 0},
	{2, 11.00, 12.00, 0.003, 0.05, 62, 0.65, 0.2, 0.2, 0},
	{4, 11.00, 12.00, 0.003, 0.05, 66, 0.70, 0.2, 0.2, 0},
	{5, 11.00, 12.00, 0.003, 0.05, 69, 0.65, 0.2, 0.2, 0},
};
static void demo_controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	for (const DNote& n : demo) {
		if (n.voice >= nv || t < n.t0) continue;
		double e = gate(t, n.t0, n.t1, n.att, n.rel);
		if (e <= 0) continue;
		Ctl& c = v[n.voice];
		double u = (t - n.t0) / (n.t1 - n.t0);
		double swell = n.t1 - n.t0 > 1.0 ? 0.75 + 0.35 * sin(M_PI * pmin(u, 1.0)) : 1.0;
		c.pres = (float)(n.pres * e * swell);
		double vd = n.vib * pmin(pmax(u * 2 - 0.6, 0.0), 1.0);
		c.but_x = (float)(vd * sin(2 * M_PI * 5.2 * t));
		double tl = n.tilt0 == n.tilt1 ? n.tilt0 : (u < 0.5 ? n.tilt0 + (n.tilt1 - n.tilt0) * 2 * u : n.tilt1 * (2 - 2 * pmin(u, 1.0)));
		c.but_y = (float)tl;
		c.note = (float)(n.note + VA_DEMO_TRANSPOSE + 0.5 * c.but_x * c.but_x * c.but_x);
		last_note[n.voice] = c.note;
	}
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
}
#endif

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
#if defined(VA_FUZZ)
	fuzz_controls(b, v, nv, g);
	(void)controls;
#elif defined(VA_DEMO)
	demo_controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	(void)controls;
#elif defined(VA_TONE_NOTE)
	// -DVA_TONE_NOTE=n [-DVA_TONE_Y=y] [-DVA_TONE_PRES=p]: one steady held note (spectrum checks)
#ifndef VA_TONE_Y
#define VA_TONE_Y 0.0f
#endif
#ifndef VA_TONE_PRES
#define VA_TONE_PRES 1.0f
#endif
	(void)controls;
	for (int i = 0; i < nv; i++) v[i] = {69, 0, 0, 0, 0};
	v[0] = {(float)VA_TONE_NOTE, b > 10 ? (float)VA_TONE_PRES : 0.0f, 0, 0, (float)VA_TONE_Y};
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
#else
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
#endif
	for (int i = 0; i < nv; i++) {
		v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}

#ifdef VA_TEST_HOOK
// -DVA_TEST_HOOK [-DVA_LIVE_EVERY=n] [-DVA_LIVE_SEED=s] [-DVA_LIVE_PRESETS]: live parameter test.
// Called by va::process() every block: runs 10 voices (6..9 are played here, 0..5 by the
// performance), writes a random in-range value to a random live parameter every n blocks (the way
// the app does: *ptr = value, then changed()), and with VA_LIVE_PRESETS also switches presets
// now and then. On the first call it checks the parameter table: unique names, distinct pointers,
// every preset value inside its range.
#include <stdio.h>
#include <string.h>
#ifndef VA_LIVE_EVERY
#define VA_LIVE_EVERY 1
#endif
#ifndef VA_LIVE_SEED
#define VA_LIVE_SEED 1
#endif
namespace va {
static void va_test_hook(Engine& e) {
	typedef strisy::EngineParams<Engine> EP;
	static uint32_t r = 2654435761u * (uint32_t)VA_LIVE_SEED + 7u;
	static int b = 0;
	static float pold[4];
	auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (float)(r >> 8) * (1.0f / 16777216.0f); };
	if (b == 0) {
		e.nvoices = 10;
		int bad = 0;
		for (int i = 0; i < EP::count; i++) {
			const strisy::ParamDesc* d = EP::desc(i);
			for (int j = 0; j < i; j++)
				if (!strcmp(d->name, EP::desc(j)->name) || EP::ptr(e, i) == EP::ptr(e, j)) { fprintf(stderr, "!! duplicate param %s\n", d->name); bad++; }
			for (int k = 0; k < NUM_PRESETS; k++) {
				Params q = preset_params(k);
				float x = q.*(param_table[i].m);
				if (!(x >= d->min && x <= d->max)) { fprintf(stderr, "!! preset %s: %s = %g outside [%g, %g]\n", preset_name(k), d->name, x, d->min, d->max); bad++; }
			}
		}
		fprintf(stderr, "live params: %d, table check %s\n", EP::count, bad ? "FAILED" : "ok");
	}
	// voices 6..9: overlapping phrases with swells, strikes, tilt and note changes
	// (-DVA_LIVE_SOLO: not played, only the performance's voices sound)
	const double t = (double)b * BLOCK / SAMPLERATE;
#ifdef VA_LIVE_SOLO
	if (b == 0) e.nvoices = 6;
	for (int k = 0; k < 0; k++) {
#else
	for (int k = 0; k < 4; k++) {
#endif
		const int v = 6 + k;
		const double per = 0.55 + 0.17 * k;
		const int n = (int)(t / per);
		const double u = t - n * per;
		static const int nn[9] = {45, 52, 57, 60, 64, 67, 72, 76, 84};
		const float pres = (float)(u < per - 0.08 ? (u < 0.01 ? u / 0.01 : 1.0) * (0.5 + 0.4 * sin(2 * M_PI * 1.3 * t + k)) : 0.0);
		strisy::VoiceControls& c = e.vc[v];
		c.note = (float)nn[(n * 5 + k * 2) % 9] + 0.2f * (float)sin(2 * M_PI * 5 * t);
		c.vpres = pmax(-1.0f, pmin(1.0f, (pres - pold[k]) * (SAMPLERATE / BLOCK) * 0.0125f));
		c.pres = pres;
		c.but_x = 0;
		c.but_y = (float)sin(2 * M_PI * 0.4 * t + k);
		pold[k] = pres;
	}
	// live writes
#ifdef VA_LIVE_SWEEP
	// -DVA_LIVE_SWEEP: slider drags, one parameter at a time (0.3 s each, a write every 4 blocks):
	// from its preset value down to min, up to max, back to the preset value
	{
		static float orig = 0;
		const int seg = (int)(0.3 * SAMPLERATE / BLOCK);
		const int i = (b / seg) % EP::count, j = b % seg;
		const strisy::ParamDesc* d = EP::desc(i);
		float* p = EP::ptr(e, i);
		if (j == 0) orig = *p;
		if (j % 4 == 0) {
			const float u = (float)j / seg;
			float x = u < 0.25f ? orig + (d->min - orig) * u * 4 : u < 0.75f ? d->min + (d->max - d->min) * (u - 0.25f) * 2
			          : d->max + (orig - d->max) * (u - 0.75f) * 4;
			if (j + 4 >= seg) x = orig;
			if (d->kind == 2) x = u < 0.5f ? 1.0f - orig : orig;
#ifdef VA_LIVE_LOG
			if (j == 0) fprintf(stderr, "W %d %s %g\n", b, d->name, orig);
#endif
			*p = x;
			EP::changed(e, i);
		}
	}
#endif
	if (VA_LIVE_EVERY > 0 && b % (VA_LIVE_EVERY > 0 ? VA_LIVE_EVERY : 1) == 0) {
		const int i = (int)(rnd() * EP::count);
		const strisy::ParamDesc* d = EP::desc(i);
		const float a = rnd();
		float x = d->kind == 2 ? (a < 0.5f ? 0.0f : 1.0f) : a < 0.1f ? d->min : a < 0.2f ? d->max : d->min + (d->max - d->min) * rnd();
		if (d->kind == 1) x = (float)(int)(x + 0.5f);
#ifdef VA_LIVE_LOG
		fprintf(stderr, "W %d %s %g\n", b, d->name, x);
#endif
		*EP::ptr(e, i) = x;
		EP::changed(e, i);
	}
#ifdef VA_LIVE_PRESETS
	if (rnd() < 0.002f) set_preset(e, (int)(rnd() * NUM_PRESETS));
#endif
	if (b == (int)(17.5 * SAMPLERATE / BLOCK) - 1) fprintf(stderr, "guard resets: %d, active voices at end: %d\n", va_guard_resets, active_voices(e));
	b++;
}
} // namespace va
#endif

#endif
