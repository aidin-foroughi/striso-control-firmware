// Accordion test performance (30 s; the test renders the first 17.5 s): a 10-note chord at full
// pressure (the worst case for CPU), a chord held through a bellows reversal (pedal), fast strikes,
// one note at pp/mf/ff, the compass, microtonal notes and a bend. Deterministic, evaluated once per block.
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48000
#define BLOCK 32
#define NVOICES_MAX 15

struct Ctl { float note, pres, vpres, but_x, but_y; };

struct ANote { int voice; double t0, t1, att, rel, note, pres, glide_to; };

static const ANote anotes[] = {
	// 10-note chord at full pressure (all registers: the CPU worst case)
	{0, 0.2, 4.2, 0.020, 0.08, 36, 1.0, 0},
	{1, 0.2, 4.2, 0.020, 0.08, 43, 1.0, 0},
	{2, 0.2, 4.2, 0.020, 0.08, 48, 1.0, 0},
	{3, 0.2, 4.2, 0.020, 0.08, 52, 1.0, 0},
	{4, 0.2, 4.2, 0.020, 0.08, 55, 1.0, 0},
	{5, 0.2, 4.2, 0.020, 0.08, 60, 1.0, 0},
	{6, 0.2, 4.2, 0.020, 0.08, 64, 1.0, 0},
	{7, 0.2, 4.2, 0.020, 0.08, 67, 1.0, 0},
	{8, 0.2, 4.2, 0.020, 0.08, 72, 1.0, 0},
	{9, 0.2, 4.2, 0.020, 0.08, 76, 1.0, 0},
	// chord through a bellows reversal (pedal at 6.0 s)
	{0, 5.0, 7.5, 0.030, 0.10, 57, 0.6, 0},
	{1, 5.0, 7.5, 0.030, 0.10, 64, 0.6, 0},
	{2, 5.0, 7.5, 0.030, 0.10, 69, 0.6, 0},
	// fast strikes
	{3, 8.0, 8.12, 0.004, 0.02, 72, 0.9, 0},
	{3, 8.3, 8.42, 0.004, 0.02, 74, 0.9, 0},
	{3, 8.6, 8.72, 0.004, 0.02, 76, 0.9, 0},
	// A4 at pp, mf, ff (2 s each)
	{0, 9.0, 11.0, 0.030, 0.05, 69, 0.15, 0},
	{0, 11.5, 13.5, 0.030, 0.05, 69, 0.50, 0},
	{0, 14.0, 16.0, 0.030, 0.05, 69, 1.00, 0},
	// the compass at mf: C1 .. C8 (0.8 s each)
	{1, 16.5, 17.3, 0.020, 0.05, 24, 0.5, 0},
	{1, 17.5, 18.3, 0.020, 0.05, 36, 0.5, 0},
	{1, 18.5, 19.3, 0.020, 0.05, 48, 0.5, 0},
	{1, 19.5, 20.3, 0.020, 0.05, 60, 0.5, 0},
	{1, 20.5, 21.3, 0.020, 0.05, 72, 0.5, 0},
	{1, 21.5, 22.3, 0.020, 0.05, 84, 0.5, 0},
	{1, 22.5, 23.3, 0.020, 0.05, 96, 0.5, 0},
	{1, 23.5, 24.3, 0.020, 0.05, 108, 0.5, 0},
	// microtonal notes and a slow bend
	{2, 25.0, 26.0, 0.020, 0.05, 60.5, 0.5, 0},
	{2, 26.2, 27.2, 0.020, 0.05, 61.37, 0.5, 0},
	{2, 27.5, 29.5, 0.020, 0.05, 60, 0.5, 62},
};

static double agate(double t, double t0, double t1, double att, double rel) {
	if (t < t0) return 0;
	double a = (t - t0) / att;
	a = a < 1 ? a : 1;
	if (t < t1) return a;
	double r = 1.0 - (t - t1) / rel;
	return r > 0 ? a * r : 0;
}

static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	for (const ANote& n : anotes) {
		if (n.voice >= nv || t < n.t0) continue;
		double e = agate(t, n.t0, n.t1, n.att, n.rel);
		if (e <= 0) continue;
		Ctl& c = v[n.voice];
		double note = n.note;
		if (n.glide_to > 0) note = n.note + (n.glide_to - n.note) * (t - n.t0) / (n.t1 - n.t0);
		c.pres = (float)(n.pres * e);
		c.note = (float)note;
		last_note[n.voice] = c.note;
	}
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	if (t > 6.0 && t < 6.3) g[7] = 1.0f;
}

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	for (int i = 0; i < nv; i++) {
		float d = (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f;
		v[i].vpres = d < -1 ? -1 : (d > 1 ? 1 : d);
		pres_old[i] = v[i].pres;
	}
}

#endif
