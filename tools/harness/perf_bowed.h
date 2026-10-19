// Stress performance for the bowed string voice (voice_bowed.h): same interface as performance.h.
// Sections (17.5 s, the renderer's default length):
//  A 0-2.5      slow pressure swells 0.05 -> 1 -> 0.05 with vibrato, low and mid note together
//  B 2.5-5      pressure tremolo (8 Hz, 0.15..0.95), tilt sweeps -1..1, a third voice holding
//  C 5-6.5      extreme pressure: held at 1.5, and repeated instant 0 -> 1.4 attacks (vpres = 1)
//  D 6.5-8.5    fast strikes: 17 ms notes over the whole range on all voices, 2 ms attacks
//  E 8.5-11     note jumps on a held bow (20 <-> 115 and everything between), then fast glides
//  F 11-14.5    all 6 voices: extreme chord 20..115 at pressure 1.2, tilt extremes, shaking,
//               rotation, pedal 1; released all at once at 13.5 (pedal held: long ring)
//  G 14.5-16.5  bow bouncing: pressure flutter 0 <-> 1 at 30 Hz on all voices with random tilt
//  H 16.5-17.5  silence (tails, gating)
#ifndef PERFORMANCE_H_
#define PERFORMANCE_H_
#include <math.h>

#define SAMPLERATE 48828
#define BLOCK 32
#define NVOICES_MAX 15
#define pmax(x,y) ((x)>(y)?(x):(y))
#define pmin(x,y) ((x)<(y)?(x):(y))

struct Ctl { float note, pres, vpres, but_x, but_y; };

static double frac_(double x) { return x - floor(x); }
// deterministic pseudo random in [0,1) from an integer
static double hash01(int i) {
	unsigned int h = (unsigned int)i * 2654435761u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	return (h & 0xffffff) / 16777216.0;
}

static void controls(double t, Ctl* v, int nv, float* g) {
	static float last_note[NVOICES_MAX] = {69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69, 69};
	for (int i = 0; i < nv; i++) v[i] = {last_note[i], 0, 0, 0, 0};
	// globals: acc_abs, acc_x, acc_y, acc_z, rot_x, rot_y, rot_z, pedal
	g[0] = 1; g[1] = 0; g[2] = 0; g[3] = -1; g[4] = 0; g[5] = 0; g[6] = 0; g[7] = 3.0f / 16;
	auto set = [&](int i, double note, double pres, double bx, double by) {
		if (i >= nv) return;
		v[i].note = (float)(note + 0.5 * bx * bx * bx);
		v[i].pres = (float)pmax(pres, 0.0);
		v[i].but_x = (float)bx;
		v[i].but_y = (float)by;
		last_note[i] = v[i].note;
	};
	if (t < 2.5) {  // A
		double sw = 0.5 - 0.5 * cos(2 * M_PI * t / 2.5);
		double vib = 0.35 * sin(2 * M_PI * 5.5 * t) * pmin(t / 0.8, 1.0);
		set(0, 45, 0.05 + 0.95 * sw, vib, 0);
		set(1, 69, 0.05 + 0.95 * sw * sw, 0.35 * sin(2 * M_PI * 5.1 * t + 1), -0.3);
	} else if (t < 5.0) {  // B
		double u = t - 2.5;
		set(0, 62, 0.55 + 0.4 * sin(2 * M_PI * 8 * u), 0, 0);
		set(1, 50, 0.6, 0, sin(2 * M_PI * u / 1.5));
		set(2, 81, 0.5 + 0.2 * sin(2 * M_PI * 0.5 * u), 0.2 * sin(2 * M_PI * 6 * u), -sin(2 * M_PI * u / 1.1));
	} else if (t < 6.5) {  // C
		double u = t - 5.0;
		set(0, 57, 1.5, 0, 0.5);
		double ph = frac_(u / 0.25);
		set(1, 76, ph < 0.6 ? 1.4 : 0.0, 0, -0.5);
		set(2, 33, 1.5 * pmin(u / 0.01, 1.0), 0, 1.0);
	} else if (t < 8.5) {  // D
		double u = t - 6.5;
		int k = (int)(u / 0.05);
		double ph = u - k * 0.05;
		int vi = k % 6;
		double note = 20 + 95 * hash01(k);
		if (ph < 0.017) set(vi, note, ph < 0.002 ? ph / 0.002 * 0.95 : 0.95, 0, 2 * hash01(k + 1000) - 1);
		else set(vi, note, 0, 0, 0);
	} else if (t < 11.0) {  // E
		double u = t - 8.5;
		static const double seq[] = {20, 115, 40, 96, 60, 108, 25, 84, 52, 71, 115, 20, 66, 67, 43};
		if (u < 1.75) {
			int k = (int)(u / 0.15);
			set(0, seq[k % 15], 0.7, 0, 0);
			set(1, seq[(k + 5) % 15] - 0.5, 0.4 + 0.5 * hash01(k + 7), 0, 0.8);
		} else {
			double w = u - 1.75;
			set(0, 40 + 60 * w / 0.75, 0.8, 0, 0);       // fast glide up
			set(1, 100 - 70 * w / 0.75, 0.6, 0, -0.8);   // fast glide down
		}
	} else if (t < 14.5) {  // F
		double u = t - 11.0;
		static const double ch[6] = {20, 35, 52, 71, 90, 115};
		double rel = u < 2.5 ? 1.0 : 0.0;
		for (int i = 0; i < 6; i++)
			set(i, ch[i], 1.2 * rel * pmin(u / 0.03, 1.0), 0.9 * sin(2 * M_PI * 6.3 * u + i), (i % 2 ? 1.0 : -1.0));
		double s = sin(2 * M_PI * 4 * u);
		g[0] = 1 + 2.8 * s * s;
		g[4] = sin(2 * M_PI * 0.9 * u);
		g[5] = sin(2 * M_PI * 1.3 * u + 1);
		g[6] = sin(2 * M_PI * 1.7 * u + 2);
		g[7] = 1.0;
	} else if (t < 16.5) {  // G
		double u = t - 14.5;
		for (int i = 0; i < 6; i++) {
			double ph = frac_(u * 30 + i * 0.17);
			set(i, 30 + 13 * i, ph < 0.5 ? 1.0 : 0.0, 0, 2 * hash01((int)(u * 30) + 50 * i) - 1);
		}
	}
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
