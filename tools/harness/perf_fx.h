// Musical test performance for the master effects (same interface as performance.h), 17.5 s:
//  A 0.0-6.4   four 7th chords (F, Em, Dm, C), 1.6 s each, with a staccato melody on top
//  B 6.6-9.4   staccato single notes 0.45 s apart (echo / ping-pong test)
//  C 9.6-12.6  held Fmaj9 swelling (chorus / phaser / tremolo test), slight vibrato
//  D 12.6-17.5 silence: reverb and delay tails
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

struct Note { int voice; double t0, t1, note, pres, vib; };

static const Note notes[] = {
	// A: chords on voices 0-3
	{0, 0.00, 1.50, 53, 0.55, 0}, {1, 0.00, 1.50, 57, 0.50, 0}, {2, 0.00, 1.50, 60, 0.50, 0}, {3, 0.00, 1.50, 64, 0.50, 0},
	{0, 1.60, 3.10, 52, 0.55, 0}, {1, 1.60, 3.10, 55, 0.50, 0}, {2, 1.60, 3.10, 59, 0.50, 0}, {3, 1.60, 3.10, 62, 0.50, 0},
	{0, 3.20, 4.70, 50, 0.55, 0}, {1, 3.20, 4.70, 53, 0.50, 0}, {2, 3.20, 4.70, 57, 0.50, 0}, {3, 3.20, 4.70, 60, 0.50, 0},
	{0, 4.80, 6.30, 48, 0.55, 0}, {1, 4.80, 6.30, 52, 0.50, 0}, {2, 4.80, 6.30, 55, 0.50, 0}, {3, 4.80, 6.30, 59, 0.50, 0},
	// A: melody on voices 4/5
	{4, 0.40, 0.55, 72, 0.70, 0}, {5, 0.80, 0.95, 76, 0.70, 0}, {4, 1.20, 1.50, 77, 0.75, 0},
	{5, 2.00, 2.15, 74, 0.70, 0}, {4, 2.40, 2.55, 76, 0.70, 0}, {5, 2.80, 3.10, 79, 0.75, 0},
	{4, 3.60, 3.75, 77, 0.70, 0}, {5, 4.00, 4.15, 74, 0.70, 0}, {4, 4.40, 4.70, 72, 0.75, 0},
	{5, 5.20, 5.35, 71, 0.70, 0}, {4, 5.60, 6.20, 72, 0.70, 0.25},
	// B: staccato notes
	{0, 6.60, 6.68, 76, 0.80, 0}, {1, 7.05, 7.13, 79, 0.80, 0}, {2, 7.50, 7.58, 83, 0.80, 0},
	{3, 7.95, 8.03, 81, 0.80, 0}, {4, 8.40, 8.48, 76, 0.80, 0}, {5, 8.85, 8.93, 72, 0.80, 0},
	// C: held chord
	{0, 9.60, 12.40, 41, 0.50, 0.15}, {1, 9.65, 12.40, 53, 0.50, 0.15}, {2, 9.70, 12.40, 57, 0.45, 0.15},
	{3, 9.75, 12.40, 60, 0.45, 0.15}, {4, 9.80, 12.40, 64, 0.45, 0.15}, {5, 9.85, 12.40, 67, 0.45, 0.15},
};

static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	for (const Note& n : notes) {
		if (n.voice >= nv || t < n.t0) continue;
		const double rel = 0.12;
		double e = gate(t, n.t0, n.t1, 0.008, rel);
		if (e <= 0) continue;
		Ctl& c = v[n.voice];
		double swell = n.vib > 0 ? 0.8 + 0.4 * sin(2 * M_PI * 0.4 * (t - n.t0)) : 1.0;
		c.pres = n.pres * e * swell;
		c.but_x = n.vib * sin(2 * M_PI * 5.0 * t + 1.3 * n.voice) * pmin((t - n.t0) / 0.8, 1.0);
		c.but_y = 0;
		c.note = n.note + 0.5 * c.but_x * c.but_x * c.but_x;
		last_note[n.voice] = c.note;
	}
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
}

static void block_controls(int b, Ctl* v, int nv, float* g) {
	static float pres_old[NVOICES_MAX] = {0};
	controls((double)b * BLOCK / SAMPLERATE, v, nv, g);
	for (int i = 0; i < nv; i++) {
		v[i].vpres = pmax(-1.0f, pmin(1.0f, (v[i].pres - pres_old[i]) * (SAMPLERATE / BLOCK) * 0.0125f));
		pres_old[i] = v[i].pres;
	}
}

#endif
